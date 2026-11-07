# 17 – Roadmap: da prototipo deko3d a build giocabile

Stato al 2026-11-02, branch `sync-clean`, HEAD `17f3a18` + modifiche locali (`17f3a18-dirty`).
Fonti: run 44 letto in [16-deko3d-devlog.md](16-deko3d-devlog.md) §P; run 45 costruito in §Q e letto in §R; run 46 costruito in §R; studio dei port esterni (questo documento, §3–§4).
Ogni affermazione è **VERIFICATO** (log, codice o sorgente esterno letto, con percorso), **DA VERIFICARE** (serve una prova, di solito sulla console) o **STIMA** (inferenza).

---

## 1. Situazione attuale (VERIFICATO, run 44)

| Area | Stato |
|---|---|
| Avvio, ingresso in partita, uscita | stabili: 0 fault, uscita con `+` in 9 passi, `libdeko3dd errors 0` (run 37–44) |
| Rendering, stato "scuro" | **corretto nella sostanza dopo l'hand-off della depth (run 44)**: niente più strada bianca, zolle nere, cunei. Pixel scuri 4,6–21 % contro 33–36 % (run 42/43) |
| Rendering, stato "chiaro" | **sbagliato**: frame sovraesposti blu-bianchi con cunei, alternati ai frame scuri (luma 161–197 contro 22–30) |
| Geometria | auto e volto del filmato **frantumati** (triangoli mancanti, punte) |
| Testo | lettere mancanti intermittenti |
| Prestazioni, gioco | **1,1–2,0 fps** a 2.900–4.500 draw/frame |
| Prestazioni, filmato | stallo di ~90 s a 0,6 fps: singoli draw a schermo intero da **770–800 ms di GPU** |
| Audio, controlli | funzionano (run 24–30 sul percorso NVK; nessuna regressione segnalata su deko3d) – DA VERIFICARE in modo esplicito su deko3d |

## 2. Colli di bottiglia misurati (VERIFICATO, run 44)

| Fase | Limite | Numeri |
|---|---|---|
| Gioco | **thread di rendering del titolo dentro `SubmitTitleCommand`** | 8,3–9,4 s su 10 s; front end 5,4–7,7 s + push in coda 1,5–3,1 s; attesa al Present **0** |
| Gioco | recorder | registra 48–60 % del tempo, aspetta pacchetti 27–40 %; 135–148 µs per draw |
| Gioco | GPU | **occupata 9–27 %** del tempo (timestamp) |
| Filmato | GPU | 9,5–11,6 s su 10 s; un pass da un draw = 770 ms |
| CPU totale | – | ~226 % su 300 %; thread di rendering 57 % di CPU ma ~90 % del tempo dentro la chiamata → **parte del tempo è attesa/prelazione**, non lavoro (run 45 lo separa) |

**Diagnosi:** il gioco è limitato dalla CPU sul lato del titolo, non dalla GPU e non da deko3d. Migliorare la GPU, la risoluzione o gli shader oggi non sposta gli fps.

**Aggiornamento run 45 (16 §R, campione unico):** lo skip toglie il 44 % dei comandi, ma il gioco scende a 0,2–0,7 fps nella stessa scena. Il collo di bottiglia ora è il **recorder**: coda piena, «the rest» per draw 193–764 µs in crescita. Il recorder però usa solo il 5–28 % di un core, le chiamate del titolo stanno su una CPU il 35–45 % del loro tempo e il processo usa ~105–135 % su 300 %. Quindi i due thread caldi **aspettano**, non lavorano. Ipotesi migliore (DA VERIFICARE): i lock condivisi sul percorso, cioè quello di malloc (un nodo della deque e fino a 4 vettori per pacchetto) e quello della coda (fino a 64 free sotto lock dal run 45). Risposta: R1, costruito come run 46.

### Il confronto che conta (VERIFICATO nei repository citati in §3)

