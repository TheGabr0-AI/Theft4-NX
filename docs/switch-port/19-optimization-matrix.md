# Ottimizzazione ReNut su Switch: matrice e risultati

Documento vivo della fase di ottimizzazione iniziata dopo il run 70 (2026-11-11), il primo con la scena differita
corretta sulla console. Obiettivo dichiarato dall'utente: 30 fps a 720p, qualità equivalente alla Xbox 360, clock
standard (CPU 1020 / GPU 460,8 / memoria 1331,2 MHz), nessun overclock. **Niente qui è "risolto" senza un run sulla
console che lo misuri** (`09-rules.md` §8).

Regole di lavoro (dall'utente, 2026-11-11): baseline prima del codice; una modifica attribuibile per volta (o gruppi
indipendenti); dopo ogni build test PC, NRO, verifica ELF/simboli/shader, test console breve, confronto con la
baseline, documentazione; cache `deko3d_shaders.bin` e `pipeline_list.bin` conservate tra i run; nessuna riduzione di
risoluzione, geometria, traffico o distanza visiva.

## 1. Baseline: run 70 (`cc64728`, `out/console-runs/70-logs`)

Clock letti uguali a quelli voluti per tutto il run; cache calda (367 varianti DKSH, 538 pipeline in lista); 710 s,
0 fault. **Il run aveva il trace dei resolve acceso** (`banjo_renut_diag_resolve_trace`): un frame ogni 30 s con
letture di ritorno sincrone. Le finestre che contengono un `[trace] frame start` durano 17-21 s a 2-4 fps: **i blocchi
di diversi secondi visti dall'utente sono in gran parte la misura**, non il gioco. I numeri sotto sono delle finestre
senza trace.

