# 15. Il port Switch di kabba536 e il renderer di Liberty Recompiled sulla base europea

Obiettivo (2026-10-20): capire se il renderer nativo di Liberty Recompiled
(`gta4_native`, già nel nostro albero) gira sulla base europea, come nel port
Switch di kabba536, che dichiara ~17 FPS in città a clock da dock. Il renderer
PM4 (N3) è in pausa, non cancellato.

Riferimento: `github.com/kabba536/Grand-Theft-Auto-IV-Native-Port-Nintendo-Switch`,
commit `dbd7288` (2026-10-19), GPL-3.0; base dichiarata
`vaduur/LibertyRecompAndroid` commit `7c97c58` ("android 0.5.9.2"); gioco USA
con Title Update 8 (la "v8" dei nostri documenti). Clone di sola lettura in
`<refs>/gta4-kabba536`. **Nulla di quel repository è stato copiato nel nostro**
(né l'NRO né file sorgente): sotto si citano solo nomi, commenti e numeri.

## 1. Lettura

### Controllo del codice di gioco (prima di tutto)

| File | Codice del gioco tradotto? |
|---|---|
| `source/gta4nx-switch.patch` (975 KB, 111 file, 18.587 righe aggiunte) | **sì, in un file**: `gta4-recomp/src/switch_audio_fast.cpp` (5.773 righe) è, per sua stessa intestazione, una copia "verbatim" dell'output del ricompilatore (`generated/gta4_recomp.74.cpp`, `.3.cpp`, `.4.cpp`) di sei funzioni della v8: `sub_82935BD0` (riverbero), `sub_82936CE0`, `sub_829321A0`, `sub_82934F00`, `sub_821997F8`, `sub_821B0310`, con 1.109 righe di disassemblato PowerPC nei commenti e 1.527 assegnazioni `ctx.rN = …`. Va trattato come codice del gioco: **non si copia**. Nessun altro file del patch ha corpi `PPC_FUNC`/`REX_FUNC` generati, disassemblato o tabelle di dati del gioco |
| resto del patch | indirizzi v8 di funzioni e globali (~200 righe con `0x82…`/`0x83…`), nomi `sub_…` nei commenti, cvar; una riscrittura **a mano** in NEON del mixer `0x82199BC8` (`gta4_native_hooks.cpp`, scritta da zero, non tradotta); due shader Bink HLSL scritti a mano (`LibertyRecompLib/shader_overrides/bink`). Nessun hash di file di gioco (gli unici valori a 64 bit sono costanti di bit) |
| `source/build_in_docker.sh` | pulito: clona LRA al commit, applica il patch, esegue lo script sotto, costruisce. Nota: il ramo LRA contiene già `gta4-recomp/generated/` (il codice ricompilato della v8, ~180 MB): il loro build lo usa così com'è |
| `source/build_nvk.sh` | pulito: ricostruisce mesa-switch (`danfromtico`, rev `0ff5207`) con tre modifiche a NAK/NVK (sotto) |
| `source/otimizar_codigo_gerado.py` | pulito: un passo sul codice generato **al momento del build** (chiamate dirette, barriere di ciclo, `dcbt` → prefetch); non contiene codice del gioco |
| `native.toml` | pulito: 41 cvar |

Il loro `README` chiede di non condividere la build fuori dal repository: non
la usiamo e non la copiamo.

### Cosa cambia, per categoria

Base di confronto: LRA `7c97c58` (il loro punto di partenza) e il nostro SDK
v0.9.0 con il livello Switch di `sync-clean` (`tools/switch-game`). Guadagni
come dichiarati da loro (log `sd_logsNN` loro, non verificati da noi).
Colonna "da noi": **applicabile** (così com'è), **adattabile** (va rifatto per
la base europea o per il nostro SDK), **USA/TU8** (legato agli indirizzi o al
codice della v8; per alcuni c'è l'equivalente europeo trovato col censimento
del §2).

#### CPU e codice generato

| Cambiamento | Guadagno dichiarato | Da noi |
|---|---|---|
| Chiamate dirette tra funzioni senza hook (`sub_X` → `__imp__sub_X`) | – (metodo di nfsmw-nx) | già fatto: `tools/switch-codegen/direct_calls.py`, 94 % di chiamate dirette |
| Memoria guest non `volatile` nel codice generato (`REX_GUEST_NONVOLATILE`) + `REX_LOOP_BARRIER()` su ogni salto all'indietro | – (consente al compilatore di tenere valori in registro) | adattabile: è un passo sul nostro codice generato (locale) + la macro in `gta4_init.h`; rischio: attese attive su valori scritti da altri thread, coperte dalla barriera. Da misurare |
| `dcbt`/`dcbtst` → `__builtin_prefetch` | riverbero: un load in attesa di memoria era il 6,5 % del thread audio | adattabile (stesso passo) |
| Flush-to-zero lasciato acceso (niente `msr fpcr` a ogni cambio VMX/FPU) | ~9 % di un core su quelle istruzioni, tutti i thread | adattabile: modifica a `rex/ppc/context.h`; cambia i risultati solo per i denormali scalari |
| `-O2` sul generato, `-O0` sul registratore, `REX_FUSED_VMADDFP=1` | memoria di GCC | in parte già nostro (06); `REX_FUSED_VMADDFP` da verificare |
| `switch_audio_fast.cpp`: sei funzioni DSP del gioco ricompilate a `-O3` senza `volatile` | thread audio 80–93 % di un core | **USA/TU8 e non copiabile**. Lo stesso effetto si può avere da noi con un'opzione per funzione nel generatore sul nostro codice europeo locale (riverbero EU `828C6740`, somiglianza 0,80, da verificare) |
| Mixer `0x82199BC8` riscritto in NEON (bit-identico) | 12,7 % di tutta la CPU in città | USA/TU8; equivalente EU `8216DBB0` (sequenza identica): la riscrittura è adattabile cambiando l'indirizzo |

#### Renderer (`gta4_native`)

Il loro renderer è quello di LRA, **più recente del nostro**: 89 file
diversi, `graphics_system.cpp` ~38.700 righe contro 34.450, 66 file che noi non
abbiamo. Metà delle impostazioni richieste per il punto 4 esiste solo lì (vedi
§2, cvar).

| Cambiamento | Guadagno dichiarato | Da noi |
|---|---|---|
| `gta4_native_frame_complete_signal = 2` (il titolo non aspetta che il registratore abbia inviato il fotogramma) | prima produttore ~45 ms + registratore ~45 ms in serie, ~11 FPS in città | non esiste nel nostro `gta4_native`: adattabile solo portando la versione LRA |
| `gta4_native_max_queued_frames = 3`, `gta4_extra_frames_in_flight = 1` (hook sull'attesa dello slot del titolo) | pipeline senza stadi in attesa l'uno dell'altro | l'hook di attesa è codice di gioco (LRA, `android_frame_wait_hook.cpp`): da ritrovare sull'europea |
| Resolve dello stesso formato come draw a schermo intero invece di `vkCmdCopyImage` (`gta4_native_resolve_draw_all_formats`) | resolve 19 ms di 76 (GPU), 11 di 33 a 720p: la copy engine serializza col 3D | adattabile (solo renderer); coerente con quanto visto in N2 sul nostro renderer PM4 |
| `gta4_native_identity_texture_views` | corregge rosso/blu scambiati su NVK (taxi e pelle blu) | applicabile (correttezza) se presente nel nostro |
| Allineamento UBO 256 di NVK → banchi di costanti via indirizzi di buffer | evita device lost ai primi fotogrammi | applicabile (correttezza) |
| Lotti di comandi più grandi, retain per lotto, salto del confronto per i buffer che cambiano sempre, cache dei nomi degli shader | produttore bloccato 15 %, `memcmp` 13 %, 2 % del registratore | adattabile (versione LRA) |
| Pipeline cache: rinomina su Horizon (non sovrascrive) | la cache restava ai primi 30 s: stutter a ogni filmato | applicabile |
| `std::bad_alloc` in upload → salta il draw | niente terminate dopo ~9 min | applicabile |
| `gta4_native_texture_lock_reuse = 4` | lock di texture prodotte dalla GPU = drenaggio completo della pipeline | adattabile |
| `gta4_far_clip_max` (hook sul timecycle `0x822CC388`, campo +68) | meno scena visitata (CPU e GPU) | USA/TU8; equivalente EU `822F5DE8` (sequenza identica) |
| Priorità: registratore e assemblatore a 45, thread di rendering del titolo a 44 (riconosciuto da 1.000 draw/s) | thread di rendering pronto ma non in esecuzione ~19 % | adattabile al nostro schema di priorità |

#### Memoria

| Cambiamento | Guadagno dichiarato | Da noi |
|---|---|---|
| Tetto della cache texture (`gta4_native_texture_memory_limit_mb`, mai 0 su Switch) | niente `bad_alloc` dopo minuti di guida | applicabile (cvar già nel nostro) |
| Sotto-allocazione delle texture in blocchi da 32 MB (`native_texture_memory_arena.h`) | 1,9 → 0,75 ms per texture (misura di nfsmw-nx) | adattabile: non c'è nel nostro `gta4_native` |
| Memoria fisica guest impegnata al primo accesso (non i 512 MB subito), letture da pagine protette emulate dall'ombra | il kernel rifiutava le allocazioni contando 5 viste (>2 GB) | il nostro livello Switch ha la sua soluzione (03-memory); da confrontare |
| Pagine in sola lettura fuori dalla memoria fisica date come lettura-scrittura | ogni lettura di costanti XEX passava dal gestore (2 µs) | da verificare sul nostro |
| Stack dei thread guest ridotti (16 MiB × ~30 = ~470 MB) | memoria | già fatto da noi (stack 2048 KiB, 06 §30) |
| Cache per thread davanti a `malloc` di newlib | contesa del lock unico | adattabile |

#### NVK (mesa-switch `0ff5207`)

| Cambiamento | Guadagno dichiarato | Da noi |
|---|---|---|
| NAK: `FloatCmpOp::flip` per `IsNum`/`IsNan` (simmetrici) invece di `panic` | senza: schermo nero con audio (migliaia di pipeline falliscono) | da verificare se la nostra NVK lo ha già; se no, applicabile |
| Latenza stimata di texture e memoria 32 → 200 cicli nello scheduler | da nfsmw-nx | applicabile (patch alla nostra Mesa) |
| `peephole_select` limite 0 → 8 (if piccoli senza salto) | da nfsmw-nx | applicabile |
| Revisione ABI del compilatore 10 → 11 (invalida la cache) | – | conseguenza delle due sopra |

#### Thread e priorità

| Cambiamento | Guadagno dichiarato | Da noi |
|---|---|---|
| Eventi e semafori senza mutex (parola atomica + arbitraggio degli indirizzi del kernel) | thread principale bloccato 57 % dei campioni in città | adattabile al nostro `threading_switch` (stessa origine nfsmw-nx) |
| Thread guest a 0x3B (time-sliced), servizi host sopra; thread principale del gioco a 46 | thread principale pronto ma fermo ~24 % | adattabile |
| Thread XAudio2 (entry `0x821909D0`) da solo sul core 3 | ~85 % di un core tolto ai core 0–2 | USA/TU8; entry EU `82169400` (sequenza identica). Richiede il core 3 nella maschera (che noi abbiamo: 0xF) |
| `yield` dei thread alzati → brevi sleep | lascia girare il proprietario di un lock a 0x3B | adattabile |

#### Streaming

Le cvar `gta4_streaming_budget_scale`, `gta4_streaming_preload_min/max`,
`gta4_streaming_prefetch_pending_limit` sono in `native.toml` ma **non nel
patch**: vengono da LRA. Sono hook sul codice di streaming del gioco (da
ritrovare sull'europea, non fatto). Dichiarazione loro: oltre 1,25 nessun
guadagno e memoria esaurita.

#### Audio

| Cambiamento | Guadagno dichiarato | Da noi |
|---|---|---|
| Uscita `audout` propria, un thread che mescola tutti i client, pompa a tempo fisso (`audio_switch_bomba`) | blocchi da 1.024 campioni, ~64 ms di latenza | noi abbiamo l'audio nativo (06 §30): da confrontare |
| Decodifica XMA asincrona sul thread del decoder | ~20 % del thread audio | adattabile (SDK) |
| Reset morbido del decoder WMA Pro invece di `avcodec_open2` | – | adattabile |
| `gta4_audio_reverb = false` (salta il riverbero) | metà del riverbero era attesa di memoria | USA/TU8 (hook sul riverbero); EU `828C6740` da verificare |

#### Altro

Input HID diretto (senza SDL), vibrazione inviata solo se cambia (5,8 % di un
thread), log errori subito su SD, SaltyNX/Reverse-NX e sys-clk per FPS e clock,
installatore dalla cartella `install/` (ISO o cartella + TU). Tutto lato SDK:
adattabile, nessun indirizzo di gioco.

### Licenza e crediti

Il port è GPL-3.0 come LibertyRecomp e LRA. Crediti dichiarati: Liberty
Recompiled (OZORDI), LibertyRecompAndroid (vaduur), ReXGlue (Tom Clay) da Xenia,
mesa-switch (danfromtico, NaGaa95), nfsmw-nx (StevensND, riferimento chiave),
switchbrew/devkitPro, FFmpeg. Se prendiamo idee (non file), il credito va a
kabba536 e, per quelle che lui attribuisce a nfsmw-nx o MarathonRecomp-NX, a
quei progetti. `switch_audio_fast.cpp` non si può prendere comunque.

## 2. Censimento degli hook sulla base europea

Strumento: `tools/gta4_native_eu_census.py` (tabella in
`out/gta4-native-census/census.tsv`). Legge gli indirizzi v8 dai sorgenti del
renderer e degli hook (esclusi commenti e stringhe), poi confronta il codice
generato v8 (locale, fuori dal repository) con il nostro europeo, funzione per
funzione, su sequenze di istruzioni normalizzate (destinazioni dei salti come
offset, chiamate e indirizzi costruiti con `lis` mascherati, offset delle
strutture mantenuti):

1. sequenza identica e unica → **trovato**; identica ma ripetuta, e una sola
   copia nel posto indicato dai vicini → **trovato (posizione)**;
2. altrimenti somiglianza a 4 istruzioni (≥ 0,85) o, se l'europea alloca i
   registri diversamente, sui soli mnemonici (6 istruzioni, ≥ 0,80) →
   **trovato (simile)**; 0,5–0,8 → **ambiguo**; sotto → **da trovare**;
3. globali e indirizzi interni (siti di chiamata, indirizzi di ritorno):
   attraverso l'istruzione che li costruisce nella funzione corrispondente
   (stesso indice, o allineamento sui mnemonici ±8 istruzioni con ≥ 15/17).

Nota: le due build collegano le unità di traduzione in ordine diverso. Sopra
v8 `0x82790000` (motore e librerie) lo scarto è quasi costante
(`0x6E000`–`0x72000`; **`0x714A0` esatto per tutta la libreria D3D**), sotto è
rimescolato; per questo la posizione attesa viene dai vicini unici (20.439
coppie uniche), non da uno scarto globale.

### Risultati (gruppo "nativo": `gta4_native_hooks.cpp` e i suoi `.inc`, `src/graphics/gta4_native`)

| Ruolo | Hook | trovati | non trovati |
|---|---|---|---|
| disegno, libreria D3D | 37 | **37** (tutti = v8 − 0x714A0, confermati dalla sequenza) | 0 |
| disegno, codice di gioco (risoluzione, passi, postfx, ombre, riflessi, luci, font, TV, sole) | 40 | **39** | 1: `8247BED8` (TV; da trovare) |
| LOD e distanza (`gta4_force_highest_lod`, `gta4_draw_distance_scale`, limite dei drawable) | 3 | 3 | 0 |
| tracciamento (phone, help, fade, bulb, cloud, offscreen, luci) | 49 | 46 | 3 ambigui (`821F32F8` ×2, `82223CF8`, `828DFF00` ×2) |
| **totale hook** | **129** | **125** | 4 |
| chiamate ad altre funzioni (D3D 11, gioco 8) | 19 | 19 | 0 |

Quindi: **hook di disegno 76 su 77**, hook in totale 125 su 129 (i "circa 135"
del piano: 129 hook + le chiamate). Le "~57 funzioni che non sono nei
sorgenti" del piano 06 §11 b non esistono come tali: il conteggio di allora
includeva solo la libreria D3D; il resto sono gli hook di gioco qui sopra.

Globali e indirizzi interni del gruppo nativo: 70 + 68 trovati, 14 no. Quelli
che toccano il disegno:

- i tre proprietari dei font vettoriali (`0x82B99B9C`, `…D90`, `…F84`): trovati
  **a mano** (oggetti font da 500 byte a `0x82B8DE78`, proprietario a +476):
  `82B8E054` (confermato da due funzioni), `82B8E248` e `82B8E43C` (**da
  verificare**, stesso passo);
- `kEaaParameterUploadReturnAddress` (`822D07E0`): allineato 17/17 a `822E3B28`
  ma in una funzione ambigua: lasciato non tradotto; se non combacia l'hook
  `828C8A50` prende il percorso originale (nessun danno, nessun effetto);
- `kMainPresentCaller` (`828BF4B0`): solo un'etichetta diagnostica.

Il resto (12) è tracciamento.

Fuori dal gruppo nativo (input, popolazione, multiplayer, frontend, ecc.):
523 funzioni trovate, 106 ambigue, 35 da trovare. Popolazione: 4 su 7
(i setter di veicoli `82598ED0`/`82598EE8` sono sequenze ripetute ×6).

### Shader: la cache USA vale per l'europea?

Il renderer cerca ogni shader nella sua cache per XXH3 del contenitore guest.
Misura sul PC (`tools/pc-eu-base`, cvar nuova `pc_shader_hash_log`, run di 5,5
minuti fino in partita, `out/pc-runs/kabba-shaders/shader-hashes.csv`):
**1.274 shader creati dall'europea, 1.250 (98,1 %) con lo stesso hash di una
voce della cache costruita dalla v8**; 24 mancano (18 pixel, 6 vertex). Degli
18 override HLSL del nostro `gta4_native`, 11 hanno un hash presente
nell'europea. Uno shader mancante non viene registrato (riga d'errore) e i
draw che lo usano saltano: niente blocco, al più un pezzo mancante.

### Cvar `gta4_*` di `native.toml`: codice di gioco o solo renderer

| Cvar | Dipende da | Nel nostro `gta4_native`? |
|---|---|---|
| `gta4_traffic/parked_car/ped/scenario_ped_density_scale` | codice di gioco (hook di popolazione: 4 su 7 trovati) | sì (`gta4_population_hooks.cpp`) |
| `gta4_draw_distance_scale` | codice di gioco (hook `821DFFE8`, trovato) | sì |
| `gta4_dynamic_draw_distance`, `…_min` | codice di gioco | no (LRA) |
| `gta4_far_clip_max` | codice di gioco (timecycle `822CC388` → EU `822F5DE8`) | no (patch) |
| `gta4_shadow_cascade_count` | codice di gioco (configurazione delle cascate) | no (LRA) |
| `gta4_shadow_distance_scale` | codice di gioco | sì |
| `gta4_native_shadow_interval`, `…_min_indices` | renderer + riconoscimento dei passi (hook di fase, trovati) | no (LRA) |
| `gta4_native_water/environment_reflection_interval`, `…_min_indices` | renderer + riconoscimento dei passi | no (LRA) |
| `gta4_reflection_capture_distance` | codice di gioco (hook `828BE580`, trovato) | sì |
| `gta4_reflection_aa`, `gta4_native_upscaler`, `gta4_fsr1_quality`, `gta4_native_anti_aliasing` | solo renderer | sì |
| `gta4_motion_blur` | renderer (override del composito) | no (LRA) |
| `gta4_native_texture_memory_limit_mb` | solo renderer | sì |
| `gta4_native_texture_suballocation`, `gta4_native_buffer_change_streak`, `gta4_native_resolve_draw_all_formats`, `gta4_native_max_queued_frames`, `gta4_native_frame_complete_signal` | solo renderer | no (patch/LRA) |
| `gta4_extra_frames_in_flight`, `gta4_frame_wait_sleep` | codice di gioco (attesa dello slot del titolo) | no (LRA) |
| `gta4_streaming_budget_scale`, `gta4_streaming_preload_min/max`, `gta4_streaming_prefetch_pending_limit` | codice di gioco (streaming) | no (LRA) |
| `gta4_present_mode` | presentazione (app) | sì |
| `gta4_audio_reverb` | codice di gioco (riverbero) | no (patch) |
| `gta4_profile_native_detailed_cpu/gpu` | solo renderer | sì |
| `switch_*` (priorità, core 3) | SDK; `switch_core3_thread_entry` è un indirizzo di gioco | no |

## 3. PC (stato al 2026-10-20, lavoro interrotto)

Build: `tools/pc-eu-base` con `-DTHEFT4_GTA4_NATIVE=ON
-DTHEFT4_GTA4_NATIVE_PRIVATE_ROOT=<cartella locale con shader/shader_cache.cpp e
font_atlases>` (più `GTA4_NATIVE_DXC_ROOT` = DXC 1.9: il DXC 1.8 incluso va in
crash sugli shader Bink; `GTA4_NATIVE_SPIRV_VAL`). Gli hook sono copie tradotte
(`tools/gta4_native_eu_translate.py` → `out/gta4-native-eu/src`), le chiamate
verso gli indirizzi agganciati restano deboli (`direct_calls.py`, 94,0 %
dirette). Correzione al nostro `gta4_native`: `native_worker_batch.h` incluso
sempre (prima solo con `THEFT4_LAB_BUILD`, non compilava).

Esito (`out/pc-runs/kabba-native-3`, `-4`, `--gpu_plugin=gta4-native`):
**il filmato iniziale esce riconoscibile e corretto** (Niko al molo, titoli,
luci), nessun crash in 270 s; 24 shader mancanti segnalati, nessun effetto
visibile nelle catture. Le catture vanno fatte con `PrintWindow`
(`PW_RENDERFULLCONTENT`): `CopyFromScreen` restituisce un'immagine vecchia.

Da fare: la finestra resta 1920×1080 nonostante `--window_width/height`
(causa da trovare), quindi FPS a 640×360 non ancora misurati; immagini
affiancate con l'emulato da rifare con `PrintWindow`; confronto a pari
impostazioni (`gta4_force_highest_lod=false`, `gta4_draw_distance_scale=1.0`).

## 4. Switch: non iniziato

Ostacolo noto: la nostra NVK (`libvulkan.a` dei portlibs) contiene ancora il
`panic!("Cannot flip unop")` di NAK che, secondo kabba536, fa fallire migliaia
di pipeline degli shader di GTA IV: serve ricostruire mesa-switch con la loro
correzione (`<refs>/CBO1-NX/mesa-switch-main` ha un percorso MSYS2). Metà delle
impostazioni chieste (cascate, intervalli di ombre e riflessi, motion blur,
sotto-allocazione) esiste solo nel `gta4_native` più recente di LRA.