| Progetto | Modello | Costo CPU per draw sulla console |
|---|---|---|
| sa2-nx (Renut, deko3d) | hook D3D → coda → 1 thread di rendering | **7,2 µs** thread di gioco + **7,2 µs** thread di rendering (~1.400 draw/frame) |
| renut-nx (Banjo N&B) | come sa2-nx | **10,1–11,9 µs** per draw a ~5.000 draw/frame |
| nx-mod/xbox360-nx (anello PM4, deko3d) | lettore PM4 + RenderDk | **8–14 µs** (9,9 µs misurati) |
| nfsmw-nx (PM4, NVK) | lettore PM4 | ~8,5–16 µs |
| **Theft4-NX run 44** | hook D3D → front end sul thread del titolo → coda → recorder | **~100–150 µs front end + ~140 µs recorder** (libreria debug) |

**Conclusione architetturale (STIMA motivata):** il nostro modello è lo stesso di sa2-nx/Renut, il port più vicino e più veloce. **Non serve migrare a un altro renderer**, e la via PM4 sarebbe peggiore per GTA IV: farebbe girare il D3D ricompilato del gioco sul thread di rendering, che è già il collo di bottiglia. Il problema è il **costo per draw della nostra implementazione, 10–20 volte quello dei port riusciti**. La strada è portare le loro tecniche, misurate, nel nostro front end e nel recorder. Obiettivo intermedio: ≤ 20 µs per draw per thread, cioè ~60–90 ms per frame a 3.000–4.500 draw (~11–16 fps). Obiettivo finale: ≤ 10 µs, cioè 30 fps a ~3.000 draw.

## 3. Matrice comparativa

Legenda priorità: **P1** prossimi 1–3 run, **P2** dopo, **P3** quando la CPU non è più il limite.
Licenze: sa2-nx, nfsmw-nx, zumasrevenge-nx, gears2/ds2/dante-nx, xbox360-nx: GPL-3.0; rexglue-sdk-nx: BSD-3; plume: MIT; uam: zlib/MIT/MPL-2.0.
**reNut, renut-nx e tutto `app/src/renut_engine`: nessuna licenza, quindi solo idee, mai codice.**

