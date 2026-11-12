# 20. Migrazione a renut_nx_ultimate_base: audit, mappa delle differenze, piano

Iniziata il 2026-11-12, su indicazione di ChanseyIsTheBest: rebase completo sulla Ultimate Base, recupero di tutte le
funzioni specifiche del renderer di Theft4-NX, codice rigenerato, machine copies ricreate, impostazioni predefinite
della base mantenute (sono già calibrate per accuratezza e prestazioni). Questo documento è l'audit (fase U0) e il
piano; i risultati di ogni fase si aggiungono in fondo (§6) e nella matrice `19-optimization-matrix.md`.

Fonti lette per questa pagina: `renut_nx_ultimate_base` commit `8de5d51a` (2026-11-13, "Ports 123-128 and an index for
docs/"), clone in `<refs>/renut_nx_ultimate_base`: `README.md`, `docs/README.md`, `docs/hand-and-machine-optimising-guide.md`
(tutta), `docs/porting-a-360-game-prompt.md` (tutta), `docs/building.md` (§1-4), le righe di `docs/ultimate-fixes.md`
che toccano EDRAM, tiling, resolve, MSAA/SMAA, risoluzione dinamica, cache DKSH, LZ4; `app/CMakeLists.txt` (struttura);
gli include del renderer.

## 1. Stato trovato (U0)

- Branch `sync-clean` a `7688d55` = `origin/sync-clean`; albero pulito (non tracciati: `dk_recorder.cpp` alla radice e la
  voce `C:/` di sempre, lasciati come sono).
- **Checkpoint:** tag annotato `checkpoint/pre-ultimate-base-2026-11-12` su `7688d55`, pushato su origin. Per tornare a
  questo stato: `git switch -c <nome> checkpoint/pre-ultimate-base-2026-11-12` (nessun reset necessario).
- Run 72 (`5d542d6`, lavori di sfondo di ReNut dal core 2 al core 0) pronto e non ancora eseguito: resta valido come
  misura del vecchio albero e come controllo per la base.

## 2. Mappa delle differenze

### 2.1 Struttura

| Parte | Theft4-NX ora | Ultimate Base `8de5d51a` | Distanza misurata |
|---|---|---|---|
| SDK | `glue/rexglue-sdk-main`, ReXGlue **0.9.0** con l'emulatore GPU di Xenia, il layer Horizon di nfsmw-nx e i nostri cambi (memoria, thread, audio, profiler) | `sdk/`, ReXGlue **0.10.0** senza l'emulatore GPU, layer Horizon di nfsmw-nx esteso (governor della memoria, attese su indirizzo, LZ4 a blocchi, apm, perf, saltynx, sysclk) | `src/`: 569 file, +33.643 / -143.408 righe (ignorando i fine riga); contro 0.9.0 pulita: 210 file, +32.790 / -6.778 |
| Generatore e passate | generatore 0.9 + `tools/switch-codegen` (`direct_calls.py`, `gaps.py`, `read_before_write.py`), `tools/renut-gta4/weak_calls.py` | generatore 0.10 della base + `tools/post_codegen.py` (flush-mode flow, float singoli, forme vettoriali, dcbz come DC ZVA, stack forwarding, store cards, liveness dei registri, machine copies) | da rifare tutto: le machine copies esistono solo sul generatore della base |
| Renderer | `thirdparty/renut/renut_engine` = sa2-nx `adf3a4e` + ~1.000 righe GTA IV (11 commit, `df941c7..cc64728`) | `app/src/renut_engine` = katamari-nx 1.0.1 + 128 port delle altre conversioni | sa2-nx -> base: 139 file, +32.286 / -4.778 (`render/draw.cpp` da solo +13.496) |
| plume | sa2-nx + 9 file nostri (D3D12 stencil-plane views, deko3d MS non compressi / decay) | plume della base | sa2-nx -> base: 9 file, +3.026 / -146 |
| Traduttore shader | reblue-XenosRecomp di sa2-nx + 2 file nostri (NORMAL1/TANGENT1, DXC) | quello della base (fetch spec, tessellation, literal indicizzati, vertex fetch raw...) | 5 file, +735 / -25 |
| App del gioco | `tools/switch-game` (main, `nx_profiler`, `nx_audio_mixer`/routes, `nx_clocks`, `nx_threads`, `nx_input`), `tools/renut-gta4` (layout, device 6274, compat, shader) | `app/` di Katamari (manifest, chiavi, `cpu/hot`, natives, tomls, PGO) da sostituire col gioco | il "game layer" si rifà da zero sul modello della base |
| Strumenti | profiler e letture nostri (`where.py`, `guest_profile.py`), estrazione dei 1.370 contenitori shader | `profile_breakdown.py`, `switch_hot_pcs.py`, `switch_hot_bulk.py`, `deko3d_variant_cache.py`, `switch_shaders.ps1`, `lz4b_pack.py`, `pgo_merge.py`, `cc_*_census.py`, difftest | si adottano quelli della base; i nostri restano dove la base non ha l'equivalente |
| Toolchain | VS (`D:`), clang di VS, Python `D:/Python` 3.14, devkitA64 GCC 16.1 | VS 18 Build Tools, clang 22 (MSVC ABI), CMake 4.x, Ninja 1.13, Python con `xxhash` e `Pillow` | VS installato è 18 (MSVC 14.51, clang 22.1.3, CMake 4.3.1, Ninja 1.13.2): compatibile. Mancano i moduli Python `xxhash`, `Pillow` |

### 2.2 Funzioni specifiche di Theft4-NX da conservare e dove vanno nella base

| Funzione (commit) | Dove sta ora | Equivalente nella base | Azione |
|---|---|---|---|
| Layout del device 2.0.6274 (22.400 byte; zone N&B, -8, -16, -20) | `tools/renut-gta4/gta4_d3d.h`, `thirdparty/renut/.../d3d.h`, `d3d_native.h` (`50212a0`) | `d3d.h`/`d3d_native.h` a Katamari 2.0.5632; `docs/xdk-notes.md` per gli XDK diversi | riportare i valori 6274 (verificati da `tests/renut/test_gta4_d3d.py`, 65 istruzioni in 21 funzioni) |
| Hook D3D agli indirizzi EU (CreateDevice, KickOff, BeginRingBig, BlockOnFence, Swap, Draw*, Resolve, SetPredication, InsertCallback, Create*Shader, ClearF, Destroy...) | `gpu/d3d/hooks/*.cpp`, `gta4_d3d.h` X() table | hook della base agli indirizzi di Katamari; alcuni corpi "aspettano la funzione del gioco" (`ultimate-fixes.md`) | rilegare ogni hook agli indirizzi di doc 18 §2 |
| BeginTiling / EndTiling / coda di EndTiling `829DDF28` (blocco di tile senza attesa) | `hooks/draw.cpp`, `hooks/device.cpp` (`96f8261`) | port 87 "tiles once" (vivapinata-nx): ogni buffer registrato eseguito una volta per tile predicata | usare il meccanismo della base; il nostro hook della coda resta (GTA IV non è Viva Piñata: confronto PC) |
| Creazione del device per 6274 (percorso senza hardware) | `tools/renut-gta4/gta4_renut_device.cpp` | `hooks/device.cpp` della base | portare il file nel game layer |
| Formati colore Xenos 0xA / 0xC come 2 / 3 (HDR 7e3) | `render/format.cpp` (`efe507d`) | port 106 (clamp 7e3), port 75 (formati RGB10A2 / R11G11B10) | confronto: tenere il nostro mapping solo se la base non lo copre |
| Ripristino depth da colore (EDRAM) | `MaybeRestoreDepthLocked`, `depth_restore_ps.hlsl` (`6921903`) | port 29 `sasrt_renut_depth_restore` + port 54 (candidati) | provare quello della base sulla scena di apertura; il nostro resta solo se diverso |
| Metà dei tile di depth EDRAM scambiate (40 campioni) | `depth_restore_ps.hlsl` (`00683a5`) | non trovato nella base | **da riportare** (specifico del layout di depth di GTA IV); handoff alla base se universale |
| Resolve multisample disegnati, target MS non compressi su deko3d, descrittori "decayed" | `render/draw.cpp`, `resolve_ms_ps.hlsl`, `plume_deko3d*` (`1846a14`, `e6d8b94`) | port 88 (resolve dall'ultimo), 115 (resolve di depth MS rimandato), 127 (MSAA ridotto da `msaa_level` mantenendo i trucchi EDRAM), port 09/45 (depth decompressa prima della lettura) | con `msaa_level` 1 (default della base) la compressione MS non si presenta; **verificare** se GTA IV ha bisogno di MS per compatibilità (§4) |
| Zcull invalidato dopo il ripristino della depth | `1846a14` | plume della base (`banjo_deko3d_zcull`) | riportare se la base non lo fa |
| GuestPtr tradotto con la tabella guest dello Switch | `ebc999f` | layer Horizon della base (`guest_memory_switch.cpp`) | verificare: probabilmente coperto |
| Rect dei resolve ritagliato, vsync D3D12 spento, sistema grafico nullo per il vblank | `4c0eb6c` | da confrontare | caso per caso al primo avvio PC |
| Profilatore console per thread/funzione a 1 kHz, thread di ReNut inclusi | `nx_profiler.cpp`, `where.py`, `guest_profile.py` (`5d542d6`) | campionatore dell'SDK (`stacks_profile.flag`), `profile_breakdown.py`, `switch_hot_pcs.py` | usare quello della base (è quello che alimenta `switch_hot_bulk.py`); il nostro solo se manca qualcosa (GPU per passata: `renut_gpu_profile` è del renderer, resta) |
| Routing audio nativo, XMA asincrono, priorità a scadenza | `nx_audio_mixer`, `nx_audio_routes.h` (run 49, 64) | XMA frame cache, kick XMA senza lock (spento), attese su indirizzo | misurare la base da sola, poi riportare il routing solo se serve |
| Hook di gioco non D3D (aspect, font, distanza, limitatore) | vecchie build | - | dopo: non dipendono dal renderer |
| Stub di link (vk_icd, `RexSetGuestThreadCoreLayout`), `chdir` a `sdmc:/switch/theft4` | `gta4_renut_compat.cpp`, `main.cpp` | `switch_vulkan_stubs.c`, `switch_elf_stubs.c` della base | probabilmente coperti |
| Estrazione dei 1.370 contenitori shader da `common.rpf` + xex | strumenti run 61 | `renut_shader_dump.py` (solo xex) + un "file finder" per gioco (`katamari_files.py`) | il nostro estrattore diventa il file finder di GTA IV |

### 2.3 Impostazioni predefinite della base da mantenere

- `msaa_level` 1 (MSAA spento), `banjo_renut_smaa` acceso: SMAA gira solo dopo il draw il cui pixel shader è in
  `banjo_renut_smaa_after_ps` (la composizione finale prima dell'HUD, da trovare in gioco con
  `banjo_renut_diag_draw_log_s`). Finché non è impostato, la riga dei 10 s dice "SMAA ... 0 frames".
- Le chiavi di `docs/off-by-default.md` §1 restano spente salvo un A/B su GTA IV che mostri lo stesso difetto.
- Stack dei thread guest da 8 MB (`switch_guest_stack_mb`).
- Liveness dei registri delle machine copies: valida solo dopo `cc_entry_census.py` ("none") e `cc_exit_census.py`
  su GTA IV.
- `switch_sysclk.cpp` esiste nell'SDK della base: **nessun overclock** (richiesta dell'utente). Da verificare che sia
  spento di default e lasciarlo spento.

## 3. Decisioni di struttura

1. **La base entra nel repository in `ultimate/`**, con lo stesso schema usato per ReNut (`df941c7`): primo commit con i
   file invariati e la provenienza, poi ogni modifica GTA IV in commit propri (`git log -p ultimate` le mostra tutte).
   L'albero attuale (`glue/`, `thirdparty/renut`, `tools/switch-game`) resta il riferimento funzionante e la fonte del
   controllo (run 70-72) finché la base non disegna la stessa scena sulla console; non viene cancellato.
2. **Le librerie di terze parti dell'SDK** (`sdk/thirdparty`, 372 MB: FFmpeg, simde, SDL3, glslang, Vulkan, tracy...)
   non si copiano nel repository: uno script le prende dal commit fissato della base (`8de5d51a`) e le mette in
   `ultimate/sdk/thirdparty` (ignorata da git). Le loro licenze restano quelle del commit fissato.
3. **Nessun `git reset`, `clean`, checkout distruttivo**; file aggiunti esplicitamente; nessun commit di codice non
   compilato e provato; NRO, ELF, dati del gioco, cache, log mai nel repository.
4. **Ordine:** prima il PC (genera il codice, costruisce il traduttore), poi gli shader, poi la NRO, poi la console; ogni
   ottimizzazione dopo una scena giusta sulla console, misurata contro un controllo della stessa sessione.

## 4. Piano per fasi

Ogni fase ha la sua verifica; nessuna si dichiara fatta senza la sua prova (PC e console distinti).

| Fase | Lavoro | Verifica |
|---|---|---|
| **U0** | Audit, checkpoint, questo documento | tag su origin; documento pushato |
| **U1** | Import della base in `ultimate/` invariata; script per `sdk/thirdparty`; README di provenienza, licenze | hash degli oggetti uguali a `8de5d51a` (script di confronto) |
| **U2** | Build host: generatore 0.10 (`rexglue.exe`), toolchain del PC; moduli Python `xxhash`, `Pillow` | `rexglue.exe --help`; build host senza errori |
| **U3** | Game layer GTA IV, parte 1: manifest dal nostro `gta4_eu_base_manifest.toml`/`config`/`gaps`, via Katamari (chiavi, liste `cpu/hot`, `hand.txt`, `indirect_promotions.txt`, `pgo/`, tomls); generazione + `post_codegen.py` completo | log di ogni passata; `cc_entry_census.py` = none, `cc_exit_census.py` letto voce per voce |
| **U4** | Rilegare il renderer a GTA IV: layout 6274, hook agli indirizzi EU, device 6274, tiling, chiamate dirette (equivalente di `weak_calls.py` se il generatore 0.10 ha lo stesso problema) | `test_gta4_d3d.py` contro il nuovo `d3d.h`; avvio PC fino al primo Swap |
| **U5** | Fix GTA IV del renderer (§2.2), ognuno confrontato con l'equivalente della base | scena di apertura sul PC uguale ai riferimenti R3 (`run65-logs/captures`, catture PC di R3) |
| **U6** | Shader: i 1.370 contenitori con `switch_shaders.ps1` della base; cache DKSH offline (`deko3d_variant_cache.py`) dalla `pipeline_list.bin` della console | 0 errori di traduzione/compilazione; chiavi della cache = varianti della console |
| **U7** | NRO della base per GTA IV (deko3d, NVK non collegato), everyday + guard | NRO collegata; ELF con shader host; Ryujinx se disponibile |
| **U8** | Primo run console sulla base: stessa scena e percorso dei run 70-71 | scena giusta; fps, GPU per passata, audio, stalli contro il run 71/72 |
| **U9** | Impostazioni di default sulla console: SMAA dopo la composizione finale di GTA IV; prove 720p+MSAA, 720p+SMAA, 540p+SMAA, DRS 540-720p+SMAA | tempo GPU, fps, pacing, catture della stessa scena |
| **U10** | CPU: machine copies dai profili console (run 71-72 + base) e dai conteggi PGO, in place; guard sul PC; PGO + LTO; natives a mano solo oltre il cancello della guida (candidati: mixer `sub_821737C0`, `sub_8216BFE8`, `sub_82185E38`) | guard 0 differenze ripetute; profilo console prima/dopo con rumore dichiarato |
| **U11** | Audio e thread: XMA frame cache, attese su indirizzo (contesa di `XEvent::Set`), spinlock del thread 17; core solo con misura | righe `audio:` al 100 % del tempo reale, nessuna uscita a secco |
| **U12** | Installer web (renut_nx_ultimate_installer): LZ4 a blocchi della base + ZXC da valutare per tipo di asset | decoder verificati, integrità, compatibilità con le installazioni esistenti |
| **U13** | RAM 1600 MHz: esperimento separato, solo se i dati mostrano un limite di banda | A/B stock/1600, temperature, stabilità |

### 2.4 Layout del device: Katamari (2.0.5632) contro GTA IV (2.0.6274)

Confronto dei valori di `ultimate/app/src/renut_engine/gpu/d3d/d3d.h` con quelli verificati per GTA IV
(`thirdparty/renut/.../d3d.h`, `tests/renut/test_gta4_d3d.py`): **uguali** creation thread id 10888, title process 10892,
dichiarazione dei vertici 11812, index buffer 12428, target 12432, depth 12448, stream 12452 / stride 12520, texture
12536, pixel / vertex shader 12684 / 12688, predicazione 12700, snapshot 12704, fine del ring dei vertici 13428, fence
tag 10908 / 10928, flag dello Swap 10941. **Diversi** (cinque):

| Costante | Katamari | GTA IV |
|---|---|---|
| `kDeviceAllocSize` | 0x5700 (22.272) | 22.400 |
| `kDeviceOffsetBehaviorFlags` | 22.264 | 22.280 |
| `kDeviceOffsetCommandBuffer` | 13.492 | 13.500 |
| `kDeviceOffsetPresentFenceTag` | 0x3A44 (14.916) | 14.924 |
| `kDeviceOffsetSwapCount` | 0x4098 (16.536) | 16.544 |

U4 parte da qui: le zone della coda (+8 / +16 rispetto a Katamari) si riprendono da `gta4_d3d.h`, e
`test_gta4_d3d.py` si fa girare sul nuovo `d3d.h`.

## 5. Rischi noti

- **Layout e XDK:** la base è a 2.0.5632 (Katamari), GTA IV a 2.0.6274: ogni offset si riprende da `gta4_d3d.h`, non si
  assume.
- **Generatore nuovo:** il codice generato cambia tutto; i nostri gap, le promozioni indirette e le correzioni del
  generatore 0.9 (`tools/switch-codegen`) vanno riverificati sul 0.10 (`docs/codegen-crash-fixes.md`).
- **Default della base che cambiano il quadro:** MSAA spento. GTA IV disegna il G-buffer in EDRAM con resolve
  multisample (runs 68-70): `msaa_level` 1 deve dare la stessa scena (port 127 tiene "i trucchi EDRAM"); se non la dà, il
  multisample necessario alla compatibilità si distingue da quello di sola qualità (richiesta dell'utente) e si decide
  con un A/B.
- **Spazio su disco:** 24 GB liberi su C: al momento dell'audit; la generazione e le build della base vanno controllate
  prima di partire.

## 6. Risultati per fase

| Fase | Data | Commit | Risultato |
|---|---|---|---|
| U0 | 2026-11-12 | `b084426` | checkpoint `checkpoint/pre-ultimate-base-2026-11-12` -> `7688d55` su origin; audit e piano |
| U1 | 2026-11-12 | `ebd1da4`, `d35f7b5` | base `8de5d51a` in `ultimate/` (4.484 file): ogni albero confrontato con gli hash del commit (`app`, `tools`, `docs`, `dist`, `shaders`, `sdk/{src,include,cmake,switch_compat,tests,resources}`, file della radice): **identici**. `sdk/thirdparty` (18.874 file) da `tools/ultimate/fetch_sdk_thirdparty.ps1`, ignorata da git; `pgo/` di Katamari non importata. `tools/ultimate/env.bat` = `pc_env.bat` della base con `THEFT4_VS_ROOT` / `THEFT4_PYTHON` (nessun percorso della macchina nel repository). Moduli Python `xxhash` 4.1.0 e `Pillow` 13.0.0 installati in `THEFT4_PYTHON` |
| U2 | 2026-11-12 | (sotto) | **Difetto della base trovato:** un clone pulito non compila l'SDK: `FFmpeg/config.h`, `libavutil/avconfig.h` e `libavutil/ffversion.h` mancano dal commit (li esclude il `.gitignore` di FFmpeg stesso). Ricostruiti in `tools/ultimate/ffmpeg/` (il dispatcher del fork Xenia a `0604b46` + ramo `__SWITCH__` -> `config_switch_aarch64.h` + `config_masseffect_wmv3.h` dopo ogni piattaforma, come dicono i commenti della base); lo script di fetch li copia. Da segnalare alla base (handoff) |
