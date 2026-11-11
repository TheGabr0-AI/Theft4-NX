# Protocollo ridotto dei run (dal run 30)

Vale per ogni run sulla console finché non viene sostituito. Nasce dai run
27–29 (06 §29): NRO forse vecchia, cache non cancellata, clock imposti da
Horizon OC (CPU 918 / GPU 192 MHz invece di 1020 / 460,8), e niente tempo per
3 avvii da 15 minuti.

**Un solo avvio per configurazione**, **5 minuti dall'ingresso in partita**
(Niko che si muove), poi uscita con + tenuto 1 s. Log in
`out/console-runs/runNN/` (`game.log`, `game-crash.log`, `game-exit.log`) e,
nella stessa cartella, una riga in `note.txt`: cosa si è visto, quando si è
entrati in partita, cosa si è premuto.

**Tre controlli prima di ogni avvio:**

1. **NRO sovrascritto:** copiare l'NRO della cartella del run su
   `sdmc:/switch/theft4/`, sovrascrivendo (fino al run 34 `switch-game.nro`;
   dal run 37 l'NRO ha il nome della variante: `theft4.nro` NVK,
   `theft4-dk.nro`, `theft4-dk-debug.nro`, fatti da
   `tools/switch-game/build_variants.sh`, e lo stesso nome nel menu homebrew).
   Nella schermata di avvio (10 s, cvar `nx_startup_summary_s`) il commit deve
   essere quello del file `COMMIT` della cartella del run, senza `-dirty`
   (run 37: `17f3a18-dirty`, costruito senza commit: dichiarato nel devlog).
2. **Cache cancellata:** cancellare `sdmc:/switch/theft4/runtime-cache`
   (intera). Nella schermata: `cache files: runtime-cache 0`. Se un run vuole
   la cache calda, lo dice il suo piano.
3. **Horizon OC (o sys-clk) disattivato o con il profilo di default:** nella
   schermata `clocks read` deve dare quello che dice `clocks wanted`; una riga
   rossa `CLOCKS READ != WANTED` vuol dire che il run non è confrontabile.

**Dichiarazione obbligatoria** in ogni analisi di questi run: *campione unico:
nessuna differenza è dimostrata.* Un numero diverso tra due run è un indizio da
ripetere, non un risultato; si confrontano solo run con la stessa NRO, la
stessa cache iniziale e gli stessi clock letti.

**Dal run 31** (06 §30 D): i default di `sync-clean` sono la configurazione
del run 28, quindi `game.toml` può restare senza righe; ogni cvar si spegne
ancora una a una dal toml. Run 30: Horizon OC risultava `RUNNING` ma i clock
letti erano quelli voluti per tutto l'avvio: è il caso "profilo di default",
che vale. La cache invece non era cancellata (controllo 2): annotarlo se
succede di nuovo.