| Tecnica | Origine | File / funzioni d'origine | Theft4-NX oggi | Differenza concreta | Compatibilità GTA IV | Beneficio atteso | Rischio | Test necessario | Prio |
|---|---|---|---|---|---|---|---|---|---|
| Non inviare i comandi di stato che il front end non legge | nostro (run 45) | – | **fatto (run 45)** | ~80 % delle chiamate in meno | piena | thread del titolo: meno lock/timer per chiamata (STIMA) | basso; interruttore `nx_dk_skip_state_commands` | run 45 A/B | P1 ✔ |
| Diff delle costanti su gruppi da 64 B | nfsmw-nx (idea), nostro | `native_constant_diff.h` | **fatto (run 45)** | 4 registri per `memcmp`, swap solo dei cambiati | piena | front end per draw (STIMA) | basso, test 8.000 draw = riferimento | run 45 | P1 ✔ |
| **Coda senza allocazioni**: anello preallocato, pacchetto costruito sul posto, scambio a lotti | sa2-nx (Vyukov ring, `render_thread.cpp`: idee), xbox360-nx (1 pacchetto per draw) | sa2-nx renut-renderer.md item 42; rexglue-sdk-nx | `std::deque` di variant da **1.840 B**: un nodo `malloc` per pacchetto, liberato su un altro thread; 3 copie da 1,8 KB per draw | 0 `malloc` per draw; 1 lock per lotto | piena | push 15–30 % del thread del titolo (VERIFICATO come tempo; causa = STIMA) | basso-medio (ordine, risvegli persi: regola 09 §1) | test offline di ordine e contenuto; run con `threads in the last` | **P1** |
| Delta di costanti in memoria interna/arena invece di `std::vector` | zumasrevenge-nx, rexglue-sdk-nx | dirty range + push dei gruppi | fino a 4 `malloc` per draw (`DiffRegisters` reserve) | 0 | piena | come sopra | basso | test constant diff esteso | **P1** |
| Lookup risolti ai setter: hash shader, maschera texture, dichiarazione, `DeviceState*`; stream solo quelli della dichiarazione | sa2-nx (chain reuse), revisione locale | `native_front_end.cpp:121-179` | 4 `unordered_map` + 2 `std::function` con `lower_bound` su ~12k shader + 17 stream per draw | 1 lettura per draw | piena | front end per draw (STIMA) | basso | test offline del front end contro il vecchio su un trace | **P1** |
| Decodifica dello stato fisso spostata sul recorder (copia grezza) | revisione locale; Renut "move work to the consumer" | `core::DecodeFixedState` (~70 letture con swap) | sul thread del titolo | sul recorder (che aspetta il 27–40 %) | piena | thread del titolo (STIMA) | basso | stesso test | P1 |
| Percorso veloce degli hook quando nessun trace è attivo | revisione locale | `SubmitFireTracedCommand`: `FireTraceContext` ~460 B per comando | costruito sempre | saltato | piena | per comando (STIMA) | basso | log invariati | P1 |
| **Cache della geometria tra frame** (chiave indirizzo+dimensione+hash, ammessa alla 2ª vista; poi store cards) | rexglue-sdk-nx (BSD-3) geometry store; sa2-nx/renut store cards (idee) | `geometria.cpp`; `tools/store_cards.py` | copia vertici/indici nel ring a ogni draw (run 39: ~70 % del recorder prima della cache) | conversione una volta, riuso | buona; store cards richiedono passata sul codegen | recorder (STIMA: la voce più grande) | medio: scrittori non marcati = geometria vecchia | modalità verify (hash e confronto) | **P1–P2** |
| Submit parziali ogni N draw | gears2-nx, renut (kick submits) | `video.cpp KickLocked` (idea) | 1 submit per frame (`dk_core.cpp:165-176`) | GPU parte mentre si registra | piena | latenza GPU, utile quando la CPU scende | basso-medio (fence per lista) | `GPU time from timestamps` | P2 |
| Barriere minime (niente `Full` per liste di soli descrittori) | gears2-nx | – | `DkBarrier_Full` sempre in `dk_core.cpp:369`, 3 in `dk_recorder.cpp` | meno WFI | piena | GPU (STIMA piccolo oggi) | basso | contatore barriere | P2 |
| **Fattore alpha di blend scambiato in deko3d 0.5.0** (riscrittura dei registri con `dkCmdBufReplayCmds`) | renut-nx / SwitchWakerHD (MPL) / Tenkaichi-3-NX (MIT) | `plume_deko3d.cpp blendWords` (idea) | deko3d 0.5.0 installato (VERIFICATO), nessun workaround; usiamo `dkCmdBufBindBlendState` singolare | alpha con il proprio fattore | piena | correttezza: candidato per testo e luminosità | basso, ma **DA VERIFICARE** se anche la funzione singolare ha il bug | frame con alpha ≠ colore; clean-room dai registri Maxwell | **P1** (rendering) |
| Exp bias delle resolve / NaN scrub | gears2-nx, masseffect-nx | `copy_dest_exp_bias` | non applicato (audit C2) | 2^n sulla luminosità | piena | stati di luminosità (STIMA) | basso | conteggio resolve con bias ≠ 0 | P1 (rendering) |
| Readback delle texture prodotte dalla GPU (`kTextureLock`) | nostro (B4) | `ExecuteTitleCommand` | restituisce la copia guest vecchia | il titolo legge i dati veri | piena | **stati di luminosità** se l'esposizione è letta dalla CPU (STIMA) | medio (sincronizzazione) | log di quali texture il titolo blocca | **P1** (rendering, prima la misura) |
| Gamma ramp al present | sa2-nx | `present.cpp` | non applicata | ±12–16 livelli | da verificare per GTA IV | luminosità globale | basso | confronto con PC | P2 |
| Varianti shader asincrone / fallback | renut-nx, gears2-nx | `genericDrops`, fallback "all interpolators" | 4 job uam che non finiscono mai; draw saltati (motivo 13: 565) | nessun draw saltato | buona | geometria/testo mancanti (STIMA) | medio | contatore motivo 13 = 0 | P1 (rendering) |
| Fetch dei vertici: endian degli indici, BLENDINDICES float | gears2-nx | fix in `gow2` | swap fisso 16/32 bit | modalità endian dal fetch | piena | **geometria frantumata** (STIMA) | basso | test offline su buffer noti | P1 (rendering) |
| Costanti catturate prima del rifiuto del draw (bug latente) | revisione locale | `native_front_end.cpp:128` vs `:184` | il delta si perde se il draw è rifiutato | delta sempre consegnato | – | correttezza (0 rifiuti nel run 44: latente) | basso | test offline | P1 (facile) |
| Loop e costanti intere degli shader | xbox360-nx, gears2-nx | traduzione con flusso strutturato | non verificato | – | – | **draw da 770 ms** (STIMA) | – | hash dello shader lento (run 45) | P1 (misura) |
| Quattro core: core 3 per audio/compilazioni | renut-nx (da NetherSX2_nx, MIT) | `__wrap_svcSetThreadCoreMask`, NPDM del forwarder | 3 core (0x7) | ~0,4 core liberato | richiede forwarder/NPDM | CPU (renut: "0.4 of a core") | medio | `cpu by core` (run 45) | P2 |
| Layout dei core (render thread su un core proprio) | sa2-nx | `RexSetGuestThreadCoreLayout` | guest per `cpu_index/2`, recorder flottante 0–2 a 0x3B (VERIFICATO nell'ELF) | – | buona | attese/prelazione (STIMA) | medio | `cpu by core` | P2 |
| Libreria deko3d release | nfsmw-nx, run 41 | – | run di misura con `libdeko3dd` | −30..45 % CPU del recorder (run 41) | piena | recorder | nullo | run con `theft4-dk.nro` | P1 (lancio C) |
| LTO + `-fipa-pta` + limiti dell'inliner | sa2-nx | `SA2_SWITCH_LTO`, `INLINE_AUTO 64` | non attivo | – | piena | sa2-nx: ~15 % (stima dell'utente) | basso; link lungo | A/B sulla console | P2 |
| PGO da una sessione sulla console | sa2-nx | `sa2_pgo.cpp`, `-fprofile-info-section` | non attivo | – | piena | sa2-nx: **~15 %** (README) | basso; ri-profilare dopo i cambi | A/B | P2 |
| Funzioni calde in testa a `.text` | nfsmw-nx, sa2-nx | `hot_functions.ld` | no | – | piena | non misurato isolato | basso (`crt0` primo) | A/B | P3 |
| Promozione delle chiamate indirette, meno `msr fpcr`, registri come locali | sa2-nx, gears2-nx | `tools/indirect_promotions.py`, `flush_mode_flow.py`, `leaf_locals.py` | no | – | buona (RAGE: molte virtuali, molto VMX) | sa2: 20–26 % dei campioni dopo `msr fpcr` | medio | verify mode | P2–P3 |
| Risoluzione dinamica, SMAA, FSR1 | renut-nx | `graphics_options.cpp` | FSR1 solo su PC | – | buona | GPU | medio | – | P3 |
| Arena GPU in cima all'heap, niente `mallinfo` nei thread caldi | renut-nx (battd_nx) | `plume_deko3d_region.h` | – | – | buona | stabilità memoria (heap OOM storico) | medio | contatori `mem:` | P2 |

