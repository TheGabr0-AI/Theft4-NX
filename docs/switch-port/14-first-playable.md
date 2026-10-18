# 14. Prima build giocabile: definizione, divario, leve

Nessuna implementazione: cosa vuol dire "giocabile", quanto manca coi numeri
di oggi (run 30, build `7c94eee`, 06 §30) e quali leve ci sono, ciascuna
misurabile con una sola variabile in un avvio.

## Definizione

Una build è **giocabile** quando, su un avvio con la cache del run
precedente e Horizon OC spento o col profilo di default (`run-protocol.md`):

| Requisito | Soglia | Come si legge nel log |
|---|---|---|
| clock | standard: portatile apm `0x92220008` (CPU 1020 / GPU 460,8 / memoria 1331,2); in dock 1020 / 768 / 1600 vale come seconda misura, non al posto della prima | schermata di avvio `clocks read` = `clocks wanted`; righe `clocks:` costanti |
| fotogrammi | **≥ 15 FPS stabili** a piedi **e** in macchina: ≤ 66,7 ms per fotogramma; nessuna finestra da 10 s sotto 13 swap/s | riga `vulkan: … presents (N/s, frame X ms)` del battito, una finestra per volta |
| audio | **senza scatti**: 100 % del tempo reale, "ran dry" 0 (al più qualche unità per finestra) | riga `audio: … frames (P% of real time), output ran dry N` |
| stabilità | **5 minuti in partita senza crash**, uscita con + | `game-exit.log` con i passi del modo D, nessun record di fault non gestito |

## Il divario coi numeri di oggi (run 30, 1020 / 460,8 / 1331,2, in partita)

| | Oggi | Traguardo | Fattore |
|---|---|---|---|
| ms per fotogramma | ~640 (1,4–1,6 swap/s) | ≤ 66,7 | **÷ 9,6** |
| GPU per fotogramma | ~395 ms | ≤ ~60 ms (lasciando margine al resto) | ÷ 6,6 |
| attesa seriale CPU ↔ GPU | ~440 ms + ~157 ms di CPU del command processor | sovrapposte: max(GPU, CPU) | – |
| CPU del command processor | ~157 ms (38 µs × ~4.100 draw) | ≤ ~50 ms | ÷ 3 |
| Main XThread | 81–87 % di un core a 1,5 FPS | a 15 FPS il lavoro per fotogramma va moltiplicato per 10: **da misurare** quanto di quell'84 % è attesa attiva | – |
| audio | 37–43 % del tempo reale, mixer (id 18) ~79 % di un core | 100 % | **× 2,5** di velocità del mixer |
| memoria | margine minimo 345 MiB su 85 s | ≥ ~150 MiB su 5 minuti | ok oggi, da verificare su 5 minuti |
| stabilità | uscita pulita a 280 s, mai 5 minuti in partita con questa build | 5 minuti | da verificare |

Nessuna leva da sola chiude il divario: la GPU va divisa per ~6–7, il mixer
moltiplicato per ~2,5, e CPU e GPU vanno sovrapposte.

## Leve, ordinate per costo e beneficio stimato

Ogni riga: una variabile, un avvio, la riga del log che la misura. "Stima" =
da verificare; il beneficio è riferito al fotogramma di oggi (~640 ms).