| Misura (gameplay, finestre senza trace) | Valore |
|---|---|
| fps | 12,9-14,5 |
| GPU per frame | 67-75 ms (55-62 nella 69 con l'illuminazione nera: ora lavora) |
| Render thread "busy" per frame | 116-143 ms (somma dei lavori meno le attese registrate: include attese non contate, da chiarire col profilo) |
| Draw per frame / draw al secondo | 580-990 / 7,6-13,1 k (conteggio di ReNut, non confrontabile con i ~4.000 del vecchio recorder) |
| Compilazioni uam | quasi zero (cache calda) |
| Audio | 62-70 % del tempo reale, l'uscita va a secco (fino a 83 volte in 10 s): robotico e rallentato **anche fuori dal trace** |
| Memoria | heap libero 1.325 MiB alla fine, nessuna pressione |
| Thread | tutti maschera 7 (core 0-2); core 3 inutilizzato; render thread fisso sul core 1 (priorità 0x2C); lavori di sfondo di ReNut sul core 2 |

Mancano alla baseline il **tempo CPU per thread e per funzione** (`nx_profile` spento) e il **tempo GPU per passata**
(`renut_gpu_profile` spento): li raccoglie il run 71 (sotto).

Confronto con il vecchio renderer (runs 59-62, clock standard): 5,3-5,8 fps, GPU 120 ms + 50-70 ms di attesa a frame.
ReNut nella 70: 13-14,5 fps, GPU 67-75 ms, frame completo.

## 2. Matrice

Categorie: **D** trasferibile direttamente; **A** con adattamenti a ReNut/plume/deko3d; **N** specifica di un'altra
architettura, non applicabile; **R** non utile o potenzialmente regressiva. Stato: *fatto* (già nel nostro albero),
*pronto* (fattibile ora, prove disponibili), *profilo* (da decidere col profilo del run 71), *dopo*.

### 2.1 Stalli e stutter

| Tecnica | Origine (file) | Cat. | Costo | Beneficio atteso | Verifica | Stato |
|---|---|---|---|---|---|---|
| Trace dei resolve spento nei run di gioco | nostro (`game.toml`) | D | nullo | via i blocchi di 10-15 s ogni 30 s | finestre senza `[trace]`, catture regolari | **run 71** |
| Cache DKSH calda conservata | ReNut (`banjo_renut_deko3d.cpp`) | D | nullo | 295 -> 5 compilazioni (run 68 -> 69) | `uam N compiles` | fatto |
| Cache DKSH costruita sul PC dalla lista delle pipeline | rr6-nx `tools/deko3d_variant_cache.py` (stesso formato `BDKC`, stesso lignaggio banjo-nx) | A | basso (tag del compilatore, testi GLSL del nostro pacchetto, uam del PC) | nessuna compilazione anche a scheda pulita | `DKSH cache N variants` al primo avvio, 0 compilazioni | pronto (serve `pipeline_list.bin` dalla scheda) |
| Scritture su SD dai thread critici | torchlight-nx `DECOUVERTES` §1.7 | A | basso | blocchi da 94 ms a 1,1 s (loro) | nostro log: bufferizzato da un thread suo; pipeline list: chi la scrive? | profilo |
| Mode boost che abbassa la GPU a 76,8 MHz | torchlight §1.8 | R/N | - | - | il nostro `nx_apm_clock` tiene 460,8 (righe `clocks`) | non pertinente |

### 2.2 CPU (codice del gioco)

| Tecnica | Origine (file) | Cat. | Costo | Beneficio atteso | Verifica | Stato |
|---|---|---|---|---|---|---|
| Profilo console a 1 kHz per thread/funzione | nostro `nx_profiler.cpp`, `where.py --profile`, `guest_profile.py` | D | nullo | decide tutto il resto | righe `profile id` | **run 71** |
| Copie "macchina" delle funzioni calde (registri in locali, write-back dei soli registri cambiati, callee inline, dispatch diretto) | bejeweled3-nx / katamari-nx / rr6-nx `docs/optimising-functions.md`, `tools/localize_native.py`, `post_codegen.py` | A | alto (SDK v0.10 contro il nostro v0.9; test di equivalenza) | nei loro port 62,8 % dei campioni in copie; guadagno per funzione da misurare | modalità verify, profilo prima/dopo | profilo |
| Natives a mano solo da profilo console | stessi, `hand-natives.md`; nostro run 64 (`dk_run64_routes.cpp`, routing audio) | D (metodo) | medio per funzione | dove il profilo lo indica | test di equivalenza + profilo | profilo |
| Lock globale delle sezioni `mfmsr`/`mtmsrd` inutile | torchlight `atomiques_sans_verrou.py` (§1.4) | A | medio (uno strumento sul codice generato, rifiuto di ogni sezione con altro che `lwarx`/`stwcx.`) | 2,2 % del thread principale (loro); da noi **163 siti in 7 file**, il primo a `0x82169C04` vicino al routing audio | profilo: quota di `REX_ENTER_GLOBAL_LOCK` | profilo |
| Finestra della memoria guest su blocchi da 2 MiB | torchlight §1.1 (loro: -15/-17 %) | A | - | **già allineata da noi** (`memory_switch.cpp` 435-452: arena e pool a 2 MiB, blocchi mappati da 2 MiB); resta da verificare che le `svcSetMemoryPermission` a 4 KiB non spezzino i blocchi | microtest TLB (accessi casuali, torchlight §1.1) | dopo |
| Codice del .nro su pagine grandi | torchlight §1.2 (-2/-4 %) | A | medio (riproiezione del codice, niente ritorno a hbloader) | -2/-4 % | A/B in sessione | dopo |
| Attesa attiva con `db16cyc` vuoto | torchlight §1.9 | A | basso | meno carico, nessun frame (loro) | profilo | profilo |
| LTO, PGO da sessione console, ordine delle funzioni | rr6/katamari `optimising-functions.md` §PGO; torchlight §1.10 (insidie) | A | alto | da misurare; PGO solo dopo le copie | profilo | dopo |
| Timer che recupera ogni tick perso dopo una sospensione | katamari-nx `console-rc9-breakdown.md` (TimerQueue 66-88 % di un core dopo il sonno) | D | basso | evita un core occupato dopo la sospensione | profilo dopo una sospensione | dopo (non è il problema attuale) |

### 2.3 Render thread e GPU

| Tecnica | Origine (file) | Cat. | Costo | Beneficio atteso | Verifica | Stato |
|---|---|---|---|---|---|---|
| Delta del device, catena, wake a lotti, prefetch, callback veloci | sa2-nx `deko3d-renderer-guide.md` §7 | D | - | - | righe `device snapshots`, `pipeline binds` | fatto (in ReNut, attivi) |
| Store cards (prova delle copie senza hash) | sa2-nx §7, `tools/store_cards.py` | A | alto (passata sul codice generato e su ogni scrittore) | hashing 16 % -> 0 (SA2) | `cached streams: checks of N MB` | profilo |
| Geometria quasi mai cambiata tra frame | torchlight §1.6; nostro vecchio store (runs 60-65) | A | medio | -5/-7 % (loro) | MB riconvertiti a frame | profilo |
| Tempo GPU per passata | ReNut `renut_gpu_profile`, `banjo_renut_diag_gpu_shaders` | D | nullo | decide le ottimizzazioni GPU | righe del profilo GPU | **run 71** |
| Resolve multisample disegnati, target MS non compressi | nostro run 69-70 | - | - | correttezza; costo in banda da misurare | profilo GPU (passate di resolve) | fatto; la compressione dei soli 1x resta |
| Pruning degli output VS, draw solo depth, early Z | ReNut/banjo (`vsout`, `banjo_gpu_depth_only`, `earlyz`) | D | - | - | righe `draw kinds` | fatto |
| Zcull | plume (`banjo_deko3d_zcull`) | - | - | - | - | fatto (invalidato dopo la ricostruzione della depth) |
| Kick anticipati, sync col display, late input | sa2-nx §8 | D | basso | pacing e latenza quando il frame rate è vicino al limite | - | dopo (a 14 fps non contano) |

### 2.4 Audio

| Tecnica | Origine | Cat. | Costo | Beneficio | Verifica | Stato |
|---|---|---|---|---|---|---|
| XMA asincrono, routing nativo, priorità a scadenza | nostri run 49, 64 | - | - | - | righe `audio:` | fatto (attivi) |
| Funzioni del motore audio del gioco come natives | rr6-nx (Audio Worker 48 %, 86 % in tre funzioni), katamari | A | medio | da profilo | profilo del thread audio | profilo |
| Lavoro spostato sul core 3 | renut-nx (`banjo_system_core_*`) | R finché non misurato | - | renut-nx: audio peggiore con le compilazioni lì | - | escluso senza misura (richiesta dell'utente) |
| Lavori di sfondo di ReNut via dal core dell'audio (core 2 -> 0) | profilo del run 71 (§5) | D (un cvar) | nullo | audio meno affamato, compilazioni più brevi | run 72 contro 71 | **run 72** |

### 2.5 Compatibilità e funzioni

| Voce | Origine | Cat. | Stato |
|---|---|---|---|
| Hook di gioco (aspect, motion blur, grana) | vecchie build (`theft4_add_lra_hooks`) | A | non trasferibili come file: dipendono dal livello LRA e da `gta4_native_hooks.cpp`; vanno riagganciate le funzioni del gioco in `tools/renut-gta4` |
| SaltyNX | nfsmw-nx | - | da studiare (non letto ancora) |
| Pannello della frusta mancante nell'introduzione | - | - | indagine a parte: i pannelli si alternano mentre il gioco carica (sul PC, che carica in fretta, se ne vedono due); serve una cattura fitta dell'introduzione sulla console |

## 3. Run 71: la baseline completa

Stessa NRO del run 70 (`cc64728`), solo `game.toml` diverso: trace spento, `nx_profile` dal secondo 240 (i primi 4
minuti sono benchmark puro, il resto profilo), `renut_gpu_profile` ogni 10 s, catture ogni 30 s. Da leggere: fps e
audio delle finestre (senza trace), `profile id` per thread e funzione (`where.py --profile`), il profilo GPU per
passata, `cpu by core`. Le prime ottimizzazioni si scelgono da lì.

## 4. Risultati

| Run | Modifica | fps a piedi | fps in auto | GPU ms/frame (auto) | Audio % in gioco | Stalli | Note |
|---|---|---|---|---|---|---|---|
| 70 | baseline (trace acceso) | 12,9-14,5 (fuori dal trace) | - | 67-75 | 62-70 | 10-15 s ogni 30 s (il trace) | |
| 71 | trace spento, profilatori accesi | 12,8-14,5 | **8,5-10,3** | **80-89** | 65-74 a piedi, **52-60 in auto** (fino a 121 uscite a secco in 10 s) | finestre di 12,8-15,2 s a 4-5 fps: compilazioni uam (1,5-2,9 s l'una) in zone nuove | la baseline vera |
| 72 | lavori di sfondo di ReNut dal core 2 al core 0 (`banjo_background_compile_core = 0`); profilo esteso ai thread di ReNut | - | - | - | - | - | da fare |

## 5. Run 71: lettura dei profili (`out/console-runs/71-logs`, 889 s, 0 fault)

Clock standard per tutto il run, cache calda (385 varianti), trace spento (nessun blocco periodico: confermato
dall'utente). Campionatore CPU dal secondo 240; profilo GPU ogni 10 s.

**Stutter e cali di fps sono due cose diverse.**

- **Stutter in zone nuove = compilazioni di varianti degli shader.** Le finestre che si allungano a 12,8-15,2 s (4-5 fps,
  render thread a 230-248 ms a frame) sono quelle con nuove compilazioni uam (da 1 a 16 per finestra), che costano in
  media **1,5-2,9 s l'una** contro 0,65 s quando la CPU è libera: il render thread aspetta lo shader. La cache calda le
  elimina dove si è già stati; non dove non si è mai stati.
- **Il frame rate basso è GPU e render thread insieme.** In auto, 3.400 draw a frame: GPU 80-89 ms, render thread di
  ReNut 80-97 ms (non campionato: non era nella lista del campionatore; corretto nel run 72).

**GPU per passata (in auto, 89 ms a frame, 59 frame):**

| Passata | ms a frame | Draw a frame |
|---|---|---|
| Scena 1280x720 colore+depth (G-buffer, luci, forward) | 38,4 | 1.955 |
| Shadow map 1280x1280 solo depth | **30,9** | **2.385** |
| Resolve (colore) | 9,6 | - |
| 1280x720 colore, depth resolve, 256x256, 320x180, presentazione, upload, resto | ~10 | - |

Il costo segue **il numero dei draw** (13-20 µs a draw), non i pixel (una shadow map 1,6 MP solo depth non costa 31 ms
di riempimento). Contatori deko3d a frame (in auto): ~3.400 draw, ~900 bind di pipeline (~750 di stato), **~118 barriere
di frammento** (~80 invalidano le texture), 2-3 barriere complete; unione e alleggerimento delle barriere già attivi
(`banjo_deko3d_barrier_merge`, `_lean`). Da qui le prossime ottimizzazioni GPU: costo fisso per draw (stato, costanti
spinte, binding), le barriere (quali transizioni le producono), e il resolve (9,6 ms: i resolve multisample disegnati
del G-buffer, 3 tile x 4, su target non compressi).

**CPU per thread (campioni dal secondo 240):**

| Thread | Ruolo | Cosa fa |
|---|---|---|
| id 16 (`0x82169400`) | **audio del gioco** (mixer) | **occupato**: cicli DSP NEON in `sub_821737C0` (~25 %), `sub_8216BFE8` (~25 %), `sub_82185E38` (~12 %), routing nativo `sub_8216D7E0` 3 %; contende uno spinlock con il 17 |
| id 17 (`0x82167810`, `0x828C0968`) | audio (lato) | dorme (`KeDelayExecution` 46 %) e **gira su uno spinlock con `sched_yield`** (32 %) |
| id 6 | thread principale | **aspetta il 97 %** (eventi, attese multiple): non è il collo di bottiglia |
| id 9/10/11 | worker del gioco | aspettano; **14 % bloccati in `XEvent::Set`** sul `std::mutex` dell'evento (contesa) |
| id 12 | render thread di GTA IV | 77 % in attesa della coda di ReNut |
| id 2 | decoder XMA (host) | inattivo al 95 %: non è il problema dell'audio |

**Core:** tutti i thread del gioco hanno maschera 7 (core 0-2); i due thread audio preferiscono il core 2 (`ideal 2`);
**ReNut fissa sul core 2 anche tutti i suoi lavori di sfondo** (uam, ring cleaner, prebuild, salvataggio della cache),
mentre il core 0 ha il thread principale che aspetta quasi sempre. Il core 2 è sovraccarico: spiega insieme l'audio
affamato (il mixer non finisce in tempo) e le compilazioni 3-4 volte più lente. (`cpu by core` dà zero: il contatore
per core non funziona in questa build; la lettura viene dalle maschere, dai core ideali e dalle quote dei thread.)

**Allocazioni, decompressione, I/O:** nessun segno nei profili dei thread campionati (il thread principale non è
occupato; nessun thread di streaming tra i più carichi). Il log a livello debug scrive ~40 righe al secondo dal suo
thread: trascurabile.

## 6. Piano dopo il run 71 (in ordine)

> **2026-11-12:** su indicazione di Chansey il lavoro passa alla Ultimate Base (`20-ultimate-base-migration.md`): i
> punti sotto restano validi come obiettivi, ma si misurano sulla base (che porta già cache DKSH offline, machine
> copies, PGO + LTO, SMAA, risoluzione dinamica, kick XMA senza lock, attese su indirizzo). La run 72 resta il controllo
> del vecchio albero.

1. **Run 72: i lavori di sfondo dal core 2 al core 0** (una variabile, solo `game.toml`; la NRO aggiunge al profilo i
   thread di ReNut). Atteso: audio più vicino al 100 %, compilazioni più brevi. Verifica: righe `audio:`, `uam N compiles
   (avg ...)`, durata delle finestre in zone nuove, confrontati con il run 71 sullo stesso percorso.
2. **Cache DKSH offline** (rr6-nx `deko3d_variant_cache.py`, adattato): le nostre pipeline danno più costanti di
   specializzazione di quelle di rr6 (905 vsout, 909 earlyz, 904, 906, ...), quindi le chiavi vanno ricostruite
   dal nostro `render/pipeline.cpp`. Verifica senza console: le chiavi generate da `pipeline_list.bin` devono coincidere
   con quelle di `deko3d_shaders.bin` della scheda (l'utente li copia con i log del run 72).
3. **Audio, se il run 72 non basta:** il mixer (id 16) come natives a mano delle tre funzioni DSP (metodo di rr6-nx e
   del nostro run 64, con test di equivalenza), e lo spinlock dell'id 17 (`xeKeKfAcquireSpinLock` con `sched_yield`).
4. **Render thread** (dal profilo del run 72) e **GPU** (costo per draw, barriere, resolve): una modifica misurata per
   run.
5. **Contesa in `XEvent::Set`** (14 % dei worker): il `std::mutex` per evento dell'SDK.
6. **RAM a 1600 MHz**: esperimento separato, solo dopo la baseline stock e le prime ottimizzazioni (richiesta
   dell'utente): stesso percorso, A/B stock/1600, temperature e stabilità; la build di riferimento resta a clock
   standard.

## 7. Ultimate Base: misure U13-U14 e piano (2026-11-15)

La matrice sopra resta il catalogo; le misure della base sono in `20-ultimate-base-migration.md` §2.12-§2.20. Il quadro
dopo la correzione della luce (U14, console, clock standard):

| misura (prima finestra a 650 draw) | U13 V-light (luce sbagliata) | U14 (luce giusta) |
|---|---|---|
| fps | ~26 | 16 |
| GPU per frame | 35,5 ms | 61,5 ms |
| render thread per frame | 15 ms | 13,7 ms |
| mixer per blocco da 5,33 ms | 5,43 ms | 5,25 ms |
| audio consegnato | 86,5 % | 91,4 % |

U15-A (port 129-141 della base), stessa cucina: 16,1 fps, GPU 61,2 ms, render thread 13,4 ms, audio 89,8 %, mixer 5,56 ms per blocco (interno): invariati; thread XMA ~30 -> ~10 % di un core. U15-P: il ripristino dello stencil a nove passaggi costa ~12,8 ms di GPU a ripristino (25,6 ms a frame in cucina).

In strada (U10/U11): ~10 fps, render thread 35-70 ms e GPU 33-45 ms, tre core pieni. Il mixer è limitato dal calcolo
(router `sub_8216D7E0` 28 %, `sub_828C2D28` 17 %, `sub_828C6740` 14 %, `sub_828C5A58` 11 %, `sub_828C7848` 7 %; tutte e cinque sono già copie macchina in place, che il profilo nomina come la funzione generata).

Piano, un gruppo per prova, ognuno con misura prima/dopo:
1. **U15**: i port 129-141 della base (esatti; CG-13 resta spento): cache XMA v2 (PC: frame serviti dalla cache 6,5 ->
   92 %), salto delle scritture FPSCR, catture ristrette, handle fuori dal lock globale, correzione deko3d della
   decompressione differita. Più il profilo GPU per passaggio sulla console (run P).
2. **GPU**: dal profilo di U15-P, il passaggio che costa sul deko3d (il ripristino dello stencil, l'ambientale, le luci,
   le ombre 1280x1280) e la sua riduzione esatta.
3. **CPU / audio (U16)**: rigenerazione con i passaggi di codegen della base (136: float loads a ogni modo, fmadds noti;
   140: store cards per le viste), nativo del biquad `sub_828C2D28` (stato del filtro nei
   registri, stesse operazioni nello stesso ordine), guardia sul PC.

U16 (PC, da provare sulla console): il ripristino dello stencil in due passaggi quando la sorgente è la copia del gioco (atteso ~-20 ms di GPU a frame in cucina), le forme ARM dei passaggi di codegen 136/140, la risoluzione dinamica della base (run D, 540-720 righe). Prossimi: il nativo del biquad del mixer (stato del filtro nei registri, esatto), le funzioni del render thread, i glitch con il salvataggio dell'utente sul PC.

U16 sulla console (doc 20 §2.23): fps = U15-A (cucina 16,5, strada 9,3, filmato 20,0, garage 10,7); audio 95-99 % (U15:
85-92 %); il ripristino in due passaggi **non è mai partito** (il percorso veloce dei draw di deko3d non segna la copia):
corretto in U17. Risoluzione dinamica: nessun guadagno, la GPU di cucina e filmato non scala con l'altezza 3D.

| Collo di bottiglia (U16-A) | Misura | Leva U17 | Stato |
|---|---|---|---|
| GPU, cucina/filmato | 49-60 ms; copia + ripristino 20-25 ms | ripristino in due passaggi anche su deko3d | nell'albero, console da provare |
| CPU, strada/garage | core 0-2: ~357 % di richiesta su 300 %; core 3 libero | mixer XAudio sul core 3 (`gta4_xaudio_supervisor_core3` 1/2) | nell'albero (spento), A/B C1/C2 |
| Mixer | ~105 % di un core in ogni scena | nativi esatti di biquad e DSP | non iniziato |
| Thread principale | 38-40 %, in attesa di semafori e di un core | `switch_thread_priorities` "Main XThread=0x2E" (kabba536) | da provare |
| Vetri/fari/ombre intermittenti | differenza solo sui vetri a scena ferma | bisezione G0-G3 (percorso veloce, barriere, salto texture) | da provare |

U17 (doc 20 §2.24, 2026-11-16): NRO `15da0fff…` costruita (1 job, incrementale), pacchetto `out/console-runs/u17` con A,
C1, C2 (facoltativa), G0-G3; **nessuna misura sulla console ancora**. Leve preparate per U18, dalla base (`bf6ccc1e`, port
142-147, i "speed-up" di TW2DE-NX) e da TW2DE stesso; i benefici sono quelli misurati da altri giochi, non da GTA IV:

| Leva (U18+) | Dove agisce in GTA IV (U16-A) | Misurato altrove | Rischio | Prova |
|---|---|---|---|---|
| Port 145: `stream_redeem`, `keep_unused_targets`, `dynamic_texture_rows`, `native_16bit` | render thread ReNut ~60 % in strada (46 % di calcolo) | TW2: -7 ms di render thread a frame | basso (accesi nella base, guardia PC) | A/B contro U17-A, render thread e fps strada/garage |
| Port 145: `banjo_async_present` | presentazione | TW2: -2,7 ms a frame; Ryujinx non conclusivo | pacing | `[latency]` swap -> present, A/B |
| Port 146: pezzi di file da 4 MB, buffer host diretti, mutant senza mutex | streaming, attese | TW2: richiesta FS ~2,3 ms fissi | basso | tempi di caricamento, stutter in guida |
| Port 144: `spin_hint_yield` | 12 `db16cyc` (XAudio `8216C750`/`82169D90`, D3D `829CFE80`/`829DD928`) | - | latenza audio | audio %, CPU mixer |
| LTO (`BANJO_SWITCH_LTO`), poi PGO della base (`pgo-inplace/`) | tutto il codice | sa2-nx, katamari, TW2 (non quantificato qui) | memoria del link (16 GB); build ~2 h | build LTO sola, poi ciclo PGO su cucina, strada, garage, filmato |
| `vec_neon_forms.py` (TW2) | 179 vslw, 2.222 vpermwi/vcsxwfp/vcuxwfp; mixer quasi assente | - | basso (esatto, test qemu) | guardia densa, render guest |


**U17 letto (doc 20 §2.25, 2026-11-16; misure Switch).** Ripristino in due passaggi attivo sulla console: GPU cucina
59,3 -> 39,2 ms (16,6 -> 24,6 fps), filmato 49,0 -> 38,6 ms (20,0 -> 25,1 fps); strada e garage invariati (9,4 / 11,1 fps):
limite CPU (render thread 44-54 ms). C1/C2 respinte (audio 80,9 / 72 % in strada: il mixer sul core 3 riceve meno CPU; C2
rallenta anche le letture del SD da 10 a 51 ms). G1 (fast draw spento) costa 7,4 ms di render thread in strada; G2/G3 neutri;
nessuno dei tre toglie i lampeggiamenti. Le raffiche di catture costano ~4 ms di render thread a frame (G0 contro A).

| Collo di bottiglia (U17-A) | Misura | Leva U18 | Stato |
|---|---|---|---|
| Audio | mixer saturo (102-111 % di un core, 94 % sulla CPU), audio 95,5-98,4 % | nativi esatti di router `8216D7E0`, `828C6740`, biquad `828C2D28`, `828C5A58` con guardia bit-exact; **non** un altro core (C1/C2) | da fare (PC prima) |
| Correttezza CPU | 4 copie native diverse dall'originale sul percorso della nuova partita | fuori da `localized_bulk.txt` (`out/u18-prep/g_bulk_drop.diff`) | preparata, non applicata |
| Alone/vetri/ombre | alone a due stati solo sulla console (PC stabile a 60 e 30 fps); non fast draw, barriere, texture skip | run di viste V (verifiche del renderer), poi gruppi H1-H4 | da provare |
| CPU strada/garage | render thread ReNut 56-58 %, render guest 46-47 %, main 37-38 % | port 145 (stream redeem, righe di texture), 146 (I/O), main a 0x2E | gruppi pronti |
| Render thread ReNut | `FastDrawLocked` 21,9 %, `PushFloatConstants` 6,7 % (U16) | port 145; present asincrono al massimo ~0,5 ms a frame (misura libnx) | gruppo pronto |
| Codegen | -O3, nessun LTO/PGO | gruppo 144 (rigenerazione), LTO sola, poi PGO | da fare |

**Proposta U18 (non costruita: serve l'autorizzazione).** Due NRO, ciascuna a 1 job, cache DKSH U10, pacchetto con toml
che cambiano **una** leva per run, tutte a 720p fissi, stesso salvataggio e percorso di U17-A (cucina, strada, filmato, garage),
catture una ogni 5 s (le raffiche solo nella run di viste).

NRO **U18-1** = U17 + gruppi 142, 143, 145, 146, 147 + `g_bulk_drop` (le chiavi nuove sono a runtime, accese di default nella
base: i toml le spengono dove serve).

| Run | Rispetto a | Differenza | Misura |
|---|---|---|---|
| B0 | U17-A | tutte le chiavi nuove spente (`banjo_renut_stream_redeem`, `banjo_renut_keep_unused_targets`, `banjo_renut_dynamic_texture_rows`, `banjo_deko3d_native_16bit`, `banjo_async_present`, `thread_mutant_release_lockfree` = false, `filesystem_bounce_piece_mb` = 1, `filesystem_bounce_guest_only` = false, `kernel_deferred_overlapped_delay_ms` = 100, `xmp_poll_throttle` = false): resta solo il codice e le 4 copie tolte | uguale a U17-A entro il rumore; audio |
| B1 | B0 | gruppo 146 acceso (I/O) | `[io]` letture, caricamento del filmato, stutter |
| B2 | B0 | 145: `stream_redeem` + `dynamic_texture_rows` + `keep_unused_targets` | render thread strada/garage |
| B3 | B2 | 145: `native_16bit` | GPU, memoria texture, catture (formati) |
| B4 | B0 | 145: `banjo_async_present` | render thread, `[latency]`, pacing |
| B5 | B0 | 143 (overlapped 0, `xmp_poll_throttle`) | tempo dall'avvio alla stanza |
| V | B0 | verifiche del renderer (`banjo_deko3d_push_constants_verify`, `banjo_renut_fast_draw_verify`, `_chain_reuse_verify`, `_texture_slot_skip_verify`, `_store_cards_verify`, `banjo_deko3d_txq_verify`), raffiche, 60 s fermi nel corridoio | log: prima differenza; non è una run di misura |
| N | - | nuova partita su slot separato (run-protocol "Dalla U18") | la missione parte? |

NRO **U18-2** = la migliore di U18-1 + gruppo 144 (rigenerazione: `spin_hint_yield`, NORMSHORT2): contro la stessa
configurazione su U18-1. Poi, solo se i lampeggiamenti restano senza nome dopo V: H1-H4 (doc 20 §2.25), un gruppo per run.

**LTO e PGO (progetto, nessuna build).**
- **LTO da sola prima**: stesso commit, stessi toml, solo `BANJO_SWITCH_LTO=ON` (i 4 file SDK già fuori LTO); si misura la
  memoria massima del link a 1 job (16 GB di RAM, ~25 GB liberi su C:) e si confronta con la NRO senza LTO dello stesso commit
  sulle stesse finestre. Confronto valido perché cambia un solo flag.
- **PGO** (catena della base): NRO strumentata (`BANJO_PGO=generate`), quattro run dal salvataggio con lo stesso percorso:
  cucina (60 s fermi + giro), strada a piedi e in auto (120 s), filmato intero, garage (60 s); i flussi `pgo/banjo_pgo_<N>.stream`
  (ogni 2 min) uniti con `tools/pgo_merge.py` in `pgo-inplace/`; NRO `BANJO_PGO=use` contro la NRO LTO (o -O3) dello stesso
  commit. Costo: due build complete a 1 job (strumentata e finale; ~2 h ciascuna, stima), ~25 minuti di console; la NRO
  strumentata è più lenta (i suoi fps non sono misure). Riproducibilità: commit fissato, hash dei profili uniti nel manifest,
  stessi toml; i profili si rifanno a ogni rigenerazione del codice (dopo il 144, prima del PGO).
- Il guadagno atteso non è stimabile senza misura: oggi la strada è limitata dal render thread e dal mixer, codice in gran
  parte già copiato a mano o a macchina.

**U18-1 costruita (doc 20 §2.26, 2026-11-16; build sul PC, nessun run su console).** NRO `315669d3…` (stamp
`168baa62b651-dirty+u18-1`) = U17 + port 142, 143, 145, 146 + le quattro copie native tolte (il 147 dipende dal 144: va con U18-2); pacchetto
`out/console-runs/u18-1` con B0-B4, B5 (facoltativa, dopo B0), V (viste), N (nuova partita). U18-2 (gruppo 144), LTO, PGO
e mixer non iniziati.

## 8. U18 MAX (2026-11-17): una build con tutto il trasferibile, al massimo 2-3 sessioni su console

Richiesta dell'utente: niente più run isolate; **MAX-A** = tutte le ottimizzazioni trasferibili e compatibili accese
insieme, una sessione (cucina, strada, filmato, garage); MAX-B solo per una regressione concreta; MAX-C solo se
indispensabile. U18-1 letta (doc 20 §2.27): i port 142-146 non cambiano nulla di misurabile e non peggiorano nulla.

| Tecnica | Fonte | MAX-A | Prova sul PC | Rollback senza ricostruire |
|---|---|---|---|---|
| Percorso veloce: bias/gamma dei target nel numero dello stato (alone intermittente) | Theft4-NX (V di U18-1) | acceso | non eseguibile sul PC (solo deko3d) | nessuna chiave (correzione) |
| Nativo del riverbero `828C6740` (15,5 % del mixer) | Theft4-NX | `gta4_hand_reverb` | guardia: 4.026 confronti, 0 differenze | `gta4_hand_reverb = false` |
| Nativo del limiter `828C5A58` (12,1 % del mixer) | Theft4-NX | `gta4_hand_limiter` | contatori + modello esatto (240.000 finestre, 0 differenze) | `gta4_hand_limiter = false` |
| Cache per sito delle chiamate indirette (CG-08), 14.161 siti | base 150 + correzione di `indirect_calls.py` per GTA IV | acceso (solo Switch) | - (il PC usa il percorso del censimento) | no (compile time: `REX_SITE_CACHE_OFF`) |
| CAS relaxed + fence divise | base 150 | acceso | guardia 0 differenze | `blur_guest_cas_relaxed`, `blur_guest_fences_split` |
| Double nell'unità vettoriale (154 load), barriere dei cicli contati | base 150 | acceso | guardia 0 differenze | no (rigenerazione) |
| `spin_hint_yield` (12 siti), NORMSHORT2 NEON | base 144 | acceso | guardia 0 differenze | no (rigenerazione) |
| Strumenti di test PowerPC | base 147 | (fuori dall'NRO) | - | - |
| Lavoro CPU di Renut (binds senza lock, memo, set-words, pool) | base 148 | acceso; GTA IV non registra command buffer | build | chiavi `blur_renut_*` |
| Hand-over della griglia dei campioni | base 149 | acceso (esatto) | build | - |
| Allocatore dei blocchi piccoli, 32 MB | base 151 | acceso (solo Switch) | - | `blur_small_alloc_mb = 0` |
| APC senza lock, registro delle attese, strumenti, frame allo Swap | base 152-154, 156 | acceso | build | - |
| Colore scalato per il bias dei target | base 155 (1) | **spento per GTA IV** | il PC è giusto senza | - |
| Resolve della griglia nella firma | base 155 (2-3) | acceso | build | - |
| 6 copie macchina nuove (2 tolte dalla guardia, 1 non verificabile sul PC) | profili U18-1 | acceso | guardia 0 differenze sulle 6 | no |
| Port 143, 145, 146 (incl. 16 bit nativo, async present) | base | acceso (default) | U18-1 B1-B5 senza regressioni | chiavi della base |
| Priorità XAudio 0x2D | U11 | nella NRO | - | `gta4_xaudio_thread_priority = -1` |

Esclusi: C1/C2, prefetch del mix, `audio_switch_pump_pairs`/`_frames` (latenza), `BLUR_FPCR_ONE_MODE` (non esatto),
`thread_event_spin`, `BLUR_LOCALIZE_ABI_TRUST`, LTO e PGO (vedi sotto). Fast draw resta acceso (ora corretto).

**LTO e PGO dopo MAX-A.** La LTO è una build intera in una cartella propria con un picco di memoria al link non misurato
(16 GB, 1 job): va fatta su una base già verificata, non insieme a 20 cambi. La PGO vuole una sessione con l'NRO
strumentata (il percorso di MAX-A, ~25 minuti): se MAX-A è buona, MAX-B può essere quella sessione di addestramento
più l'eventuale correzione, e MAX-C la build PGO + LTO.

**Il prossimo nel mixer:** `828C2D28` (biquad in cascata, 14,1 %): stato e coefficienti nei registri (stima 4-5 % del
thread). **Il prossimo nella grafica:** il disco della lampada (solo console; candidati in doc 20 §2.27) e il profilo GPU
della run P.

## 9. U18 MAX-B (2026-11-18): MAX-A letta, la seconda sessione

MAX-A sulla console (doc 20 §2.28): audio risolto (mixer 5,12 -> 4,37 ms per blocco, consegna 100 %), strada invariata
(9,3 fps, core 0-2 saturi), disco della lampada causato dal clamp 7e3 (L3), nessuna chiave del percorso veloce diversa (P).
La strada resta il limite: la CPU dei core 0-2 (mixer ~95 % di un core, render thread dell'host ~60 % sul core 1, thread di
rendering del gioco ~45 %, main ~40 %, audio RAGE ~28 %, decompressione LZX ~14 %, XMA ~11 %, worker ~30 %).

| Tecnica | Fonte | MAX-B | Prova sul PC | Rollback senza ricostruire |
|---|---|---|---|---|
| Clamp 7e3 spento per GTA IV (il disco della lampada) | MAX-A L3 | `gta4_switch_defaults.toml` | il PC non l'ha mai avuto | `dishonored_renut_clamp_7e3 = true` |
| Barriera delle texture lette dai vertex shader (luci esterne che lampeggiano: ipotesi) | base 161 (fnxf_xiii) | acceso per GTA IV | solo deko3d | `banjo_deko3d_vertex_texture_barrier = false` (run V0) |
| Nativo del filtro audio `828C2D28` (16,3 % di un core in strada): stato e coefficienti in locali | Theft4-NX | `gta4_hand_biquad` | guardia (vedi doc 20 §2.28) | `gta4_hand_biquad = false` |
| Batch dell'host di Blur (render thread ~16 % più veloce per draw su Blur) | base 177 | acceso (default) | verify PC | chiavi `blur_renut_*` della base |
| Contabilità del frame dopo il present | base 161 | acceso | build | `banjo_renut_bookkeeping_after_present = false` |
| Submit progressivo, untile su 2 thread, barriere delle copie, log di deko3d su thread | base 161, 164, 171 | acceso (default) | build | chiavi della base |
| Memo delle superfici con base EDRAM, uam più veloce e senza perdite | base 161, 162 | acceso (correzioni) | build | - |
| Indirizzo delle store card calcolato una volta | base 160 | acceso (post-codegen) | guardia | no (rigenerazione) |
| Ripresa delle sottomissioni GPFIFO rifiutate (crash di Blur ai caricamenti) | base 157 | acceso | solo Switch | - |
| Menu di gioco (A+B+X+Y+L+R), registrazione -O0, strumenti | base 158, 177-180 | come la base | build | - |
| Texture park | base 168/181 | spento (default della base) | - | - |
| EDRAM carry, crop dello scaler, altre correzioni di fnxf_xiii/rehatched | base 159, 163, 166, 169, 170, 172, 174, 179 | spenti (default) | - | - |

Verifiche sul PC (doc 20 §2.28): guardia densa 0 differenze su 1,39 M chiamate (due run), il filtro 4.667 confronti
con le istruzioni originali; il blocco nativo esegue ogni passata del ciclo (770.213 su 770.213, run di conteggio); con la
chiave spenta (toml) la copia, 0 differenze; il test di sovrapposizione esatto su 2 M casi. La build PC rielaborava il
codice generato (`post_codegen` non idempotente su un albero già elaborato): rigenerato da zero e controllato contro il
manifest prima di ogni prova.

Esclusi: un nativo del decoder LZX `829B2600` (14 % di un core in strada, foglia intera di 272 istruzioni: il prossimo
candidato, da scrivere come nativo esatto e da verificare con la guardia sulle uscite), il router `8216D7E0` (mix XAudio
già nativo, limitato dalla memoria), ogni riduzione dei sottopassi della fisica (cambierebbe il gioco).

**LTO.** Il PC ha 15,9 GB di RAM (9,4 liberi a riposo) e 12 GB liberi su C:. La LTO della base mette in GIMPLE il codice
ricompilato (ELF da 85 MB senza LTO) e lo ottimizza al link (`-flto=N -flto-partition=balanced`): la WPA tiene in memoria
l'intero programma, stimata oltre 8-10 GB per un binario di questa taglia, più i file temporanei dei LTRANS. Rischio
concreto di esaurire la RAM e il disco C: con N >= 2. Fattibile solo con `-flto=1`, la cartella temporanea su D: (158 GB
liberi), nient'altro in esecuzione, dopo che MAX-B è verificata; non avviata in questo ciclo.

**PGO.** Il flusso è pronto nella base (`BANJO_PGO=generate`, `tools/pgo_merge.py`, `BANJO_PGO=use`, una cartella per
forma: `pgo-inplace/` con le copie in place). Serve una NRO strumentata degli STESSI sorgenti di MAX-B e una sessione con
il percorso rappresentativo (cucina ferma, strada a piedi e in auto, filmato, garage), così il profilo copre anche le copie
macchina e i nativi del mixer. Nessun profilo di altri giochi: `pgo/` della base è di Katamari. Con il limite di 2-3
sessioni: la NRO strumentata può accompagnare MAX-B nella stessa sessione (una run in più dopo A/L), e la terza sessione
(MAX-C) prova PGO + LTO.