## 4. Interventi ordinati

Un intervento per run. Ognuno ha un interruttore a runtime (09 §13), test offline prima della build e una prova sulla console solo per ciò che offline non si vede.

### R0 – Run 45: invio dal titolo più leggero e misura CPU contro parete (FATTO, misurato: 16 §R)

Dettagli in 16 §Q. File: hook (in **due copie**, `gta4-recomp/src` e `out/gta4-native-eu/src`), `native_front_end.*`, `native_constant_diff.h`, `dk_graphics_system.*`, `dk_recorder.*`, `tools/switch-game/main.cpp`.
**Successo:** fps in gioco > 1,1–2,0 alla stessa scena, chiamate per 10 s ~4–5 volte meno. **Rollback:** `nx_dk_skip_state_commands = false` (lancio B); il resto non cambia l'output (test).
**Esito (run 45 A):**
- Lo skip funziona: 2,14 comandi per draw contro 3,80.
- Gli fps **scendono** a 0,2–0,7. La misura CPU/parete mostra le attese: le chiamate campionate restano su una CPU solo il 35–45 % del loro tempo.
- Il lancio B non serve: lo skip non tocca lock né allocazioni, e B non separerebbe il batch pop dalla coda.
- La riga `cpu by core` non è utilizzabile (tutti zeri).

