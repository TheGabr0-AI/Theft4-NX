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

1. **NRO sovrascritto:** copiare `switch-game.nro` della cartella del run su
   `sdmc:/switch/theft4/`, sovrascrivendo. Nella schermata di avvio (10 s,
   cvar `nx_startup_summary_s`) il commit deve essere quello del file `COMMIT`
   della cartella del run, senza `-dirty`.
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

**Smoke:** `switch-smoke.nro` della stessa cartella del run fa anche il
passo 8 (deko3d contro NVK, `13-deko3d-spike.md`, ~1–2 minuti, schermo fermo
sull'ultima immagine della sonda Vulkan); il risultato sono le righe `gpu
bench:` di `smoke.log`, da copiare nella cartella del run.
