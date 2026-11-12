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

| Run | Modifica | fps gameplay | GPU ms/frame | Audio % | Note |
|---|---|---|---|---|---|
| 70 | baseline (trace acceso) | 12,9-14,5 (fuori dal trace) | 67-75 | 62-70 | blocchi da trace ogni 30 s |
| 71 | trace spento, profilatori accesi | - | - | - | da fare |