### R1 – Percorso dei pacchetti senza allocazioni (FATTO, misurato nel run 46: 16 §S1)

**Esito (run 46):**
- in gioco 3,2–5,3 fps, contro 1,1–2,0 del run 44;
- 100–167 mila pacchetti ogni 10 s;
- «the rest» del recorder sceso a 6,5 µs per draw.

Il freeze che resta è GPU, ed è un problema separato: lo tratta G4.

Com'è stato fatto, diverso dal piano qui sotto:
- Il produttore **sposta** il pacchetto in uno slot libero invece di costruirlo sul posto (meno codice nel front end).
- Le costanti viaggiano dentro il pacchetto fino a 16 registri per banco, con spill raro sull'heap; non c'è un buffer da 7,5 KB.
- Il recorder legge gli slot **sul posto**; il lock serve solo per dormire e svegliarsi.
- **Nessuna cvar**: il cambiamento è nel tipo del pacchetto, e il confronto si fa tra run 46 A e run 45 A.
- Test offline: 2 milioni di pacchetti, **0 allocazioni per pacchetto** su entrambi i thread (`test_packet_ring.py`).

Piano originale:

- `dk_graphics_system.{h,cpp}`: anello a capacità fissa di `FrontEndPacket` preallocati. Il produttore costruisce il pacchetto **sul posto**, nello slot e non in `scratch_`, e pubblica a lotti di 16 draw o a ogni evento. Il consumatore scambia i lotti. Nessuna `std::deque`.
- `native_recorder.h` / `native_constant_diff.h`: delta delle costanti in un buffer interno al pacchetto (256 + 224 registri al massimo: ~7,5 KB, o un'arena per frame). La capacità si riusa, niente `std::vector` nuovi.
- Test offline: un simulatore produttore/consumatore con 1 milione di pacchetti casuali verifica ordine, contenuto e il caso coda piena (attesa con timeout e log, regola 09 §1).
- **Successo:** `queue` < 0,3 s su 10 s; fps in gioco più alti a GPU < 50 %. **Rischio:** risvegli persi, mitigato dall'attesa a 2 ms già presente. **Rollback:** cvar `nx_dk_packet_ring = false`.

### R2 – Front end: lookup ai setter, stato fisso sul recorder, bug delle costanti

- `native_front_end.cpp`: hash degli shader, `used_texture_mask` e dichiarazione risolti in `kSetVertexShader`/`kSetPixelShader`/`kSetVertexDeclaration`. `DeviceState*` del dispositivo corrente in cache. Stream iterati secondo la maschera della dichiarazione. `DecodeFixedState` sul recorder, da una copia grezza.
- Bug latente: controlli di index buffer e stream **prima** di `CaptureConstants` (oppure `constants_valid = false` sul rifiuto).
- Test offline: front end vecchio contro nuovo sugli stessi comandi registrati (un trace del PC `tools/pc-eu-base`): pacchetti identici.
- **Successo:** front end per draw −50 % (dal campionamento CPU del run 45).

### R3 – Recorder: cache della geometria tra frame

- `dk_recorder.cpp` `Draw`: tabella (indirizzo, byte, formato) → copia convertita in memoria GPU. Ammessa alla seconda vista, hash XXH3 di controllo campionato, invalidata da `kResourceUnlock`/`kReleaseResource`, che il front end già riceve. Le store cards vengono dopo, come passata sul codegen.
- **Successo:** `vertices`+`indices` del recorder −70 %, recorder < 60 µs per draw. **Rischio:** geometria vecchia, mitigato dalla modalità verify con cvar.

### R4 – GPU: submit parziali e barriere minime; poi quattro core e layout dei thread

Il run 45 non ha dato dati per core: `svcGetInfo(ThreadTickCount)` risponde per core solo sul thread chiamante. La scelta dei core va decisa con un'altra misura, per esempio campioni del core corrente presi dal thread stesso.

### R5 – Build: libreria release per le misure, LTO, poi PGO da una sessione sulla console

### Binario rendering (in parallelo, un run alla volta, dopo R0)

| # | Problema | Prima la misura | Poi la correzione candidata |
|---|---|---|---|
| G1 | stati di luminosità (chiaro sovraesposto/blu con cunei) | log delle `kTextureLock` (quali texture, quanto spesso, se prodotte dalla GPU); exp bias delle resolve | readback delle texture GPU che il titolo legge; exp bias |
| G2 | geometria frantumata | **candidato (16 §S3, DA VERIFICARE):** i 60 vertex shader skinnati che uam non compilava sono tutti macchine a stati; strutturati compilano (run 47). Poi: quali shader saltano (motivo 13/14) e su quali draw; indici con endian diverso | varianti di riserva; endian dal fetch; bug delle costanti (R2) |
| G3 | fattore alpha di deko3d 0.5.0 | quanti stati di blend hanno fattori alpha ≠ colore | **2026-11-03: l'utente ha ottenuto da Chansey (autore di renut-nx) il permesso di copiare e riusare il codice di renut-nx.** Al posto della riscrittura clean-room si può quindi integrare la correzione di renut (`dkCmdBufReplayCmds` sui registri `IndependentBlend`). Nel codice copiato va indicata la provenienza (repository e commit). Lo stesso vale per il registro sRGB 0x56E e per il viewport capovolto. Condizioni del permesso oltre alla citazione: DA VERIFICARE con l'utente |
| G4 | draw da 770 ms nel filmato **e freeze in gioco (run 46)** | **causa trovata (16 §S3):** la macchina a stati `while(true){switch(pc)}` di XenosRecomp porta uam a 255 GPR e 92 KB di scratch per warp. **Run 47:** 183 shader strutturati (`hlsl_structure.py`). Misura precedente (run 45): VS `0872615BD9E7930D` + PS `2673E2AF61956ECD` 760–910 ms; VS `22F0AB2B742F2053` solo depth 140–700 ms (lato vertici); VS `636FBE61D31C549A` 70–200 ms. Manca il numero di vertici/indici del draw | cercare i 4 hash nel pacchetto; loop/costanti intere o conteggi sbagliati |
| G5 | lettere mancanti | dopo G2/G3 | – |

## 5. Automazione dei test sulla console (valutazione)

**Fattibile senza installare sysmodule e senza toccare Atmosphère** (VERIFICATO sui sorgenti di Sphaira e di nxlink):

1. In Sphaira: **FTP acceso** (menu Rete, porta 5000) e **nxlink già attivo per default** (`nxlink_enabled = true`).
2. Sphaira aperto in **modalità applicazione** (title takeover: tenere R avviando un gioco). Il gioco ha bisogno della memoria della modalità applicazione (03-memory.md). È l'unico passo manuale per sessione.
3. Dal PC: FTP per cancellare `runtime-cache` e `captures`, caricare `game.toml` e NRO; `nxlink.exe -a IP -p /switch/theft4/theft4-dk.nro` carica e avvia; attesa della porta FTP (chiusa = il gioco gira, riaperta = il gioco è uscito); FTP per scaricare log, catture e `crash_reports`; analisi.
4. Due cvar nostre (da scrivere, P1 per l'automazione): `nx_auto_exit_s` (uscita pulita dopo N s, stesso percorso del `+`) e `nx_input_script` (input temporizzati iniettati in `nx_input.cpp`, run ripetibili).

**Attenzione (DA VERIFICARE):** un NRO lanciato da nxlink nel processo di Sphaira usa un loader diverso dal forwarder Theft4 (maschera dei core, spazio di indirizzi): rifare la base una volta e non mescolare i due percorsi nei confronti.

**sys-botbase:** utile ma non necessario. Upstream v2.5 dichiara HOS 21; il fork masagrator `2.6-kill` dichiara 22.0.0 e aggiunge lettura file e `killGame`, nessuno dei due dichiara HOS 23. È una porta TCP di lettura/scrittura della memoria senza autenticazione sulla LAN. **Da installare solo con il consenso esplicito dell'utente**; si rimuove cancellando `atmosphere/contents/430000000000000B`.

**Xenia:** non utile ora. Le tracce sono a livello PM4, mentre i nostri bug sono nel percorso dei comandi del titolo, che non passa per PM4. La nostra build PC (`tools/pc-eu-base`) è il riferimento giusto e l'SDK ha già trace e dump degli shader.

Script proposto, da scrivere dopo le due cvar: `tools/console-loop/run_loop.py` (Python, solo libreria standard: `ftplib`, `socket`, `subprocess`).

1. **preflight** (30 s): porta 5000 raggiungibile; `COMMIT` senza `-dirty`; SHA-256 dell'NRO in `run.json`.
2. **prepare** (FTP, 120 s): cancella `runtime-cache` e `captures`; salva i vecchi log; carica `game.toml`.
3. **launch**: nxlink con `-r 20`, timeout 300 s, un solo nuovo tentativo.
4. **wait**: la porta 5000 deve chiudersi entro 60 s, poi riaprirsi entro avvio + durata + 120 s. Altrimenti esce con "serve una persona". Con sys-botbase, nel caso, `press PLUS` e poi `killGame`.
5. **collect** (FTP): log, PNG, nuovi `crash_reports`.
6. **analyse/compare**: le righe di §6, una tabella fra due run, luma/PSNR delle PNG nelle scene fisse.

## 6. Procedura di validazione (ogni run)

Confronto fra build precedente e nuova, con la stessa scena, gli stessi clock letti e la stessa cache iniziale (`run-protocol.md`):

| Misura | Riga del log |
|---|---|
| fps, draw per frame, draw saltati per motivo | `deko3d: N frames/s, M draws per frame; totals` |
| tempo dei thread del titolo (front end, coda, Present), recorder occupato/in attesa, **CPU dei campioni** | `threads in the last` |
| GPU per pass, lavori lenti con shader | `GPU time from timestamps`, `slow work` |
| CPU per thread e **per core** | `cpu`, `cpu by core` |
| memoria | battito `host memory` |
| errori shader | `shaders: … failed, … compiling` |
| qualità | PNG `captures/` misurate (luma, pixel scuri/saturi, B/R) e aperte |
| stabilità | `game-crash.log`, anello dei fault, passi dell'uscita |

Ogni esito si classifica VERIFICATO / DA VERIFICARE / STIMA. *Campione unico: nessuna differenza è dimostrata* finché la stessa build non ha almeno due sessioni.

## 7. Rischi trasversali

- **Copie degli hook:** la console compila `out/gta4-native-eu/src`. Ogni modifica agli hook va in entrambe le copie finché il censimento non è riconciliato (16 §Q1).
- **Licenze:** niente codice da reNut/renut-nx. Codice GPL-3.0 o BSD-3 solo con attribuzione.
- **Una variabile per avvio** (09 §13), sempre con un interruttore a runtime.