| # | Leva | Dove si cambia | Cvar esistente? | Costo | Beneficio stimato | Come si verifica |
|---|---|---|---|---|---|---|
| 1 | **Diagnostica spenta**: `nx_trace_exports = false`, `nx_log_debug = false`, `nx_vk_capture_frames = false` (un avvio per ciascuna, o tutte e tre in un avvio come "profilo di gioco") | toml | **sì** (tutte) | nullo | piccolo sul fotogramma; sul mixer ~4–5 % (il tracciamento costa ~37 ms/s al thread 18, 06 §30 C.3) | righe `cpu` (id 18), `audio:` (% del tempo reale), `vulkan:` |
| 2 | **GPU in dock** (768 MHz, memoria 1600) | dock, nessun file | sì (`nx_apm_clock`, configurazione di sistema) | nullo | dal modello di 06 §30 (GPU ms × MHz ≈ costante): ~395 → ~237 ms di GPU, fotogramma ~640 → ~390 ms (il run 29 a 1785/768/1600 faceva ~385) | `clocks read`, `vulkan:` |
| 3 | **Tetto dei trasferimenti di ownership**: `nx_ownership_transfer_skip = 1` (depth↔colore) e `= 2` (tutti), o `nx_ab_transfer_modes = true` (le tre fasi in un avvio) | toml | **sì** (misura: l'immagine viene sbagliata) | nullo | i trasferimenti sono il 49 % della GPU (194 ms): il tetto del guadagno se si evitassero | `[GpuOperationTime] kind=ownership_*`, `vulkan:`, riga `[ab]` |
| 4 | **Tetto della CPU**: `nx_ab_skip_cycles = 2` (fasi a GPU spenta) | toml | **sì** | nullo | dice quanto andrebbe il gioco con la GPU gratis (DevZer0D4Y: 10,9 → 15,7 swap/s a GPU spenta, su un'altra build): separa limite CPU e limite GPU | riga `[ab] window …: GPU work off` |
| 5 | **Core 3 per i thread host** (GPU Commands, XMA sul core 3, `nx_host_worker_core_mask`) | `threading_switch.cpp` / creazione dei thread host | no (API c'è, uso no; serve una cvar) | basso | libera ~45 % dei core 0–2 per Main e mixer; sul fotogramma da verificare | righe `cpu` e `audio:` |
| 6 | **Sovrapporre CPU e GPU** (togliere l'attesa seriale di ~440 ms): capire chi aspetta cosa (resolve riletti, `nx_gpu_guest_progress_wait`, command buffer in volo) | `vulkan/command_processor.cpp` (`CheckSubmissionFenceAndDeviceLoss`, `EndSubmission`), ReXGlue | in parte (`nx_gpu_guest_progress_wait` accesa ora; nessuna cvar per i buffer in volo) | medio | fino a ~157 ms per fotogramma (la CPU del command processor sparisce dentro il tempo GPU): ~640 → ~470 ms | riga `vulkan time … blocking fence waits`, `command processor phases` (build 31) |
| 7 | **Costo del mixer audio** (thread 18): profilo per funzione con `nx_profile = true`, poi la leva che il profilo indica (export IRQL/spin lock più leggere o mixer nativo) | profilo: toml; correzione: export del kernel (`xboxkrnl_threading.cpp`) o hook sul mixer | profilo **sì**; correzione no | profilo nullo, correzione medio–alto | serve × 2,5 per l'audio continuo; libera fino a ~80 % di un core | `where.py --profile`, `audio:` |
| 8 | **Draw per fotogramma** (~4.100–6.200): oggi 38 µs l'uno di CPU del command processor; leva = ridurre il costo per draw (le fasi della build 31 dicono dove) | ReXGlue, `IssueDraw` | no | medio | a 15 FPS servirebbero ≤ ~1.300 draw a 38 µs, o ≤ 12 µs per draw con 4.100 | `command processor phases` |
| 9 | **Effetti e passate di post-processing**: individuare le passate a schermo intero (`nx_frame_structure_capture = true`, cvar esistente) e misurarle; spegnerle richiede o l'opzione del gioco (sulla 360 c'è un'impostazione "Definition" nelle opzioni video: **da verificare** cosa toglie) o saltare i draw di un dato shader (cvar da aggiungere) | toml (cattura), menu del gioco, o nuova cvar sul command processor | cattura **sì**; salto no | basso (cattura) / medio (salto) | oggi non separabile dai timestamp (dentro il 18,6 % di `guest_render`) | `[NativeOracleDraw]`, poi `[GpuOperationTime] kind=guest_render` con e senza |
| 10 | **Distanza di visuale e densità** (pedoni, traffico, LOD) | variabili del gioco: sulla 360 non sono opzioni; vanno trovate nell'eseguibile europeo senza TU (gli indirizzi delle patch note per altre versioni non valgono) e cambiate con un hook nel codice generato | no | alto (ricerca degli indirizzi) | riduce draw (8) e GPU; non stimabile prima di trovare le variabili | `vulkan:` draw per fotogramma, `[GpuOperationTime]` |
| 11 | **Risoluzione interna** | il gioco disegna a 1280×720 (`guest_output_size=1280x720`); `draw_resolution_scale_x/_y` esistono ma vanno da 1 a 8 (**solo in su**) | per scendere **no** | alto: o un hook nel gioco sulle dimensioni dei render target (stessi vincoli della 10), o il renderer nativo | fill e trasferimenti EDRAM scalano coi pixel (−44 % a 960×540, −75 % a 640×360, stima) | `[GpuOperationTime]`, `vulkan:` |
| 12 | **Renderer nativo senza emulazione dell'EDRAM** (deko3d o Vulkan, `13-deko3d-spike.md`) | nuovo | no | molto alto (06 §11 c: 25–40 sessioni) | toglie il 64,5 % del tempo GPU di oggi (ownership + resolve + compute EDRAM) e la serializzazione | tutto il battito |

**Vincoli dell'edizione europea senza TU:** gli hook servono solo per le leve
10 e 11 (variabili e dimensioni del gioco) e per un mixer nativo (7): gli
indirizzi vanno cercati nell'eseguibile `default.xex` europeo (SHA256
`74d480d2…`, quello del codice generato), non presi da patch fatte per altre
versioni. Le leve 1–6, 8, 9 (cattura) e 12 non toccano il gioco.

**Ordine consigliato** (costo crescente, ciascuna un avvio): 1 → 2 → 4 → 3 →
profilo della 7 → 5 → 6 → 8 → 9 → 7 (correzione) → 10/11 → 12. Le prime
quattro non richiedono codice e dicono quanto vale ognuna delle altre.