**Dal run 45: più avvii con la stessa NRO.** Un run può avere gli avvii A, B, C… con la
stessa NRO. Ogni avvio differisce da A per **una sola** riga del `.toml` (in genere
l'interruttore della novità), in sottocartelle `NN-logs/A/`, `B/`, … del run.
Prima si fa A; B e C sono facoltativi e si fanno solo se c'è tempo, e solo se la loro differenza da A può rispondere alla domanda aperta. Esempio, run 45: B spegneva lo skip, ma la regressione stava altrove, quindi non è servito (16 §R1). Se c'è un solo avvio, i log vanno direttamente in `NN-logs/`. Prima di ogni
avvio: SHA-256 della NRO sulla scheda contro quello del devlog, `captures/` vuota, log
vecchi rinominati.

**Misure da leggere per prime** (16-deko3d-devlog.md §P, 17-optimization-roadmap.md
§6): `threads in the last` (thread del titolo: front end, coda, Present; recorder:
attesa/lavoro; CPU dei campioni), `GPU time from timestamps`, `cpu` e `cpu by core`,
la riga dei frame/s. Le PNG si misurano tutte e alcune si aprono.

**Dal run 56: benchmark e profilo nella stessa sessione.** I benchmark ufficiali solo a CPU 1020 / GPU 460,8 /
memoria di serie, senza profili sys-clk/Horizon OC (righe `clocks:` di ogni heartbeat). Il campionatore parte dopo
`nx_profile_start_s` secondi (run 56: 420): le finestre prima della riga `profile: sampling starts now` sono il
benchmark, quelle dopo il profilo (`where.py --stacks`); i frame «anatomia» (`run 56 anatomy frame ... (not a benchmark
frame)`) si escludono dalle finestre. Run con numero di draw per frame molto diverso si confrontano solo normalizzati
(draw/s, µs per draw), mai in fps.

**Automazione (valutata, non ancora attiva; 17 §5):** con FTP e nxlink di Sphaira
(Sphaira in modalità applicazione) il PC può caricare la NRO, avviarla e
riprendersi log e catture senza sysmodule nuovi. Mancano due cvar del gioco
(`nx_auto_exit_s`, `nx_input_script`) e lo script `tools/console-loop/run_loop.py`.
Gli avvii fatti da nxlink usano un loader diverso dal forwarder: base nuova, mai
mescolati nei confronti. sys-botbase si installa solo con il consenso esplicito
dell'utente.

**Dal run 66: NRO ReNut** (`theft4-renut.nro`, `build_variants.sh renut`, `18-renut-migration.md` §6c-§6d). Gli
shader del gioco sono dentro l'NRO: sulla scheda vanno solo l'NRO e `game.toml` della cartella del run (niente
`deko3d/`). Prima di ogni avvio, oltre ai tre controlli: cancellare `sdmc:/switch/theft4/deko3d_shaders.bin` (la cache
DKSH che l'NRO scrive; il primo avvio compila le varianti sulla console e fa parte della misura). La riga
`nx_gpu = vulkan, but this build has no Vulkan backend` è innocua: questa NRO usa sempre ReNut. Log in più da
riportare: `deko3d.log` e `game.prev.log` (l'avvio precedente). Un crash si legge con `addr2line -f -C -i -e
switch-game-renut.elf <pc - base> <lr - base> <caller...>` sull'ELF della stessa cartella: `game-crash.log` dà gli
indirizzi già meno la base del modulo. Un `far` meno la base dell'arena guest (riga `memory_switch: guest arena` di
`game.log`) maggiore di 0xB01FFFFF è un indirizzo fuori dall'arena: un VA guest tradotto senza `rex_guest_table`
(run 66). Dal run 67 la cartella del run ha anche `README.txt` e `SHA256SUMS.txt`; l'ELF serve solo sul PC.
Un `ERROR TERMINATION: std::terminate (uncaught ...)` senza fault (run 67) non ha indirizzi da leggere: il thread che
termina è quello con lo stesso id `t...` della riga `export stubs at exit`; si risale all'ultima riga di quel thread e
alle funzioni che possono lanciare lì (sull'ELF, `objdump -d`: un `std::system_error` "Function not implemented" su
Switch è una chiamata pthread senza implementazione in libnx, per esempio `pthread_detach`). **Dal run 68:** con
`gta4_renut_capture_s` acceso, l'uscita con `+` deve scrivere `gta4_renut_capture_s: capture thread joined in N ms`;
senza quella riga l'uscita non è passata da `ExitGame`.

**Dal run 68 (il gioco arriva al gameplay): run grafici ReNut.** Le catture (`gta4_renut_capture_s`) sono la prova
principale: si guardano tutte e si dice per ciascuna cosa c'è e cosa manca, separando le parti **forward** del frame
di GTA IV (cielo, luna, finestre dei LOD lontani, acqua, HUD/minimappa) da quelle **differite** (suolo, palazzi vicini,
personaggi, auto: G-buffer 2x in tre tile, ricostruzione della depth, illuminazione). Un bordo nero che segue
l'orizzonte e si sposta con la telecamera non è un viewport tagliato: è la parte differita. Il confronto si fa con il
PC (D3D12) allo stesso punto del filmato iniziale (catture ogni 10 s; il numero di frame nel nome **non** identifica la
scena, dipende dagli fps). Con `banjo_renut_diag_resolve_trace = true` le righe `[trace]` di un frame ogni 30 s danno,
per ogni resolve e cambio di target, quanti texel non sono zero e le medie dei byte: il primo passaggio che diventa
nero è lì (le letture dei target multisample del PC escono a zero anche quando la scena è giusta: lì valgono solo
quelle 1x e delle texture). Righe da cercare per prime: `multisampled colour resolves drawn`, `restored from colour`,
`copy and 2D engines` (con il resolve disegnato i `blits` devono restare quasi fermi). Gli fps del primo avvio
contano anche le compilazioni uam (`uam N compiles (avg ...)`): si valutano a parte.

**Dal run 70: la scheda per le NRO ReNut** (tabella completa in `18-renut-migration.md` §6h). In `sdmc:/switch/theft4/`
si **tengono** `deko3d_shaders.bin` (cache DKSH delle varianti compilate da uam: con la cache calda il run 69 ha
compilato 5 varianti contro le 295 del run 68 a freddo) e `pipeline_list.bin` (le pipeline da precostruire all'avvio):
li scrive l'NRO ReNut stessa, si invalidano da soli se il compilatore cambia, e restano validi finché il pacchetto degli
shader del gioco non cambia. Si cancellano solo se una NRO cambia il pacchetto o uam (lo dice il README del run).
`deko3d_glsl.bin` accanto all'NRO **non** deve esserci (sostituirebbe il pacchetto incluso). Ignorati da ReNut:
`deko3d/` (vecchio renderer), `runtime-cache/` (vecchie build Vulkan), `cache/` e `cache1/` (non montate). Prima di ogni
avvio: log vecchi rinominati, `captures/` vuota. Le finestre a 2-5 fps che coincidono con i frame tracciati
(`banjo_renut_diag_resolve_trace`) sono il costo della misura, non del gioco.

**Smoke:** `switch-smoke.nro` della stessa cartella del run fa anche il
passo 8 (deko3d contro NVK, `13-deko3d-spike.md`, ~1–2 minuti, schermo fermo
sull'ultima immagine della sonda Vulkan); il risultato sono le righe `gpu
bench:` di `smoke.log`, da copiare nella cartella del run.
