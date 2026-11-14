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

### 2.5 U4: quello che i due header non dicono (2026-11-12)

- **Offset in linea.** Oltre a `d3d.h` / `d3d_native.h` (U4a, `d3bf040`: 10 valori, test a 0 differenze), i corpi
  degli hook della base scrivono offset del device in linea, quasi tutti annotati `katamari-nx: <N&B> -> <Katamari>`.
  `tools/ultimate/device_offset_census.py` li mappa a GTA IV con le zone del doc 18 §2 (N&B fino a 10.943; −8 da 11.820
  a 13.180; −16 da 13.232 a 16.728; −20 da 21.532) e cerca il valore GTA IV come spiazzamento nella libreria D3D di GTA
  IV (829C7000-829E4600) del codice 0.10: **144 offset, 56 diversi per GTA IV, 18 in zone non mappate, 22 senza prova**
  (`ultimate-reports/u4-device-offsets-base.txt`). La maggior parte sta nel percorso di **creazione nativa** del device
  della base (XenonGpuInitialize, InitializeApiState, InitializePresentationParameters, ConfigureForOnePassZPass); nei
  percorsi che GTA IV esegue con i suoi 23 hook ci sono `SetFence` (N&B 21.532 / 14.920 -> GTA IV 21.512 / 14.904, gli
  stessi valori del renderer funzionante), la rampa gamma (14.988), il conteggio dei flip in Swap (16.552) e i campi
  del tiling (12.720 / 12.724).
- **Due percorsi del device.** La base crea il device in modo nativo, come Katamari / N&B (hook su
  XenonGpu_Initialize, InitializeApiState, InitializePresentationParameters, Uninitialize, InsertCallback, Present,
  i setter di stato...). Il renderer funzionante usa il percorso di sa2-nx (`Sa2CreateDevice`: l'originale
  `Direct3D_CreateDevice` senza hardware, poi i passi dell'hardware path di 6274: flag 10942, gestore delle interruzioni
  `829DF308`, comandi precostruiti `829D0B80`, `829DEF28`, `829DECC0`, `829CA608`, 41 parole a −1) con `NewSegment`,
  `SubmitIndirect` e la coda di EndTiling come hook raw. La base ha **tolto** il percorso sa2. Gli hook di Katamari
  (`hook_census.py`, `ultimate-reports/u4-hook-census-base.txt`): 335 vivi in file compilati, 9 su un inizio di
  funzione GTA IV, 0 dei 23 binding GTA IV.
- **Scelta per U4b** (la più verificabile): il percorso del device provato sulla console (runs 66-72) entra nel layer
  di gioco GTA IV; i 23 indirizzi si legano alle funzioni della base dove il corpo è lo stesso (ring, fence, draw,
  shader), con i valori GTA IV degli offset in linea che quei corpi usano; ogni altro binding di Katamari passa alla
  forma `BANJO_RENUT_UNHOOKED_*` della base (nessun indirizzo Katamari resta legato). Il percorso di creazione nativa
  resta non agganciato: i suoi offset non si toccano finché un passo di U5 non lo provi su GTA IV. Gli altri hook della
  base di cui il doc 18 conosce l'indirizzo GTA IV (InsertCallback `829D8810`, InitializeApiState `829DECC0`,
  InitializePresentationParameters `829DEF28`, SynchronizeToPresentationInterval `829D5158` / `829D50D8`,
  SetPredication `829CD300`) sono candidati per U5, uno per volta sul PC.

### 2.6 U4b: lo stato (2026-11-12, sera)

- **Binding di Katamari:** `tools/ultimate/unhook_katamari.py` ne ha resi non agganciati 323 (device 24, draw 13,
  resource 39, shader 4, state 243). `hook_census.py`: **0 hook vivi** (`ultimate-reports/u4-hook-census-unhooked.txt`).
  Le modifiche ai sorgenti sono nel working tree, **non committate**: aspettano la build PC della base.
- **Confronto corpo per corpo** (base contro renderer funzionante, commenti esclusi):
  `KickOff_hook`, `KickOffSegment_hook`, `BeginRingBig_hook`, `RingBufferDeviceAllocate_hook`,
  `AllocateInlineData_hook`, `CBlocker_Check_hook` **identici** (si legano ai loro indirizzi GTA IV così come sono);
  `BlockOnFence_hook` uguale salvo la nostra diagnostica dei 2 s; `SetFence` uguale salvo i due offset di coda
  (0x5404 / 0x3A30 di Katamari -> 21.512 / 14.904); `D3DDevice_Swap_hook` della base senza il conteggio dei flip di
  GTA IV (`word(16552)`: senza, lo Swap originale aspetta i flip) e con la rampa gamma a 0x3A84 (GTA IV 14.988);
  `Sa2NewSegment_hook` e `Sa2CreateDevice` **assenti** nella base.
- **Draw:** i corpi GTA IV di ClearF, Resolve, BeginTiling, EndTiling (il lavoro host in coda, poi la funzione del
  gioco) si reggono su `QueueHostClear` / `QueueHostResolve`, **assenti** nella base (che fa clear, resolve e tiling in
  modo nativo, `BeginTilingImpl` / `EndTilingImpl`, con offset in zone non mappate per GTA IV). Restano da confrontare
  `D3DDevice_DrawVertices_hook`, `DrawIndexedVertices_hook`, `BeginVertices_hook`, `D3DResource_Destroy` e i tre di
  `shader.cpp` (CreateVertexShader / CreatePixelShader / CreateVertexDeclaration, raw nel renderer funzionante: il 6274
  restituisce l'oggetto shader con la parte fisica a +24).
- **Da fare per chiudere U4:** portare nella base `Sa2CreateDevice` (con i passi dell'hardware path di 6274),
  `Sa2NewSegment_hook`, i raw `829D77D8` / `829DDF28`, le correzioni di Swap e `SetFence`, `QueueHostClear` /
  `QueueHostResolve` e i corpi raw di draw / resource / shader; legare i 23 indirizzi; `hook_census.py --strict` a 0
  problemi sugli hook (23 binding, ognuno con un hook vivo su un inizio di funzione); gli indirizzi di Katamari che
  restano nel codice compilato senza essere hook (`banjo_hand_natives.h` 93, `banjo_shared_hooks.h` 34,
  `widescreen.cpp` 11, `banjo_reverb_native.cpp` 8, ...) rivisti uno per uno; rigenerare (`regenerate.ps1`, manifest
  nuovo) e ripetere i censimenti; poi la build PC (U4 finisce col primo Swap sul PC).

### 2.7 U4b: i 23 hook legati, la prima build PC, il primo avvio completo (2026-11-12 notte / 2026-11-13)

- **Codice portato nella base** (ogni pezzo dal renderer che GTA IV ha fatto girare sulla console, runs 66-72):
  `device.cpp`: `Gta4CreateDevice` (il percorso di sa2-nx con i passi dell'hardware path di 6274), `Gta4NewSegment_hook`,
  i raw `829D77D8` / `829DDF28`, i valori GTA IV delle costanti del percorso (sink 1 MiB, side buffer 16.712, −1 a
  21.540 / 21.544 / 14.920, qword `0x82A9E568`, sezioni critiche 14.928 / 14.956, frequenza 21.568, 41 parole a 21.680,
  slot `0x82000768`), `SetFence` (21.512 / 14.904), Swap (conteggio dei flip a 16.552, rampa gamma a 14.988),
  `GetCurrentThreadId` = `sub_829A27C0` (l'unica funzione di GTA IV con le stesse tre istruzioni); `draw.cpp`:
  `QueueHostClear` / `QueueHostResolve` e i raw di ClearF, Resolve, BeginTiling, EndTiling; `shader.cpp`: il registro
  delle dichiarazioni create dal gioco (`FindGuestDeclarationLocked` / `ReleaseGuestDeclarationLocked`, che invalida
  anche le cache di fast-draw della base) e i raw di CreateVertexShader / CreatePixelShader / CreateVertexDeclaration;
  `resource.cpp`: il raw di D3DResource_Destroy; `render/draw.cpp`: le due ricerche della dichiarazione trovano anche
  quelle del gioco; `guest_records.cpp`: heap dei record 1 MiB invece di 64.
- **Trappola trovata:** nella base `BANJO_RENUT_HOOK_SHARED` non definisce l'hook a `sub_X` (solo `renut_native_sub_X`
  per un altro renderer); il nostro albero lo ridefiniva. I binding GTA IV usano la forma semplice; `hook_census.py`
  conta le forme `_SHARED` come "nessun hook".
- **Verifiche:** confronto corpo per corpo (KickOff, KickOffSegment, BeginRingBig, RingBufferDeviceAllocate,
  AllocateInlineData, CBlocker_Check, DrawVertices, DrawIndexedVertices, BeginVertices, CreateVertexShader_hook /
  PixelShader / VertexDeclaration della base: identici ai nostri); `hook_census.py --strict` **0 problemi** (23 hook vivi,
  ognuno su un inizio di funzione, nessun altro; ogni indirizzo 82xxxxxx rimasto nel codice compilato rivisto, con il
  motivo, in `gta4_hook_bindings.csv`: 22 indirizzi, 7 file); rigenerazione completa con il manifest
  `ultimate-manifests/gen-u4b-hooks.json` (hash combinato `08e5df6f…`, `direct` rispetta 600 indirizzi): **0 chiamate
  dirette** del codice generato verso i 23 indirizzi (KickOff: 80 chiamate tutte attraverso l'hook); test del layout 0
  differenze; la build stessa ha rigenerato e trovato "334 di 334 file invariati".
- **Build PC** (`tools/ultimate/build_pc.bat`, `tools/ultimate/build_glslang.bat`): mancavano nella base i binari DXC
  (`dxc-bin/bin`, `lib`: esclusi dal suo `.gitignore`; ora copiati dal sottomodulo `tools/XenosRecomp/thirdparty/dxc-bin`
  da `fetch_sdk_thirdparty.ps1`) e `glslangValidator`; la cartella degli shader è fissa in `ultimate/out/renut_shader_dump`
  (una junction ai contenitori). **`renut.exe` collegato** (70,7 MB, 905 warning), ma con la tabella degli shader **vuota**:
  una prima configurazione fallita aveva lasciato la cartella vuota (ora `build_pc.bat` si ferma in quel caso).
- **Primo avvio PC** (D3D12, 7 s, quella build): device GTA IV creato dall'hook (`Guest D3D device (type 1)` e `(type
  2)`), render thread avviato, gli hook di creazione degli shader registrano (e catturano) gli shader del gioco; poi
  `PhysicalHeap::Alloc` fallisce e c'è una chiamata a guest 0. Causa: l'heap dei record da 64 MiB, la stessa del 2026-11-11
  sul renderer funzionante; corretta (1 MiB), **non ancora compilata né provata**.
- La prima ricompilazione con i 1.370 contenitori veri è stata interrotta da Claude Code per memoria del sistema scarsa;
  rilanciata col via dell'utente il 2026-11-13 (10,8 GB liberi, 4 job).
- **Scratch dei vertici** (`hooks/draw.cpp`): 1 MiB per device invece di 4, come nel renderer funzionante (stessa memoria
  fisica guest scarsa; BeginVertices oltre quella misura restituisce 0, come prima).
- **Build PC completa** (2026-11-13): 16 passi, la traduzione degli shader dai contenitori compresa, `renut.exe` 76,6 MB.
  Attenzione: il passo degli shader della base **aggiunge alla cartella dei contenitori** gli shader catturati da un
  avvio (`renut_shader_capture_dir`, predefinito `shader_capture`): ne ha aggiunti 1.250 (nomi `ps_/vs_<hash>.bin`), i
  1.370 originali intatti (date invariate). Siccome la cartella è una junction ai dati privati, negli avvii di prova la
  cattura va mandata altrove (`--renut_shader_capture_dir=<cartella di lavoro>`); i 1.250 file aggiunti sono lasciati dove
  sono (stessi shader sotto un altro nome) finché non si decide la cartella della cache offline (U8).
- **Avvio completo sul PC** (D3D12, AMD RX 560X, 120 s, `--banjo_renderer=renut --game_data_root=<eu>
  --nfsmw_captura_cada_s=10`): device GTA IV creato dall'hook, render thread, target 7e3 e D24FS8 1280x720, ripristino
  della depth EDRAM (`depth 400007C0 ... from colour 40226810 (drawn)`), resolve multisample disegnati, shadow map
  1280x1280. **Nessun `PhysicalHeap::Alloc` fallito, nessuna chiamata a guest 0, nessun errore del renderer.** Catture a
  10-120 s: logo Rockstar, filmato d'apertura (corridoio della nave, ponte coi titoli, mani di Roman) **giusto come le
  catture R3** (`out/pc-renut/cap39`): personaggi interi, illuminazione ed esposizione corrette. 59-60 fps, render thread
  3-6 ms, GPU 9-13 ms a frame; audio SDL senza frame mancanti; latenza input -> present 27,8 ms.
- **Problema aperto trovato dall'avvio:** l'autoverifica del codice generato (`REX_STACK_FWD_VERIFY`, accesa nelle build
  PC) segnala **60.434 inoltri sbagliati su 7.742.986** di `tools/stack_forward.py` (81 caricamenti inoltrati in 44 file),
  tutti allo stesso slot di stack (`7037F910`). Sul PC non cambia nulla (in verifica vince il caricamento reale); nella
  build Switch vincerebbe l'inoltro. Il rapporto dà solo l'indirizzo, non il punto: va trovato quale degli 81 inoltri
  sbaglia (o la passata spenta per GTA IV, `REX_STACK_FWD_OFF`) **prima di ogni build Switch** (U5/U6). Nella base,
  su Katamari: 0 differenze.
- **Verifiche dopo la build:** `hook_census.py --strict` 0 problemi (rapporto uguale al precedente a parte i numeri di
  riga di `draw.cpp`, aggiornato); `test_ultimate_gta4_layout.py` 0 differenze; i 342 file di `app/generated` contro
  `gen-u4b-hooks.json`: sorgenti identici, cambiano solo i 4 file di timbro e tempi del generatore (le build della notte
  hanno rieseguito il codegen, che aveva trovato tutti i file invariati).

### 2.8 U5a: lo stack forwarding corretto alla radice (2026-11-13)

- **Dove:** il reporter della differenza (`app/src/cpu/banjo_codegen_checks.cpp`, `RexStackFwdMismatch`) ora scrive
  anche ogni nuovo punto chiamante (indirizzo di ritorno, fino a 32 punti); con il PDB (`renut.pdb`, dbghelp) i 60.434
  errori di U4b vengono da **4 punti**: `sub_82896650` e `sub_828962F8` (inoltro attraverso due chiamate a
  `sub_822AF898`), `sub_82776AB0` (attraverso `sub_82772760`), `sub_82777B20` (attraverso un `bctrl`).
- **Causa (difetto della passata della base, non di GTA IV):** `tools/stack_forward.py` azzera il suo stato su chiamate,
  salti ed etichette controllando solo la riga del commento di ogni istruzione, poi salta il corpo C++ (`i = j`): le
  righe `__imp__sub_X(ctx, base);`, `REX_CALL_INDIRECT_FUNC(...)` e i `goto` dei rami condizionali stanno nel corpo e non
  venivano mai viste. Quattro `stfs` in `r1+A`, una chiamata che riceve `r1+A` e lo riscrive, poi l'`lvx` dello slot:
  l'inoltro dava i valori di prima della chiamata. La docstring della passata dichiara già "no call, label, jump ... in
  between": la correzione fa valere quella regola anche per il corpo (una chiamata: `ctx.lr = `, `(ctx, base)`,
  `REX_CALL`; un salto: `goto`, `return;`, `switch (`) e azzera tutto. Nessun controllo spento, la passata resta attiva.
  Su Katamari la base aveva 0 differenze: i suoi 24 inoltri non attraversavano chiamate che riscrivessero lo slot.
- **Rigenerazione completa** (`regenerate.ps1`, manifest `ultimate-manifests/gen-u5-stackfwd.json`, hash combinato
  `3e76d58a…`): `stack_forward` **67 caricamenti in 39 funzioni** (prima 81 in 47); nessuno dei 67 ha più una chiamata
  o un salto tra le scritture e il caricamento (controllo riga per riga del codice generato). Contro `gen-u4b-hooks.json`
  cambiano solo i 16 file `.cpp` con inoltri e i tempi; ogni altra passata dà gli stessi numeri (la riga di `direct`
  cambia solo l'ordine di due voci a pari conteggio). L'hash di `rexglue.exe` è diverso perché ogni build PC ricollega
  il generatore (stessi sorgenti, stessa versione 0.10.0): i file generati senza inoltri sono identici byte per byte.
- **Verifica PC** (D3D12, 120 s, scena d'apertura): **8.526.419 inoltri confrontati con il caricamento reale, 0
  differenze**; ogni altra autoverifica a 0 (costanti di sola lettura, mirror di r1, write-back del link register,
  copie); `hook_census.py --strict` 0 problemi; `test_ultimate_gta4_layout.py` 0 differenze. Handoff alla base: la
  stessa correzione vale per ogni gioco.

### 2.9 U5b: le correzioni del vecchio renderer contro la base, l'avvio PC affidabile (2026-11-13)

Ogni correzione verificata del renderer di Theft4-NX (R3, runs 66-70) confrontata con l'equivalente della base:

| Correzione (commit) | Nella base | Esito |
|---|---|---|
| Formati colore 0xA / 0xC come 2 / 3, HDR 7e3 (`efe507d`) | port 75, `renut_format_edram_as_variants` (acceso): 0xA come 2, 0xC come 3; 3 e 0xC in FP16 con l'alfa (`renut_format_7e3_rg11b10` spento) | **coperta**, niente da riportare (log: target 7e3 = host format 10, RGBA16F) |
| Ripristino della depth dal colore (`6921903`) | port 29, `sasrt_renut_depth_restore` (gira: "EDRAM depth restore ... (drawn)") | **coperta** |
| Metà dei tile EDRAM scambiate (`00683a5`) | port 29 le scambia in **pixel host** (`texel.x % 80`) | **riportata**: lo scambio in pixel guest (`depth_restore_ps.hlsl`, `SwapHalfTile`, scala da `depth->scale.Factor()`), come il nostro. A scala 1 è la stessa colonna (scena PC invariata); a un'altra scala (render scale, risoluzione dinamica di U9) la base scambiava le colonne sbagliate |
| Resolve multisample disegnati, MS non compressi e descrittori "decayed" su deko3d (`1846a14`, `e6d8b94`) | `msaa_level` 1 di default: ogni target MSAA del gioco diventa a un campione (`surfaces.cpp`), i resolve 4x leggono la griglia dei campioni (`blur_renut_sample_grid_resolve`); il plume deko3d della base comprime i target MS e li descrive come MS | **non riportata ora**: con `msaa_level` 1 non esistono immagini MS (il caso dei runs 68-69 non si presenta). Il nostro fix è legato ai nostri shader che leggono la griglia; quelli della base (`edram_inherit.hlsli`, `depth_resolve_ms_ps`) leggono MS veri. Da decidere in U9 (prove con MSAA) e solo con la console |
| Zcull invalidato dopo il ripristino della depth (`1846a14`) | no (solo dopo copie e upload: `invalidateZcullAfterCopy`); `banjo_deko3d_zcull` acceso | **pronta, non committata**: `Deko3DInvalidateZcull` + la chiamata dopo il ripristino (`ultimate/out/u5-wip/`, la parte plume compilata con `aarch64-none-elf-g++ -fsyntax-only`); entra con la prima build Switch (U7), che la compila |
| GuestPtr con la tabella guest dello Switch (`ebc999f`) | l'SDK 0.10 riserva una finestra contigua di 4,5 GB e mappa ogni vista al primo accesso (`guest_memory_switch.h`); su Switch `host_address_offset` è 0 (granularità 4 KB) | **non serve**: la somma `base + VA` di `GuestPtr` è `TranslateVirtual` su quel layout (verificato sul codice; la console lo proverà in U8) |
| Vsync D3D12 spento e attesa delle fence (`4c0eb6c`) | `renut_vsync` acceso; `waitForCommandFence` su un evento auto-reset | **riportata** (sotto) |

- **Avvio PC bloccato (trovato in U5):** 5 avvii su 12 si fermavano prima del device. Stack dei thread (dbghelp, PDB):
  il thread UI in `Video::PresentOverlayOnly` -> `SubmitAndAdvance` -> `WaitSlotLocked` aspettava un frame che la coda
  D3D12 non finiva (fence a 5, attesa 6, device non rimosso) tenendo il mutex video; il thread del gioco, in
  `Gta4CreateDevice`, aspettava quel mutex in `Video::SetGuestDevice`. Con la swapchain flip e `SyncInterval 1` la coda
  aspetta il back buffer. Misure: vsync acceso 3 bloccati su 6, poi 5 su 12 anche con l'attesa delle fence corretta;
  vsync spento + attesa corretta **12 su 12**; con i default nuovi **8 su 8**. Correzioni: `plume_d3d12.cpp`
  `waitForCommandFence` aspetta il valore dell'ultimo segnale (l'evento auto-reset perdeva un segnale se la fence ne
  riceveva due prima dell'attesa); `renut_vsync` spento di default **solo sul PC** (`present.cpp`; Switch invariato).
- **Rampa gamma del gioco:** nella base `banjo_renut_game_gamma` è spento (scelta di Katamari, non nella lista
  `off-by-default.md`); il nostro renderer la applicava (`sa2_renut_gamma_ramp`), come il display della 360. La rampa di
  GTA IV non è l'identità: stessa build, stessa inquadratura a 60 s, media RGB 84/126/138 con la rampa, 98/137/149
  senza (mezzitoni più profondi, cielo meno slavato). **Accesa di default** per GTA IV. Da fare in U7:
  `katamari_switch_defaults.toml` la spegne per Katamari: il toml Switch di GTA IV non deve copiarlo.
- **Percorso SA2:** `Gta4CreateDevice` e `Gta4NewSegment_hook` **identici** riga per riga (commenti esclusi) a
  `Sa2CreateDevice` / `Sa2NewSegment_hook` del renderer funzionante. Lo Swap differisce in quattro punti:
  - il pacing: la base usa `renutFrameLimit` (`frame_cap` "Off" + display sync), non il `PaceSwap` di sa2-nx che seguiva
    l'intervallo di presentazione del gioco. Sul PC GTA IV gira a 60-67 fps invece del ritmo della console.
    **Differenza reale, da decidere sulla console (U8)**: `frame_cap` 30 o l'intervallo del gioco;
  - la gamma (sopra);
  - il layout dei core: `banjo_renut_core_layout` spento nella base, acceso nel vecchio albero. Resta quello della base
    (nessuno spostamento di thread senza misure, U11);
  - `ResetAllState` a ogni frame (meccanismo della base, innocuo sul PC).
- **Chiamate in uscita** (`cc_exit_census.py` sul codice nuovo; prima istruzione che legge un registro "morto
  all'uscita", con uno script di classificazione): 14.541 chiamate indirette, 572 segnalate, e nessuna è un problema
  reale con la configurazione di GTA IV (`SASRT_REG_LIVENESS_EXIT_ABI=0`, che tiene vivi quei registri: il codice si
  comporta come l'originale). Divisione:
  - 280 sono un limite dello strumento: `frsqrte` non è decodificato da `reg_liveness` e conta come lettura di tutto;
  - 368 sono `vmrghw` / `vmrglw` e 3 `vupkhsh` su v5-v13, che l'analisi (insensibile ai percorsi) raggiunge da rami o
    cicli dove il registro è caricato poco prima (es. `sub_82831278`: `lvx128 v8`, `v9`, poi `vmrghw v12,v8,v9`).

  Accendere l'ABI d'uscita (un'ottimizzazione) richiederebbe di provarle una per una e di decodificare `frsqrte`: non
  si fa ora.
- **Verifica PC della build finale** (D3D12, default nuovi, 120 s): scena d'apertura giusta (stessa inquadratura di R3 a
  90 s, personaggi interi), 67 fps senza limite, autoverifiche a 0 differenze, nessun errore del renderer; 8 avvii su 8.

### 2.10 U6: shader per la Switch e cache DKSH offline (2026-11-13, solo PC)

Gli strumenti della base (`switch_shaders.ps1`, `deko3d_shaders.py`, `deko3d_glsl_opt.py`, `deko3d_shader_package.py`,
`renut_switch_shader_cache.py`, `deko3d_variant_cache.py`) si usano così come sono. Per GTA IV e per questa macchina
servono quattro adattamenti; due sono difetti che la catena della base non segnalava.

**Procedura** (riproducibile; ogni prodotto è un dato del gioco in una cartella ignorata da git, mai nel repository):

1. Strumenti PC, una volta: `tools/ultimate/build_deko3d_tools.bat` con `THEFT4_SPIRV_CROSS_SRC` = un albero di
   SPIRV-Cross a `22b22f5685d868828be01c9ac00c31902e60afd9` (il commit che `thirdparty/MoltenVK/SPIRV-Cross` di
   UnleashedRecomp-NX fissa, la copia che `deko3d_build_tools.sh` della base nomina; `git archive` di quel commit più un
   file `COMMIT` che lo nomina). Produce `ultimate/out/deko3d_tools/{spirv-cross-build/spirv-cross.exe,
   uam-build/uam.exe, uam-build/uam_dksh_test.exe}`.
2. Build PC: `tools/ultimate/build_pc.bat` (ora riempie da sé `ultimate/out/renut_shader_dump`, punto 4 sotto).
3. `powershell -File ultimate\tools\switch_shaders.ps1` con `THEFT4_PYTHON`: `ultimate/app/out/renut_switch`
   (`deko3d_glsl.bin` + `deko3d_glsl_switch.cpp`, `shader_cache_switch.cpp` + `spirv_cache.zst`: gli ingressi della NRO
   di U7) e `ultimate/out/deko3d_shaders/glsl_opt`.
4. `python tools/ultimate/gta4_dksh_cache.py --list <liste> [--runtime-log <game.log>] [--merge <cache della SD>]
   [--console-cache <cache della SD>] --manifest <json>`: `ultimate/out/deko3d_shaders/deko3d_shaders.bin` (+
   `.variants.txt`), da mettere accanto alla NRO. Il nome vale per i flag di uam di default (1471,
   `banjo_renut_deko3d.cpp`); con altri flag la console cerca `deko3d_shaders_f<flag>.bin`.

**Adattamenti e difetti trovati:**

| Punto | Trovato | Fatto |
|---|---|---|
| Python | `switch_shaders.ps1` ha `C:\Python314\python.exe` scritto fisso | legge `THEFT4_PYTHON` se c'è (una riga) |
| uam per PC | lo script della base usa il clang64 MinGW di devkitPro, che qui non c'è; l'uam della base sull'ABI MSVC non compila (`c11/threads.h` vuole pthread) | riportate le 5 modifiche di build del vecchio albero (`16-deko3d-devlog.md` M1): lock SRW, opzioni brevi senza getopt, `signbit`, tipo dell'enum di `ureg_DECL_output_masked`, stack di 16 MB per i vertex shader più grandi di GTA IV. Tutte sotto `_MSC_VER` / ABI MSVC tranne il tipo, che per GCC era già quello; annotate in `PATCHES.md`. La build Switch non cambia |
| **Nomi dei contenitori** | il nostro estrattore (run 61) scrive `<HASH>.<vs\|ps>.bin`, la base `<vs\|ps>_<xxh3>.bin` (stesso hash: verificato su tutti i 1.370). XenosRecomp dà all'HLSL esportato il nome del file d'ingresso, e `renut_early_alpha.py` cerca `ps_*.hlsl`: **ha elaborato 0 shader, senza errori**, nella build PC e in quella Switch. E il passo di cattura della build PC (`collect_captured_shaders.cmake`) confronta i nomi: in U4b, con la cache vuota del primo avvio, aveva copiato **1.250 duplicati** (stessi byte, nome diverso) nella cartella dei contenitori di eu-private attraverso il junction | `tools/ultimate/gta4_shader_dump.py`: copia i contenitori in `ultimate/out/renut_shader_dump` (una cartella vera, non più un junction) col nome della base, e controlla per ognuno intestazione, stadio, dimensione e hash. Chiamato da `build_pc.bat`. I 1.250 duplicati sono stati **spostati, non cancellati**, in `eu-private/offline-shaders/pc-captures-u4b` (verificati identici byte per byte agli originali); `app/out/pc/shader_capture` rinominata `shader_capture-u4b-duplicates` |
| Early alpha su GTA IV | con i nomi giusti `renut_early_alpha.py` vede i 704 pixel shader, ma non ne riscrive nessuno: 17 "little" (guadagno troppo piccolo), 687 "unsupported", quasi tutti per **costanti con nome** (`globalScreenSize` 101, `globalFogParams` 88, `gShadowParam14151617` 59...): lo strumento conosce solo la forma `g_C(N)` | non toccato: è un'ottimizzazione (U10), da misurare. Gli shader restano identici (tabella PC = tabella Switch, sotto) |

**Copertura e verifiche** (`gta4_dksh_cache.py`, manifest `docs/switch-port/ultimate-manifests/dksh-u6.json`):

- **Pacchetto:** 1.370 shader (666 vertex, 704 pixel), tutti tradotti; SPIRV-Cross e uam sulla variante di default **0
  falliti**; 48 passate GLSL della base; `deko3d_glsl.bin` 1.970 KB (21 MB di GLSL). Ogni voce della tabella della NRO ha
  il suo testo e ogni testo una voce; la tabella del pacchetto è ordinata e unica, con dimensioni e stadi giusti; 25 voci
  scompattate col dizionario del pacchetto sono uguali byte per byte ai testi.
- **Tabella:** quella della build PC (fatta da 2.620 file con i duplicati) e quella rifatta dai 1.370 sono **identiche**
  voce per voce (offset e dimensioni di DXIL e SPIR-V esclusi), con i 2.862 input dei vertici e i 2.707 literal. XenosRecomp
  scarta i duplicati per hash: 1.370 voci, nessuna ripetuta.
- **uam, libreria contro strumento** (`uam_dksh_test`, ogni 10° testo + i 10 più grandi): 147 su 147 DKSH identici; uno
  shader che non compila dà l'errore nel log e la compilazione successiva riesce. Lo shader più lento richiede 28 s sul PC
  (il DKSH più grande è un pixel shader da 478 KB): sulla console, senza cache, una variante così sarebbe uno stallo
  lungo.
- **uam del PC contro la console:** il `deko3d_shaders.bin` della SD in `out/console-runs/72-logs` (run con la NRO
  `5d542d6`, la build del run 72, vecchio renderer ReNut) ha 409 record, **tutti compilati dalla console** (nessun prefisso
  ordinato da uno strumento PC). Ricostruite le loro varianti dal vecchio pacchetto (`out/renut-switch/deko3d/glsl_opt`) e
  dalla lista della stessa SD con `deko3d_variant_cache.py --export-variants`: ritrovate **409 chiavi su 409**, e i DKSH
  compilati sul PC sono **identici byte per byte** a quelli della console, con l'uam PC del vecchio albero e con quello della
  base costruito qui. Quindi il compilatore (clang con ABI MSVC sul PC, GCC di devkitA64 sulla console) non cambia l'uscita
  su quei 409, e lo strumento della base calcola le chiavi come la console. Il passo 5 dello script (`--console-cache`)
  ripete il confronto su una SD della base dopo U8.
- **Cache:** due liste. Quella delle corse PC di U5 (`pc-u5`, versione 7, la chiave della base: 616 pipeline, 238 shader)
  e quella della console del vecchio renderer (`console-run72`, versione 4, i primi 192 byte della chiave identici
  alla base: 651 pipeline, 356 shader). **0 shader delle liste mancano dal pacchetto.** Con `--generic --plain-guess`:
  10.755 pipeline, **4.287 varianti di stadio** (1.320 generiche che le liste non avevano), **0 fallite**, 23.125 KB; ogni
  chiave controllata contro il suo testo, ogni variante ha il suo record. **Riproducibile:** rifatta da zero in un altro
  file, stesso SHA-256 (`1d6f7737…`, nel manifest con gli hash di ogni ingresso).
- **Build PC col nuovo `build_pc.bat`:** compila; 125 s su D3D12, scena d'apertura giusta (70 s e 90 s), 67 fps, 0
  shader catturati, nessun errore.

**Shader mancanti o non compilabili, sulla console** (dal codice della base, `shader_library.cpp`, `hooks/shader.cpp`,
`banjo_renut_deko3d.cpp`):

- un contenitore che la tabella non ha viene scritto in `shader_capture/` accanto alla NRO, una volta per hash; la build
  successiva lo aggiunge (`gta4_shader_dump.py --dumps <cartella>`);
- un hash della tabella senza GLSL nel pacchetto: "[deko3d] shader ... is in no GLSL package" (le prime 16 volte) e
  "Creating shader ... failed"; lo shader non si riprova e i suoi draw non disegnano;
- una variante che la cache non ha viene compilata in gioco da uam, su un thread in background, e aggiunta in coda al
  file. Intanto disegna la variante generica, che la cache ha per ogni shader;
- una cache di un altro compilatore (tag diverso da `uam110-sw8-bnx4`) viene ricominciata da zero; un record
  troncato in coda viene tagliato.

**Limiti, fino a U8:** le chiavi vengono da liste che non sono quelle della base sulla console. La lista PC ha la chiave
giusta, ma alcuni campi si riempiono solo su deko3d (la funzione d'alfa con `sasrt` acceso, le parole di fetch). La lista
della console ha la chiave vecchia, e lo strumento la completa con le stime della base: alfa 8 / 7 / 5 e fetch "plain".
Il primo run U8 dirà quante varianti compila ancora ("uam: run-time variant" nel log, record in coda alla cache della
SD). Dopo U8 si rifà la cache con
`--list <pipeline_list.bin della SD> --runtime-log <game.log> --merge <deko3d_shaders.bin della SD> --console-cache
<lo stesso>` e la si controlla prima di ogni misura ("uam 0 compiles").

### 2.11 U7: la NRO Switch di GTA IV (2026-11-13; build sul PC, nessun run su console)

**Ingressi controllati prima della build** (nessun prodotto vecchio o incompleto riusato):

- `hook_census.py --strict`: 0 problemi, 23 hook vivi, tutti binding GTA IV. Il censimento conta come compilato anche il
  codice sotto `REX_PLATFORM_SWITCH`; negli hook l'unico ramo solo-Switch è `banjo_renut_core_layout` (spento, §2.9).
- `test_ultimate_gta4_layout.py`: 0 differenze.
- `app/generated` contro `gen-u5-stackfwd.json`: 338 file su 342 identici; i 4 diversi sono gli stamp della generazione
  (`codegen.stamp`, `codegen.build.stamp`, `codegen.d`, `codegen.times.json`) riscritti dalla build PC, nessun sorgente.
- `app/out/renut_switch` (pacchetto GLSL `7963cb14…`, tabella `061feff7…`) e la cache `1d6f7737…` = `dksh-u6.json`.
- sys-clk: `switch_sysclk.cpp` dell'SDK ha un override dei clock di sys-clk / Horizon OC, spento (`g_enabled` 0) e mai
  acceso da nessuna parte del codice; spento, non apre nemmeno il servizio. I clock restano quelli standard; il profilo
  APM ufficiale GPU 460,8 + memoria 1331,2 (`banjo_gpu_boost`) è quello dei runs 70-72.

**Lavoro:**

| Punto | Fatto |
|---|---|
| Build | `tools/ultimate/build_switch.bat [everyday\|guard]`: `ultimate/tools/build_switch.bat` con l'ambiente del progetto (CMake, Ninja e Python di `env.bat`, GCC 16.1 di devkitA64, `THEFT4_SWITCH_JOBS`, default 2). Controlla ogni ingresso prima di configurare. **Solo deko3d**: `REXGLUE_SWITCH_NVK` spento, gli stub "non disponibile" dell'SDK al posto del driver; plume vuole comunque una cartella portlibs con gli header Vulkan e un `libvulkan.a` esistente (`THEFT4_NVK_SDK`, default `<devkitPro>/portlibs/switch`), che non viene collegato (verificato sulla mappa di link) |
| Identità | il commit nel log (`THEFT4_BUILD_COMMIT`, solo su `main.cpp`: "theft4-nx: GTA IV on the Ultimate Base, build <commit> (<flavour>)", "-dirty" con modifiche in `ultimate/` o `tools/ultimate/`). **Riproducibilità:** l'SDK scrive in `version.h` l'ora della configurazione e lo script configura a ogni build, quindi due build dello stesso commit davano due NRO (`aa88997`: `SOURCE_DATE_EPOCH` = ora del commit). Verificato: ricompilati `main.cpp`, `rex_app.cpp`, `debug_overlay.cpp` e ricollegato, stessa NRO e stesso ELF byte per byte |
| Toml | `app/gta4_switch_defaults.toml` (compilato nella NRO) e `app/gta4_switch.toml` (il `gta4.toml` del giocatore, scritto accanto alla NRO se manca). Confronto delle 229 chiavi di `katamari_switch_defaults.toml` coi default del codice: 156 uguali, 24 senza cvar (nativi di Katamari ai suoi indirizzi), 49 diverse: renderer nativo / GPU emulata / chiavi "non costruite" (non usate da Renut), chiavi di Katamari (pelliccia, `katamari_hand_*`), log e diagnostica spenti, scelte di resa per Katamari (1080 in dock, mip host, ombre, precisione) e **`banjo_renut_game_gamma = false`**. Nel file di GTA IV: `banjo_renderer = "renut"`, `renut_gpu_backend = "deko3d"`, **`banjo_renut_game_gamma = true`** (esplicito), nessuna modalità a 4 core (`banjo_forwarder_program_id = ""`, `banjo_system_core = 0`: nessun NPDM, nessuno spostamento di thread), guard spento, `banjo_display_sync` (scelta Switch della base, indipendente dal gioco). Tutto il resto resta al default del codice provato sul PC in U4b-U6 (log acceso, risoluzione 720, mip host, `frame_cap` "Off": scelte di U8-U9) |
| NRO | "Theft4 Ultimate" / "Theft4-NX" / 0.7.0, icona di libnx (nessuna copertina nel repository; `THEFT4_SWITCH_ICON` per una locale), cartella SD `sdmc:/switch/theft4-ultimate/` (quella del vecchio albero resta com'è, con la sua cache e i suoi log). Il percorso dei dati di gioco si decide prima di leggere qualsiasi toml (`rex_app.cpp`), quindi in `renut_app.h`: `game_root` accanto alla NRO, altrimenti i file del vecchio albero `/switch/theft4/game` (forma con la radice, come `GetExecutablePath`), in sola lettura |
| Zcull (`1846a14`) | `plume::Deko3DInvalidateZcull` (la patch di `out/u5-wip`, applicata senza modifiche) e la chiamata in `RestoreDepthFromColorLocked` subito dopo il draw, solo deko3d. Verificato che sul deko3d la command list è sempre quella vera (il registratore proxy di `banjo_renut_record` lavora solo su Vulkan), quindi il cast di plume è corretto; senza `banjo_deko3d_zcull` non fa nulla |
| Pacchetto | `tools/ultimate/package_switch.py`: controlla e poi scrive `out/console-runs/<nome>` (sotto). Rifiuta una build con commit diverso da HEAD o con modifiche locali, e una cartella esistente |

**Build** (everyday: copie in place, nessuna forma di guardia): 1.102 passi. La prima corsa (2 job) è stata fermata da
Claude Code per memoria scarsa a 1.001/1.102 (i due "FAILED" erano i compilatori uccisi, senza messaggi); ripresa con 1
job sugli oggetti già fatti (105 passi), poi ricostruita dopo il commit (5 passi). Nessun errore di compilazione.
`renut` (ELF) 72.600.144 byte, `renut.nro` 56.408.261 byte.

**Controlli del pacchetto** (`package_switch.py`, manifest `docs/switch-port/ultimate-manifests/switch-u7.json`):
opzioni della cartella (`REXGLUE_SWITCH_NVK` e `PLUME_SWITCH_NVK_LINK` spenti, `PLUME_DEKO3D` acceso, config incorporata,
shader nella NRO, `SASRT_HOT_GUARD_FORMS` OFF, `SASRT_HOT_IN_PLACE` ON); intestazione NRO0 e NACP; nell'ELF: la stringa
del commit, `gta4_switch_defaults.toml` come impostazioni incorporate e nessuna di Katamari, `g_deko3dGlslPackage` di
2.017.600 byte = `deko3d_glsl.bin` (anche i byte), `g_shaderCacheEntries`, **33 shader host deko3d** (tra cui
`depth_restore_ps`), `Deko3DInvalidateZcull`, nessun `libvulkan.a` nella mappa (le due entrate ICD vengono da
`switch_vulkan_stubs.c`); la cache: intestazione `BDKC`, tag `uam110-sw8-bnx4` = `kCacheTag` della NRO, SHA-256 = U6.
`gta4_dksh_cache.py --check-only`: pacchetto contro tabella 1.370/1.370, uam libreria contro strumento 147/147 identici
(percorso d'errore ok), cache 4.287 record, ognuno con la sua variante.

**Pacchetto di test** `out/console-runs/u7-build` (commit `aa88997`, everyday; fuori da git):

| File | Byte | SHA-256 |
|---|---|---|
| `COPY_TO_SD/switch/theft4-ultimate/theft4-ultimate.nro` | 56.408.261 | `f42ae3e47bce64b48649be8943a594a1a29bd73d4e599b9ea65d2427f99ec508` |
| `COPY_TO_SD/switch/theft4-ultimate/deko3d_shaders.bin` | 23.680.524 | `1d6f7737741daedb1be3ac82e9138ebb35d29d45f7e6b7cb49824f7992aedc4e` |
| `COPY_TO_SD/switch/theft4-ultimate/gta4.toml` | 931 | `458ea1350934bd3976cef9bd2b0d90220fc1b235ba732d134f914255b6ef6e0f` |
| `theft4-ultimate.elf` (non per la SD) | 72.600.144 | `b7a07f8cf7a909ae29df618e3371555c0833424724f6ce2db1dc9e75ff6c679e` |
| `renut.map` | 39.672.863 | `18a20cad989b86de58d3ee2278f484048ad671de3267cf55511213f35052e9f1` |

Avvio (README del pacchetto): clock standard, modalità applicazione (title takeover); il forwarder del vecchio albero
avvia la vecchia NRO. Il primo pacchetto, dal commit `12b20c2` prima della correzione della riproducibilità, è stato
rinominato `u7-build-12b20c2-superseded`, non cancellato.

**Non fatto in U7, e perché:**

- **NRO guard** (`build_switch.bat guard`): non costruita. Ricompila tutto il codice generato nell'altra forma (un'altra
  ora e mezza, col rischio di memoria visto sopra) e serve a un run di guardia: Ryujinx non c'è su questa macchina, e il
  run di guardia sulla console appartiene a U10.
- **Nessun avvio**: niente Ryujinx; la prova vera è la console (U8).

**Da guardare per primo in U8:** la riga "theft4-nx: … build aa88997d7ce8 (everyday)" e il percorso del gioco nel log;
"[deko3d] DKSH cache" (letta, non ricominciata); "uam: run-time variant" (varianti che la cache non ha); il ripristino
della depth ("EDRAM depth restore … (drawn)") e l'illuminazione dopo (lo Zcull); il pacing (`frame_cap` "Off" con
`banjo_display_sync`, §2.9).

### 2.12 U8: il primo run su console (2026-11-13; Switch Erista, run dell'utente)

**Cosa si è visto (note dell'utente).** Il gioco parte e il filmato iniziale si vede; audio robotico, gracchiante e
rallentato come nelle versioni precedenti; il filmato sembra più lento del normale; in gioco molti stutter, al massimo
circa 8 fps; un sospetto sull'illuminazione.

**Dati.** File della SD in `out/console-runs/u7-build/test` (fuori da git): `logs/renut_001.log` (3.514 righe, 14 minuti
dalle 21:27:46), `logs/rex/rex_profile.log` (il profilo a 10 s dell'SDK, campionatore degli stack spento),
`logs/rex/rex_stderr.log`, `deko3d.log`, `deko3d_shaders.bin` (26.257.728 byte), `pipeline_list.bin` (versione 7, 1.037
pipeline), `gta4.toml` (= quello del pacchetto, SHA-256 `458ea135…`). **Nessuna cattura** (`nfsmw_captura_cada_s` spento):
l'illuminazione non si può giudicare da questo run. Numeri da `tools/ultimate/u8_run_report.py` (nuovo: legge le stesse
righe dai log della base e del vecchio albero; `--baseline` per il confronto), dai 60 s in poi:

| | U8, primo run (base, NRO `aa88997`) | run 72 (vecchio albero, `5d542d6`) |
|---|---|---|
| fps, mediana (10° percentile) | **6,96** (4,75) | 10,00 (5,96) |
| render thread, ms a frame (mediana) | 46,9 | 70,1 |
| GPU, ms a frame (mediana) | **29,8** | 78,6 |
| draw a frame (mediana) | 2.293 | 2.579 |
| CPU totale (di 400 %), mediana | 299 | - |
| audio, quota del tempo reale | **69,8 %** (peggiore 56 %) | 60,9 % (peggiore 51 %) |
| compilazioni uam nel run | **495**, di cui 88 attese da un draw (77,0 s in tutto) | cache calda |
| clock letti dal profilo | 1020/460,8 ×69; **2193/76,8 ×9**; 1785/460,8 ×4 (fine); 1020/76,8 ×1 | standard |

Controlli di base: la build è quella giusta ("build aa88997d7ce8 (everyday)"), il gioco viene letto da
`/switch/theft4/game` (202 voci, sola lettura), la cache DKSH è letta (4.287 varianti, non ricominciata), il ripristino
della depth gira ("EDRAM depth restore … (drawn)", 8 volte nel filmato) e con lui la chiamata Zcull; nessun crash, uscita
con `+`.

**Audio: causa confermata dai dati, correzione non ancora fatta.** Il percorso (`switch_audio_system.cpp`,
`audio_system.cpp`): l'uscita audout a 48 kHz stereo PCM16 (verificato all'apertura) chiede al gioco un frame di 256
campioni ogni 5,333 ms (pompa a 187,5 Hz, 10 frame di coda); l'Audio Worker chiama la callback del client XAudio del
gioco, il cui mixer (codice guest) produce il frame; se alla scadenza il frame non c'è, quel pezzo d'uscita è silenzio
("requests without data"). Il tempo audio del gioco avanza di un frame per frame prodotto, quindi **la quota di frame
prodotti è anche la velocità del suono**: 70 % = suono al 70 % della velocità, con buchi di silenzio in mezzo (robotico,
gracchiante, rallentato). Le frequenze sono giuste: nei primi 30 s, col gioco a 60 fps e la CPU al 38-91 %, 1.880-1.892
frame ogni 10 s su 1.875 attesi, **0 mancanti**; dal caricamento in poi, con la CPU al 300 % (i tre core del gioco
pieni), 1.130-1.680 prodotti e 200-770 mancanti ogni 10 s (10°-90° percentile). Nessuna saturazione del mix (0
campioni oltre 1,0, picco massimo 0,261): il gracchiare viene dai buchi, non dal clipping. Il decoder XMA (host) usa il
20-30 % di un core e consegna i dati in 0,14-0,56 ms di media (mediana 0,33): non è lui a mancare le scadenze. Il produttore è il mixer guest del gioco, che nel run 71 (§5
del doc 19) era il thread 16 (DSP in `sub_821737C0`, `sub_8216BFE8`, `sub_82185E38`) e che qui, senza campioni degli
stack, non si può ancora nominare. Due misure già esistenti dicono che non basta spostare lavoro: nel run 72 (vecchio
albero, lavori di sfondo di ReNut dal core 2 al core 0) l'audio restava al 51-67 %; qui i thread del gioco hanno tutti
priorità 0x3B e maschera 0x7, e Horizon li alterna a fette di 10 ms (`threading_switch.cpp`). Le due strade (mixer come
nativo, U10; priorità del mixer, U11) aspettano il profilo del run B.

**Filmato: stessa causa dell'audio (ipotesi forte, da misurare).** Il filmato iniziale di GTA IV non è un video: è una
cutscene del motore (animazioni da `anim/cuts.img`, audio in streaming da `audio/sfx/cutscenes.rpf`; nel log nessun file
video aperto). Le cutscene di RAGE seguono l'orologio del loro audio: con l'audio al 70 % anche la cutscene va al 70 %.
Da verificare nel run A: durata cronometrata della cutscene contro la quota audio nelle stesse finestre.

**Stutter: causa confermata, correzione pronta (cache).** Le 88 compilazioni attese da un draw (in media 0,88 s l'una, 77 s
in tutto) sono gli stutter; il resto delle 495 compilazioni gira sul thread uam fissato al core 2 (in media il **39 %** di
un core per tutto il run, mediana 49 %, massimo 79 %: nel profilo è il thread 25500, priorità 0x3E prestata a 0x3B), che
è uno dei tre core del gioco. Perché la cache di U6 non le aveva: (1) tutte le 88 attese hanno come unica costante
`925 = 63`: sono le **varianti generiche** dei pixel shader che scrivono sui target HDR di GTA IV. plume disegna con la
generica mentre compila la specializzata (`genericDrops` toglie solo bool, 900, 901, 903-906, 909, 920-923, 929); se manca
anche la generica, il draw aspetta. `deko3d_variant_cache.py` della base non produceva queste generiche, e non leggeva dalla
lista le costanti **924 / 925** (la parola `reserved` della chiave, bit 4-7 e 8 in su, `render/pipeline.cpp`). Prova sul
PC: dei 329 record compilati dalla console che le regole della base non danno, la lettura di 924/925 ne spiega 13, le
generiche come plume 204, **le due insieme 297**. (2) Gli 8 record in cui il byte della console differisce da quello del PC
sono **tutti** varianti attese da un draw: la console le compila in un solo passo (`banjo_deko3d_fast_first_compile`) e
le salva così; le altre 80 attese hanno gli stessi byte del PC.

**Clock: misura contaminata, da chiarire con l'utente.** Il codice non cambia i clock (l'override sys-clk dell'SDK è
spento, §2.11), ma plume mette la console in boost mode mentre un draw aspetta una compilazione
(`banjo_deko3d_compile_wait_boost`, default della base: FastLoad, CPU 1785 MHz e GPU 76,8 MHz). Il profilo ha letto
**CPU 2193 MHz** in 9 finestre di boost (non è un clock standard dell'Erista: il boost standard è 1785) e 1785 MHz con la
GPU a 460,8 negli ultimi 40 s, dopo l'ultimo boost: segno di uno strumento di clock attivo sulla console (Horizon OC o
sys-clk con un profilo di boost). Le finestre di boost sono anche quelle a 2-3 fps (GPU a 76,8 MHz). Per i run di misura:
strumenti di clock spenti e `banjo_deko3d_compile_wait_boost = false`.

**Prestazioni: CPU del gioco, non GPU.** Senza compilazioni in corso (es. 21:38:29-21:39:29) il render thread è a 11-19 ms
a frame e la GPU a 37-51 ms (a 460,8 MHz; occupata dal 45 al 68 % del tempo), il gioco a 10-13 fps: il limite non è né la
GPU né il render thread di ReNut. La CPU è al 299-309 % su 400 per quasi tutto il run: i core 0-2 del gioco sono pieni (il
core 3 resta al sistema, `banjo_system_core = 0`, scelta di U7). Per thread, in media: un thread guest al 64 %
(XThreadD80919A0, costante per tutto il run), il thread uam al 39 %, tre worker guest al 27 % ciascuno, il render thread
di ReNut al 21 % (core 1, 0x2C), il decoder XMA al 21 %, altri due guest al 13-17 %. Rispetto al vecchio albero la GPU è
molto più leggera (29,8 contro 78,6 ms) e il render thread pure (46,9 contro 70,1, compilazioni comprese); gli fps sono
più bassi per il carico delle compilazioni e della CPU. Quale thread guest sia sul percorso critico del frame lo dice solo
il profilo con gli stack (run B). I/O: letture lente dalla SD (10-20 ms per 32 KB-1 MB) sul thread di streaming, 1,3 s ogni
15 s nei caricamenti; nessuna attesa del gioco su di esse nel log.

**Renderer e illuminazione: niente di anomalo nel log, niente immagini.** Target 7e3 con la loro clamp (costante 990),
rampa gamma del gioco applicata, resolve con exponent bias 1 e resolve dalla griglia dei campioni come sul PC, texture
float a 32 bit campionate col punto (come la Xenos, che non le filtra), ripristino della depth disegnato e seguito
dall'invalidazione Zcull, nessun errore del renderer. **SMAA: 0 frame in tutto il run** (`banjo_renut_smaa` acceso,
qualità 1): il passo dopo cui si aggancia non arriva mai in GTA IV; è il punto U9 ("SMAA dopo la composizione finale di
GTA IV"), non cambia la luce. Durante le compilazioni i draw usano la variante generica (729 stadi nei primi due minuti
di gioco): stessi pixel per progetto, ma è anche ciò che si vede nei primi secondi in una zona nuova. Per giudicare la luce
servono catture della console (run B) da confrontare con quelle del run 72 (`out/console-runs/72-logs/captures`,
scena giusta confermata nel run 70) e con le catture PC della base (U4b-U5).

**Correzione circoscritta (solo strumenti del PC, nessun cambiamento della NRO):**

- `ultimate/tools/deko3d_variant_cache.py` (strumento della base): legge 924 / 925 dalla lista come `render/pipeline.cpp`,
  e per ogni pipeline aggiunge la variante che plume disegna mentre costruisce la sua (`plume_generic_drops`, le stesse
  costanti di `genericDrops`).
- `tools/ultimate/gta4_dksh_cache.py`: `--console-merge` unisce i record della cache della SD (il compilatore della NRO),
  tranne le varianti attese da un draw, che il PC ricompila per intero; al passo 5, con `--runtime-log`, i record diversi
  dove un draw aspettava sono contati come compilazioni in un passo della console, non come errori.

**La cache U8 (sul PC; manifest `docs/switch-port/ultimate-manifests/dksh-u8.json`, fatta sull'albero di questo commit):**
ingressi le liste di U6 (`pc-u5`, `console-run72`) e quella della SD dell'U8-1 (`console-u8a.pipeline_list.bin`, `0fca9879…`,
1.037 pipeline), il log `deko3d.log` della SD (`75f694eb…`, 152 righe di varianti), `--merge` la cache di U6 (`1d6f7737…`),
`--console-merge` la cache della SD (`7291aedc…`: la cache di U6 identica come prefisso più 495 record integri). Copie
degli ingressi in `ultimate/out/deko3d_shaders/lists/console-u8a.*` (fuori da git). Risultato: **7.490 record, 40.096.816
byte, SHA-256 `e8c7cff0…`**; 2.796 varianti compilate sul PC, **0 fallite**; 7.457 varianti esportate, ognuna col suo
record e la chiave controllata; i passi 1, 2 e 4 come in U6 (1.370/1.370, uam libreria = strumento 147/147). Passo 5
contro la cache della SD: **4.774 record identici, 8 in un passo (le compilazioni attese da un draw, sostituite qui dalla
compilazione completa), 0 diversi, 0 mancanti**. Prima della correzione dello strumento lo stesso passo dava 8 errori e 329
record della console assenti (`gta4_dksh_cache: 8 checks failed`).

**Pacchetto di test** `out/console-runs/u8-build` (fuori da git): **la NRO di U7 identica** (`f42ae3e4…`, controllata contro
`SHA256SUMS.txt` di U7: un'altra build avrebbe un'altra stringa del commit, quindi un altro binario), ELF e mappa di U7, la
cache U8, due toml: `gta4.toml` (run A: le opzioni di U7 + `banjo_deko3d_compile_wait_boost = false`, SHA-256
`c3bea098…`) e `gta4-profile.toml` (run B: come A + `switch_profile_stacks`, `renut_gpu_profile = 10` con
`renut_gpu_profile_counters`, `nfsmw_captura_cada_s = 30` (40 al massimo), `banjo_deko3d_variant_log`; `d302d8c6…`).
Entrambi letti con `tomllib`: chiavi al livello principale, opzioni di U7 invariate. README con le istruzioni per la SD.

**Atteso dal run A, e da verificare sulla console (nulla è dichiarato risolto):** 0 compilazioni attese da un draw sullo
stesso percorso e poche compilazioni in tutto (righe `uam N compiles`, `blocking: N a draw waited for`); il thread uam
quasi fermo, quindi il core 2 libero per il gioco; di conseguenza, forse, più fps e audio un po' meno affamato, ma la
causa dell'audio resta (sopra). Il run B dà: il thread del mixer e le sue funzioni, il thread sul percorso critico del
frame, la GPU per passata, le catture per la luce, le varianti esatte per la cache successiva.

**Cosa serve dall'utente:**

1. Strumenti di clock della console (Horizon OC / sys-clk): spenti per i run di misura, o il loro stato, perché il profilo
   ha letto 2193 MHz.
2. Run A e run B col pacchetto `u8-build` (README), sullo stesso percorso dell'U8-1: la cutscene iniziale fino in fondo
   (inizio e fine cronometrati), poi a piedi e in auto per 10-15 minuti.
3. Dalla SD, per ognuno: `logs/` intera, `deko3d.log`, `deko3d_shaders.bin`, `pipeline_list.bin`; dal run B anche
   `capturas/` e `deko3d_variants.txt`; le note su audio, stutter, luce e percorso.

### 2.13 U8: i run A e B (2026-11-13; Switch Erista, run dell'utente; pacchetto `u8-build`)

**File letti** (`out/console-runs/u8-build/A` e `/B`, fuori da git): `logs/renut_001.log`, `logs/rex/rex_profile.log`
(B: campioni degli stack ogni 1 ms, 6.924 per thread), `logs/rex/rex_stderr.log`, `logs/rex/banjo_profiler.log`,
`deko3d.log`, `deko3d_shaders.bin`, `pipeline_list.bin`, `renut/` (salvataggio e profilo); in B anche `capturas/` (10
PNG ogni 30 s; il nome della cartella è quello del codice, `GetExecutableFolder()/capturas`) e `deko3d_variants.txt`
(910 pipeline con le costanti esatte).

**Limiti delle misure.** Entrambi i run partono dal salvataggio della SD ("The Cousins Bellic", caricato da solo
all'avvio, nella stanza dell'appartamento), non dall'inizio: niente filmato iniziale, quindi **la velocità della
cutscene non è misurata**. Dopo il caricamento il percorso è diverso (A: 4.081 draw a frame di mediana; B: 3.287; il
primo U8: 2.293): gli fps di A, B e U8-1 non si confrontano tra loro. B ha il campionatore degli stack acceso (costa
CPU: totale 333 % contro 300 %), quindi i suoi fps non sono un risultato. **Confrontabili:** la quota audio (stessa
causa in ogni scena, sotto), le compilazioni (stessa cache di partenza), i clock, e i primi 60 s nella stanza (stessa
scena, 650 draw a frame sulla Switch, 639 sul PC). Clock letti: CPU 1020 / GPU 460,8 MHz in tutte le 31 + 28 finestre
(boost spento dal toml; strumenti di clock non più visibili).

| | U8-1 (cache U6) | A (cache U8) | B (cache U8 + profilo) |
|---|---|---|---|
| fps, mediana (10° pct) | 6,96 (4,75) | 6,38 (5,12) | 7,60 (5,84) |
| draw a frame, mediana | 2.293 | 4.081 | 3.287 |
| GPU ms a frame / render thread ms | 29,8 / 46,9 | 22,1 / 50,5 | 28,7 / 46,1 |
| CPU totale (di 400) | 299 | 300 | 333 (campionatore) |
| audio, quota del tempo reale | 69,8 % | 63,7 % | 63,9 % |
| compilazioni uam / attese da un draw | 495 / 88 (77,0 s) | **21 / 4 (7,4 s)** | **18 / 2 (5,1 s)** |

**Cache: la correzione di U8 funziona sulla console.** Compilazioni da 495 a 21 e 18, attese dei draw da 88 (77 s) a 4
e 2 (7,4 e 5,1 s), boost mai usato. Le varianti ancora compilate sono di shader che nessuna lista aveva (zone nuove), e
**le 94 attese di un draw dei tre run hanno tutte la sola costante `925 = 63`**: la generica dei pixel shader che
scrivono sui target HDR. Correzione (sotto): ogni pixel shader che dichiara 925 riceve anche la generica con 925 = 63.

**Audio: il mixer è il thread 26510 di B, e il suo limite è il calcolo, non lo scheduling (dimostrato).** Il profilo con
gli stack (aggregato per thread su tutto il run, simboli dall'ELF di U7 = stessa NRO) lo riconosce dalle funzioni: il
router XAudio `sub_8216D7E0` (il routing audio già visto nel run 64) e il suo mix `sub_8216DBB0`. Quota dei campioni del
thread: `sub_8216DBB0` 26,5 %, `sub_828C2D28` 15,5 %, `sub_828C6740` 12,9 %, `sub_828C5A58` 10,4 %, `sub_8216D7E0` 7,3 %,
`sub_82188C28` 6,5 %, `sub_828C7848` 6,1 % (le sette = 85 %). Per finestra di 10 s (B):

| Fase | CPU del mixer | Campioni in attesa nel kernel | Quota audio |
|---|---|---|---|
| caricamento, 60 fps | 3,7-13,8 % | 89-97 % | 99-100 % |
| gioco (24 finestre) | **74,8-88,6 %** | 11-23 % | **56,9-69,3 %** |
| gioco a 20-26 fps | 82-88 % | 13-20 % | 63-66 % |

Correlazione CPU del mixer / quota audio: -0,95 (più lavoro di mix, meno tempo reale), e la quota non segue gli fps.
Il mixer è quasi sempre in esecuzione: dargli priorità o un core non gli darebbe il tempo che manca. Per produrre il
100 % gli servirebbe circa il **130 % di un core** a 1020 MHz: va reso più veloce il suo codice. Cosa costa (letto con
`ultimate/tools/switch_hot_pcs.py` sulle istruzioni campionate):

- `sub_8216DBB0`: è `MixFrameBufferChannel` di XAudio, istruzione per istruzione lo stesso codice per cui la base ha già
  il nativo `n_rex_XAudioMixFrameBufferChannel` (Banjo; GTA IV aggiunge solo un ciclo di `dcbt`). Il codice generato è
  già NEON stretto: i campioni stanno sulle `ldr q`/`str q` (memoria) e sui `strb` dei store card a ogni vettore. Il
  nativo toglie i 4 store card per blocco (uno per l'intervallo): guadagno limitato.
- `sub_828C2D28`, `sub_828C7848` (filtri IIR): catene seriali di `fmadds` con il controllo d'esattezza della semantica
  PPC (`fcvt`/`fmov`/`cmp` dopo ogni prodotto). È l'aritmetica esatta: una riscrittura lo toglie solo cambiando la
  semantica (decisione dell'utente, non presa).
- `sub_828C6740`: i registri guest letti e riscritti nel contesto (`ldr`/`str [x19,#…]`) a ogni passo: **è il costo che
  una copia macchina toglie** (`localize_native.py --survey`: tutte e sette le funzioni accettate come copie).

Il decoder XMA (host, FFmpeg) è al 20 % di un core e consegna in 0,2 ms: non è la causa (non toccato). La catena
d'uscita è giusta (100 % quando il mixer ha tempo): non toccata.

**Prestazioni: CPU piena, e mezzo core sprecato in attesa attiva (dimostrato).** Run A: CPU al 300 % per tutto il
gioco (i core 0-2 del gioco pieni; il core 3 resta al sistema, `banjo_system_core = 0`). In B, CPU media per thread: mixer
68,6 %, render thread di ReNut 28,0 % (73 % in attesa), tre worker del gioco 27 % ciascuno (69 % in attesa), thread
principale 24,2 % (75 % in attesa), decoder XMA 20,5 %, thread `sub_8289F7A8` 19,9 %, render thread di GTA IV 19,2 %
(`CaptureDelta` di ReNut, 79 % in attesa), streaming 8,4 %, uam 6,8 %. Nessun thread è da solo il collo di bottiglia: i
thread del frame aspettano a turno, e la somma riempie tre core. **I tre worker passano più di metà del loro tempo attivo
in `sub_82221358`** (spinlock con backoff: un CAS, `sub_827DF9C0`, poi un conteggio da 8 a 1024 su una variabile dello
stack, senza mai cedere il core) chiamato da `sub_828E2350` per un contatore condiviso: circa **il 45 % di un core**
bruciato a girare, sugli stessi core del mixer e del thread principale; con le fette di 10 ms di Horizon chi tiene il
lock può restare fermo mentre un altro gira. Correzione possibile (non fatta qui): un nativo dello spinlock che cede il
core dopo pochi giri; i chiamanti chiamano `__imp__sub_82221358` direttamente, quindi serve rigenerare il codice e
ricompilare, e la copia macchina non è possibile (CAS e `lwsync`).

**Illuminazione: il difetto è del percorso Switch, non del livello GTA IV (dimostrato).** Prova: la build PC (D3D12)
fatta girare con `--user_data_root` su una **copia** del salvataggio di B (`renut/` della SD; nessun file della SD
toccato) parte nella stessa stanza; 639 draw a frame (Switch 650). L'eseguibile PC è quello costruito il 2026-11-13 alle
14:44, prima dei commit di U7: l'unica modifica di U7 al renderer (l'invalidazione dello Zcull) è solo deko3d, quindi
il percorso D3D12 è lo stesso della NRO. Catture PC ogni 15 s in
`out/console-runs/u8-light/pc-reference-capturas`. `tools/ultimate/u8_capture_compare.py` (nuovo) sulla cattura a 60 s:

| | luma media | terzo centrale | pixel quasi neri |
|---|---|---|---|
| Switch, B 60 s | 10,7 | 14,2 | **67,9 %** |
| PC, stessa stanza, 60 s | 15,3 | 17,6 | 28,1 % |

A 30 s (schermata di caricamento) i due coincidono (0,98 / 1,04): il confronto è valido. Sul PC Niko è illuminato
(giacca, pantaloni, luce ambiente sul pavimento); sulla Switch è una sagoma nera, come in strada (B 151 s) e all'esterno
di giorno (B 242 s: cielo chiaro, edifici, strada e auto quasi neri). Le sorgenti emissive (insegne, fari) e la cutscene
nell'ufficio (B 303 s) hanno luce: manca la luce del sole e quella ambientale. Il profilo GPU di B nella stanza: G-buffer
600 draw (4,4 M pixel), shadow map 1280x1280 1.700 draw, passate a 1280x720 solo colore 1,4 draw a frame.

Cosa è stato escluso, con le prove: il caso dei runs 68-69 (letture MS a zero) non c'è (`multisampled pipelines 0`);
l'exponent bias dei target (925 = 63 → -1 in sei bit, solo sull'alfa) è applicato giusto nel GLSL; le righe dei formati,
dei resolve e dei ripristini della depth sono le stesse sul PC e sulla Switch (confronto riga per riga dei log); il
resolve della depth (`depth_resolve.hlsli`) scrive il float che legge. Restano le parti del renderer che girano **solo su
deko3d** e sono attive in questa scena: hand-over della depth (la shadow map, 62 in 10 s: una a frame, campionata
dall'immagine del target invece che dal resolve), compressione della depth e le sue decompressioni (5.197 al bind, 39.518
rimandate, 5.819 fatte per una lettura), Zcull, i salti di barriera dei GPU round (207 e 4.449 in 10 s). Quale delle
quattro sia la causa la dice solo la console: **bisezione** col pacchetto `out/console-runs/u8-light` (sotto). Nessuna
correzione speculativa.

**Salvataggi sulla SD** (dal codice: `rex_app.cpp` `SetupEnvironment`, `content_manager.cpp`; e dal log):
`sdmc:/switch/theft4-ultimate/renut/` è la cartella dati utente della NRO. I progressi sono **solo** il contenitore
`renut/B13EBABEBABEBABE/545407F2/00000001/SGTA412/` (file `SGTA412`, 707.822 byte, e `__thumbnail.png`) con la sua
intestazione `renut/B13EBABEBABEBABE/545407F2/Headers/00000001/SGTA412.header` (`B13EBABEBABEBABE` è lo XUID del
profilo, `545407F2` il titolo, `00000001` il tipo "salvataggio"). `renut/545407F2/profile/User/63E83FFF` sono le
impostazioni del gioco, `renut/achievements/545407F2.toml` gli obiettivi: non sono progressi. Da non toccare: la NRO,
`gta4.toml`, `deko3d_shaders.bin`, `pipeline_list.bin`, `logs/`, `capturas/` e i file del gioco in
`sdmc:/switch/theft4/game/`. Il gioco carica da solo l'ultimo salvataggio: per ripartire dall'inizio, copiare prima
`renut/` sul PC, poi rinominare `renut/B13EBABEBABEBABE` in `renut/B13EBABEBABEBABE.bak` (reversibile: tutti i
salvataggi di quel profilo spariscono insieme, nient'altro cambia).

**Correzioni fatte (strumenti del PC; la NRO non cambia):**

- `ultimate/tools/deko3d_variant_cache.py`: `--generic-with ID=VALUE`, la generica di ogni shader che dichiara la costante
  anche con quel valore (GTA IV: `925=63`).
- `tools/ultimate/gta4_dksh_cache.py`: `--variants` (il `deko3d_variants.txt` della console: le pipeline esatte) e
  `--generic-with`, registrati nel manifest.
- `tools/ultimate/u8_capture_compare.py`: luma, terzo centrale e pixel quasi neri di ogni cattura, appaiati per secondo
  con un riferimento.

**Cache U8c (sul PC; manifest `dksh-u8c.json`):** liste U6, U8-1, A e B; i tre `deko3d.log`; `deko3d_variants.txt` di B (910
pipeline esatte); `--generic-with 925=63`; `--merge` la cache U8, `--console-merge` la cache della SD dopo B (la cache U8
identica come prefisso, +21 record in A, +18 in B). Risultato: **8.016 record, 44.202.200 byte, SHA-256 `3a7b7376…`**; 581
varianti compilate sul PC, 0 fallite; passi 1, 2, 4 come prima; passo 5 contro la SD dopo B: 7.526 identici, 3 in un passo
(compilazioni attese da un draw, qui complete), 0 diversi, 0 mancanti. Controllo diretto delle chiavi: **i 704 pixel
shader che dichiarano 925 hanno tutti la generica `{925:63}`** (148 nella cache U8). Nel pacchetto `u8-light`, insieme ai
toml della bisezione: non cambia i pixel, toglie le attese nelle zone nuove (da verificare sulla console).

**Non fatto in questa fase, e perché:** le copie macchina delle sette funzioni del mixer, il nativo del mix XAudio legato a
`sub_8216DBB0` e il nativo che cede il core nello spinlock dei worker richiedono `post_codegen.py` completo (le copie) o una
rigenerazione (lo spinlock, chiamato come `__imp__`), poi una build PC con la guardia densa sul salvataggio e una build
Switch completa (ore, un job): sono il prossimo blocco di lavoro (U10/U11 anticipati per l'audio), da fare dopo la
bisezione della luce, che non richiede build. Per l'audio, anche fatte tutte, le stime della guida della base (copie
-4 % sul thread, nativi -7/-26 % sulla propria funzione) non bastano da sole al 100 %: il filtro IIR esatto resta il
costo maggiore e toccarlo vuol dire cambiare la semantica dei `fmadds` (decisione dell'utente).

### 2.14 U8: la bisezione L0-L5, la causa della luce, lo spinlock dei worker (2026-11-13)

**I sei run erano validi** (letto nei log, non dai toml): L1 nessuna riga di hand-over; L2 "depth targets not compressed" e
0 decompressioni; L3 "zcull off"; L4 `sasrt_gpu_lean_barrier_skip (off)` e `sasrt_gpu_target_wait_tracked (off)`; L5
"non-cpu master OFF" (85 miglioramenti attivi su 158 invece di 146; lo Zcull, la compressione e l'hand-over restano
accesi: non sono nella sua tabella). **La stanza a 61 s è la stessa in tutti e sei** (`u8_capture_compare.py`: 67,6-67,8 %
di pixel quasi neri, il PC 28,1 %; le differenze a 60-76 s di L1 e L3 sono il giocatore che si è mosso). Le quattro parti
solo deko3d sono escluse.

**La causa: una regola del port di Halo 3 entrata con la base (dimostrata sul codice e sui dati; da confermare sulla
console col run F1).** `banjoTrim()` di `ultimate/app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h`, chiamata a
ogni uscita di ogni pixel shader, nella base fa due cose in più del vecchio albero, **solo nello SPIR-V** (`#ifdef
__spirv__`: deko3d e Vulkan, non il DXIL del PC): la codifica gamma di un target `k_8_8_8_8_GAMMA` (costante 924) e
**l'alfa moltiplicata per 2^bias** del target (costante 925, `saturate`). Le accende `halo3_renut_target_gamma_bias`
(`renut_engine/gpu/ultimate.cpp`, **acceso di default per ogni gioco**; il commento dice "Halo 3's menu was nearly
transparent without the bias"), che in `render/draw.cpp` (`key.reserved`, bit 8 e oltre) mette il bias di
`RB_COLOR_INFO` nella chiave della pipeline. Entrata nel repository con l'import della base (U1, `ebd1da4`; base
`8de5d51a`, halo3-nx 2026-11-03). Prove:

- il vecchio albero (runs 70-72, luce giusta sulla console) non ha né le costanti 924/925 né quel codice
  (`thirdparty/renut/.../shader_common.h`: `banjoTrim` fa solo l'output trim; 0 dei suoi 1.370 GLSL nomina la 925, tutti i
  704 pixel shader della base sì);
- GTA IV scrive i suoi target HDR con bias -1 (`925 = 63`, sei bit con segno): **249 delle 910 pipeline del run B**, 97
  pixel shader; il loro blend usa l'alfa sorgente (186 `SRC_ALPHA / 1-SRC_ALPHA`, 9 `SRC_ALPHA + ONE`, luci additive
  pesate dall'alfa; 42 `ONE + ONE`, 12 `ONE / ZERO`): sulla Switch ognuna mescola con metà opacità, sul PC con quella
  piena. Le altre differenze SPIR-V dei due `shader_common.h` (fetch delle texture: le stesse conversioni `renutSigns`, i
  flag nella parola del sampler invece che della texture) non cambiano i valori;
- il rapporto di luminanza misurato (0,70 sull'immagine, 0,82 al centro) è quello di un fattore 2 sull'intensità lineare
  dopo la gamma;
- sul PC (D3D12) dallo stesso salvataggio `--halo3_renut_target_gamma_bias=false` lascia la stanza com'era (28,1 % di
  pixel quasi neri a 90 s, il caricamento più lento): nessuna regressione dove la luce è giusta. La prova diretta sul PC
  col backend Vulkan (che compila lo stesso SPIR-V) non è possibile qui: `renut.exe --renut_gpu_backend=vulkan` va in
  crash nel driver (`amdvlk64.dll`, 0xC0000005) subito dopo la swapchain.

Quanto vale per il 360 la regola di Halo 3 (bias sull'alfa, non sul colore) resta aperto; per GTA IV contano i fatti: senza
la regola il vecchio renderer e il PC disegnano giusto. **Prova sulla console, senza build:** `out/console-runs/u8-light2`
(`gta4-F1.toml` = L0 + `halo3_renut_target_gamma_bias = false`, README). Se la stanza torna come quella del PC, la riga va
in `app/gta4_switch_defaults.toml` (la regola di un altro gioco spenta per GTA IV) con la build successiva.

**Lo spinlock dei worker: è un'attesa di lavoro, non una sezione critica lunga.** `sub_828E2350` (raggiunta solo dalla
tabella delle funzioni: nessuna chiamata diretta nel codice generato) è il ciclo dei tre worker sulla coda di un job:
prende lo spinlock a `0x8312F23C` (`sub_82221358`: CAS `sub_827DF9C0`, poi un ritardo contato da 8 a 1.024 passi su una
variabile dello stack, mai una cessione del core), legge quanti elementi restano (totale `*(*(ctx+20)+8)` meno l'indice
condiviso a `0x83130250`), ne prenota al massimo `*0x82A939D4`, rilascia il lock, esegue gli elementi (`sub_828E1F68`) e
ricomincia finché il byte +12 del job non dice che è finito. **Quando non c'è niente da prenotare il worker riprende
subito il lock**: tre worker lo fanno di continuo sulla stessa riga di cache, e il lock resta occupato poche istruzioni
alla volta. Nel profilo di B il lavoro vero (`sub_828E1F68`) quasi non compare: i worker girano a vuoto in attesa che il
thread principale dia lavoro, sui core del thread principale e del mixer. Correzione (non una priorità, non un core):
`ultimate/app/src/cpu/gta4_job_poll.cpp`, `REX_HOOK_RAW(sub_828E2350)` con **le istruzioni generate della funzione copiate
identiche** (un diff automatico contro `renut_recomp.140.cpp` dà solo le tre righe aggiunte) più una chiamata a
`rex::thread::MaybeYield()` quando il passaggio non ha prenotato nulla e il job è aperto (la politica di attesa a polling
dell'SDK, `switch_yield_backoff`: cede il core, poi sonni di 100 us per chi continua a interrogare). Nessun lock nuovo,
nessuna scrittura in più nella memoria guest: il gioco vede solo passare il tempo, come quando il thread non è
schedulato. `gta4_job_poll_yield = false` torna al ciclo originale (A/B). Compilato con le regole del codice generato
(`BANJO_HOT_SOURCES`, le definizioni dei round CPU); `tools/ultimate/gta4_hook_bindings.csv` ha la riga `binding` e la
riga `reviewed-file`; `hook_census.py --strict` 0 problemi.

**Prova sul PC (D3D12, build incrementale: 16 passi):** quattro run di 240 s dallo stesso salvataggio (ognuno su una copia
nuova di `renut/`), due coppie in ordine inverso, il tempo CPU del processo letto ogni 15 s; si confronta la pendenza tra 120
e 240 s, quando tutti e quattro sono nella stanza (catture dalla 60-75 s in poi). Il log dice in ogni run quale ciclo gira
(`[gta4] job workers' poll loop (sub_828E2350): ...`).

| Coppia | ciclo originale | con la cessione | differenza | fps in gioco (originale / con) |
|---|---|---|---|---|
| 1 (on poi off) | 2,714 core | 2,548 core | -6,1 % | 67,0 / 67,3 |
| 2 (off poi on) | 2,623 core | 2,513 core | -4,2 % | 66,8 / 67,3 |

Le catture sono le stesse una per una (luma 14,3-15,4, 28-31 % di pixel quasi neri in entrambe le forme): il gioco disegna
lo stesso. Nessun blocco, nessuno stallo nei quattro run con la cessione (caricamenti e gioco normali). Sul PC, che ha più
core e non è limitato dalla CPU, il guadagno è solo CPU liberata (4-6 % del processo); sulla Switch, dove i tre core del
gioco sono pieni, è la CPU che i worker toglievano al thread principale e al mixer: **da misurare sulla console** (righe
`[renut] ... game frames`, CPU per thread col profilo, quota audio), con `gta4_job_poll_yield = false` come metà A/B.

**Build Switch U8-C** (incrementale: 6 passi; `16ccb7780250` everyday; manifest `switch-u8c.json`): NRO 56.416.453 byte,
SHA-256 `bf8b73bf…`; nell'ELF `sub_828E2350` viene da `gta4_job_poll.cpp.obj` (simbolo forte) e il `__imp__sub_828E2350`
generato è stato scartato dal linker (nessuno lo chiama più). Pacchetto `out/console-runs/u8-c` (fuori da git) con la cache
U8c e due toml: C1 = regola di Halo 3 spenta + cessione dei worker accesa, C2 = regola spenta + ciclo originale (la metà A/B),
`README-U8C.txt`.

**Audio: cosa si può togliere restando bit-exact (letto con `switch_hot_pcs.py` sulle istruzioni campionate di B).** Le
sette funzioni del mixer (thread 26510) e il loro costo:

| Funzione | Quota del mixer | Dove cadono i campioni | Riscrittura bit-exact possibile |
|---|---|---|---|
| `sub_8216DBB0` (`MixFrameBufferChannel` di XAudio) | 26,5 % | `ldr q`/`str q` (memoria) e un `strb` di store card per vettore | il nativo della base (`n_rex_XAudioMixFrameBufferChannel`, stesse istruzioni): un store card per intervallo; il prefetch del `dcbt` del 360. Guadagno limitato (memoria) |
| `sub_828C2D28` (filtro, catena di `fmadds`) | 15,5 % | sull'`fmadd` che aspetta il risultato precedente | no: è la latenza della catena seriale; riordinare cambia gli arrotondamenti |
| `sub_828C6740` | 12,9 % | registri guest letti e riscritti nel contesto (`[x19,#…]`) a ogni passo | sì: la copia macchina (registri come locali) toglie quel traffico |
| `sub_828C5A58` | 10,4 % | confronti sui bit dei double (`fmov`/`cmp`/`ccmp`/`fcsel`): l'esattezza dei casi limite | in parte, provando i valori a monte |
| `sub_8216D7E0` (router XAudio) | 7,3 % | **29 % sulle `msr fpcr`** (cambio di modo flush tra scalare e VMX, ~40 ns l'uno) | sì: copia/nativo che sposta il cambio fuori dal ciclo |
| `sub_82188C28` (ricampionatore: tabelle in virgola fissa) | 6,5 % | `ldr q` indicizzati (memoria) | poco |
| `sub_828C7848` (filtro) | 6,1 % | come `sub_828C2D28` | no |

Stima: copie macchina delle sette (tutte accettate da `localize_native.py --survey`), il nativo del mix e il cambio di FPCR
fuori dal ciclo tolgono forse il 10-20 % del mixer; al mixer serve il 25-35 % per arrivare al 100 % del tempo reale. Il
resto è aritmetica esatta: le sole leve più grandi non sono bit-exact (`BLUR_FPCR_ONE_MODE`: niente cambi di FPCR, ma i
denormali scalari azzerati; i filtri senza il controllo d'esattezza dei `fmadds`) e **non si accendono senza
l'autorizzazione dell'utente**. Il prossimo passo bit-exact (copie + nativo, `post_codegen.py` completo e ricompilazione
di tutto il codice generato, guardia densa sul PC dal salvataggio) è pronto da fare dopo la prova U8-C.

### 2.15 U9: C1 e C2 letti; i semafori del thread principale; la luce ristretta al solo deko3d (2026-11-14)

**Validità di C1 e C2 (dai log, non dai toml).** In `C1/logs` ci sono due log: `renut_001.log` (00:12, "non-cpu master
OFF", 85 di 158) è un resto di L5, il log di C1 è `renut_002.log` (10:27; `u8_run_report.py` legge il più recente). In
`C1/capturas` 5 delle 16 PNG sono di L5 (hash identici a `u8-light/.../l5/capturas`); le 12 di C1 sono le altre. Il log
nomina il ciclo dei worker in entrambi ("an idle pass yields" in C1, "the original loop" in C2). La regola di Halo 3 spenta
non ha una riga di log, ma lo dicono le varianti compilate a runtime: 72 delle 84 di C1 e C2 non hanno la costante 925
(nei run A e B, con la regola accesa, 24 su 39 la avevano). Clock 1020 / 460,8 MHz in entrambi.

**I worker: la correzione funziona sulla console, il confronto degli fps no.**

| Stanza, 60-120 s | C2 (ciclo originale) | C1 (cessione) |
|---|---|---|
| i tre worker (`sub_828E2350`) | 43-46 % di un core ciascuno | ~6 % ciascuno |
| draw per frame | ~650 | ~2.400 (un'altra inquadratura: la cattura mostra la tromba delle scale) |
| compilazioni uam nel run | 20 (media 3,5 s) | 142 (media 1,8 s; il thread uam al ~50 % del core 2) |
| fps | 11-13 | 12-14 |

La cessione libera circa 1,1 core sulla console. C1 non è però confrontabile con C2 negli fps: la stanza era inquadrata
diversamente e C1 ha pagato 142 compilazioni. Le ha causate il toml di prova stesso, perché la regola di Halo 3 spenta
azzera la costante 925 e la cache U8c era fatta con la regola accesa; C2 ha trovato sulla SD le varianti compilate da C1.
**Nessun miglioramento degli fps dichiarato o smentito.** La correzione resta accesa.

**La luce: la regola di Halo 3 non è la causa (smentita).** Con la regola spenta la stanza di C2 a 61 s è quella di L0
(luma 10,8, 67,7 % di pixel quasi neri, contro 28,1 % sul PC). La regola resta al default della base (accesa): spegnerla
non cambia la stanza e costerebbe le varianti della cache. Altri due fatti, confrontati sul PC dallo stesso salvataggio:
- **la luminanza 1×1 che il gioco rilegge** per l'esposizione (`sasrt_renut_resolve_readback`, i valori scritti nel log)
  è la stessa: 13,6; 3,99 contro 3,98; 13,23 contro 13,24, poi la stessa discesa nella stanza. Il gioco calcola quindi la
  stessa esposizione e la differenza nasce nel rendering GPU;
- **il ripristino della depth dal colore** (la correzione del vecchio albero per il G-buffer di GTA IV, ora
  `sasrt_renut_depth_restore`) avviene lo stesso numero di volte. Solo la ricerca dei candidati è più frequente sulla
  Switch (`sasrt_rt_restore_candidates`: ~556 a frame contro ~13).

Quello che L0-L5 non ha mai spento:
- i 46 interruttori del GPU round di SASRT accesi di default (`sasrt_gpu_*`, quasi tutti solo deko3d), che non sono nella
  tabella dell'interruttore non-CPU;
- `lo_deko3d_*`;
- le costanti delle passate GLSL di vivapinata (975-977) e di dishonored (`clamp7e3`, 990).

Le passate GLSL di `ultimate/tools/deko3d_glsl_opt.py` sono 49, contro le 27 del vecchio albero
(`tools/deko3d/deko3d_glsl_opt.py`, sa2-nx `adf3a4e`). Le 22 nuove vengono da altri port:

`scalecodes fetchspec skinonce samplerk plainfetch fetchconv onex onexsize noearly earlykill facesign clipspec nofxaa
vsepi stack3d fetchendian expfmt vskill expwin expparts clamp7e3 invariant`

Le più dipendono da una costante che quegli interruttori accendono. Il vecchio albero, con la luce giusta sulla console
(run 72: Niko illuminato nell'appartamento), non aveva né quelle passate né il GPU round. **Prova sulla console:** il
toml G1 del pacchetto `u9` spegne tutti e 51 (elenco generato dalle sorgenti). Se la stanza torna come sul PC, la causa è
fra loro e si biseziona; se no, restano le passate senza costante, e si prepara una NRO con le passate di sa2-nx soltanto.

**Il thread principale aspetta i suoi semafori (la causa CPU trovata nel run B).** Nel run B il thread principale
(`Main XThread`, 24,7 % di un core) è nel kernel il 71 % dei campioni:

| Attesa (stack simbolizzati con `nm` dell'ELF) | Quota delle attese |
|---|---|
| `pthread_mutex_lock` dentro `PosixSemaphore::Release`, da `NtReleaseSemaphore` (`sub_829A2240`) | **44,6 %** |
| stack interrotto (codice guest) | 24,6 % |
| critical section (`sub_827F13D8`) | 11,9 % |
| `KeDelayExecutionThread` (`sub_829A95D0`) | 9,7 % |

Il gioco rilascia i semafori che svegliano i suoi job thread. Il semaforo dell'SDK (`PosixCondition<Semaphore>`) usa un
mutex e una condition variable con `notify_all`. Su Horizon il risveglio dà il mutex al thread svegliato prima che giri,
e con i tre core pieni quel thread può aspettare il suo core per un intero quanto di 10 ms: chi rilascia di nuovo lo
stesso semaforo resta bloccato dietro di lui. La base aveva già documentato e corretto lo stesso difetto per gli **Event**
(sasrt-nx port 65, `thread_event_address_wait`: il 5,5 % del thread di gioco di SASRT), non per i Semaphore.
**Correzione** (`ultimate/sdk/src/core/address_event_core.h` e `threading_posix.cpp`, chiave
`thread_semaphore_address_wait`, default acceso, solo Switch):
- chi è bloccato su un semaforo dorme su un indirizzo (`svcWaitForAddress`), come gli Event;
- un rilascio di n unità ne consegna fino a n direttamente ai thread bloccati (`ReleaseLocked`: la semantica del 360,
  un'unità rilasciata appartiene a un thread in attesa) e li sveglia dopo aver lasciato il mutex;
- il thread svegliato prende il mutex solo quando gira.

Il nucleo è lo stesso degli Event (`Block` è ora `BlockUntil` con un predicato). La riga di avvio del gioco dice quale
attesa gira (`[gta4] semaphores: ...`).

Prove sul PC (test host con `WaitOnAddress` al posto di `svcWaitForAddress`, CRT statico):
- **`tools/ultimate/test_address_semaphore.cpp`, 3 esecuzioni su 3 tutte OK:** ping-pong da 200.000 giri; 20.000 round di
  4 thread bloccati liberati da un `Release(4)` e da quattro `Release(1)` consecutivi (nessuna unità rimasta a un thread
  addormentato); massimo, conteggio precedente, attese a zero; 1.000 interruzioni (peggiore 0,11 ms); tre mix casuali
  (fino a 9,5 milioni di unità) con **conservazione esatta** (unità rilasciate = prese + rimaste) e contatori a zero;
- **il test degli Event della base**, con l'header nuovo e con quello originale, si bloccava 2-3 volte su 3. È un difetto
  del test, non del nucleo: in chiusura fa 16 `Set()` di un Event auto-reset e poi `join()`, e i Set fatti mentre nessuno
  è bloccato si fondono in un solo segnale. Con la chiusura che ripete i Set finché i thread escono (copia nello
  scratchpad, il file della base non è toccato), **originale e nuovo passano 3 volte su 3**, con gli stessi conteggi
  della rete di sicurezza (0-5).

Sulla console va misurato: la quota del thread principale in `PosixSemaphore::Release` (profilo con campioni) e gli fps.

**Audio: il mixer è limitato dal calcolo, non dalla contesa (run B).** Il thread 26510 gira all'80 % di un core e aspetta
nel kernel il 18 % dei campioni (un quinto di quell'attesa è il suo evento di lavoro, `sub_82169400`): non è sottratto da
altri thread. Al 100 % di un core produrrebbe circa l'85 % del tempo reale. Due cose del piano bit-exact (§2.14) cambiano:
- il codegen Ultimate tiene già i registri di `sub_8216DBB0` e del router in variabili locali (`REX_LEAF_LOCAL`,
  `REX_LOOP_LOCAL`) e ha già tolto i cambi di modo FP ridondanti (4 nel router). Il nativo del router del vecchio albero
  (run 64, `tools/switch-game/nx_audio_routes.h`, `50492e5`) dava soprattutto questo, quindi non va riportato così;
- il router si raggiunge solo dalla tabella delle funzioni (un hook basterebbe), ma il guadagno rimasto è il nativo del
  mix della base (`n_rex_XAudioMixFrameBufferChannel`). La funzione di GTA IV è quella che descrive (stessi g0-g3, 16
  blocchi, tutte le letture prima delle scritture) e il router non legge i registri volatili dopo la chiamata. La strada
  verificata della base però è la pipeline delle copie (`localize_native.py`, `hand.txt`, la guardia che confronta
  memoria e registri): è il lavoro U10 sulle sette funzioni, non un hook scritto a mano.

**UltraNX.** Nessuna occorrenza in nessun repository (questo, la base, rr6, katamari, nfsmw, torchlight, sa2, il vecchio
`C:\dev\Theft4`), né nei documenti. Le differenze fra vecchio e nuovo albero che toccano un overlay di sistema:
- la NRO Ultimate si collega a SaltySD/SaltyNX all'avvio (`sdk/src/ui/switch_saltynx.cpp`: scrive il blocco FPS, legge
  quello di Reverse-NX; per avere una sessione chiude per un momento `sm:` o alza il limite delle sessioni con
  `svcSetResourceLimitLimitValue`); il vecchio albero non lo faceva (doc 12: "noi: no");
- la base imposta `appletSetFocusHandlingMode(SuspendHomeSleepNotify)`.

Senza sapere quale overlay sia e come non si apra, nessuna correzione: domanda aperta all'utente.

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
| U2 | 2026-11-12 | (sotto) | **Difetto della base trovato:** un clone pulito non compila l'SDK: `FFmpeg/config.h`, `libavutil/avconfig.h` e `libavutil/ffversion.h` mancano dal commit (li esclude il `.gitignore` di FFmpeg stesso). Ricostruiti in `tools/ultimate/ffmpeg/` (il dispatcher del fork Xenia a `0604b46` + ramo `__SWITCH__` -> `config_switch_aarch64.h` + `config_masseffect_wmv3.h` dopo ogni piattaforma, come dicono i commenti della base); lo script di fetch li copia. Da segnalare alla base (handoff). Generatore `rexglue.exe` 0.10.0 costruito (769/769) |
| U3 | 2026-11-12 | (sotto) | Game layer di Katamari tolto dalla generazione: `renut_config.toml` di GTA IV (chiavi "senza audit" della base; `non_volatile_as_local` come nei nostri run, v64-v127 in ctx; `elide_lr_stores` e `non_argument_as_local` spenti fino agli audit), i nostri 25 confini/frammenti in `config/renut_funcs.toml`, i 653 gap in `config/gta4_eu_base_gaps_bulk.toml`, liste delle copie / `hand.txt` / promozioni / scelte lr-r1 vuote. **Generazione:** 321 file in 72 s, 37.304 funzioni; 23 gap del nostro elenco bulk "not in any code region" diventano stub vuoti, **come col generatore 0.9** (es. `sub_8298EC34`, 12 byte di riempimento dopo un `blr`: corpo vuoto anche in `generated/eu-base`). **`post_codegen.py` completo:** ogni passata senza errori (`direct` 124.262 chiamate; `store_cards`, `flush_mode_flow` 11.986 controlli provati, `single_floats`, `fma_single` 13.758, `reg_liveness` 121.770 chiamate con registri da non passare...). Attenzione: `direct` rispetta "575 hooked addresses", che sono ancora quelli di Katamari nel renderer (U4 li rilega, poi si rigenera). **Census della calling convention:** ingresso, 1 voce (`v0` in `sub_82968AB8`, raggiunta per puntatore): è il mirror `REX_LOOP_LOCAL` di loop_locals, il primo accesso guest è una scrittura (`vmrghw v0,v12,v8`) senza etichette raggiungibili prima: indipendente dal valore. Uscita: 572 chiamate indirette leggono dopo la chiamata un vettore che la ABI d'uscita dà per morto (v8 493, v9 462, v7 360...): non verificate una per una, quindi **`SASRT_REG_LIVENESS_EXIT_ABI=0`** per GTA IV (ripiego di `building.md`), messo in `tools/ultimate/env.bat`. La rigenerazione con quel valore è stata **fermata per memoria scarsa** a metà di `post_codegen` (dopo `reg_liveness`): `app/generated` va rigenerato per intero prima di una build |
| U3, rigenerazione | 2026-11-12 | (sotto) | `tools/ultimate/regenerate.ps1`: cancella `app/generated`, generatore, `post_codegen.py` completo, poi `tools/ultimate/generation_manifest.py`; si ferma al primo passo che non arriva in fondo. Eseguito con 9,2 GB liberi: codegen 79 s (321 file scritti, 0 invariati), passate 1.093 s fino a "optimised codegen steps done". **Manifest** `docs/switch-port/ultimate-manifests/gen-u3-exitabi0.json` (commit del repository `30eafa9`, base `8de5d51a`, generatore SHA-256 `c9a59685…`, `default.xex` SHA-256 `74d480d2…` 545407F2 / 4A53F9F6 / 0.0.0.6, `SASRT_REG_LIVENESS_EXIT_ABI=0`, hash di ogni input, 82 righe delle passate): 342 file, **hash combinato `e10657be810ea2f4…`**, 37.067 funzioni definite (le passate ne contano 37.304 con i frammenti; la tabella di `renut_init.cpp` ha 37.541 voci), 23 stub (i gap di cui sopra). Confronto con la prima generazione completa (senza la variabile): **cambiano solo le righe di `reg_liveness`** ("exit bound off": 112.903 chiamate dirette con registri da non passare invece di 121.770; 1.302 indirette invece di 8.709) e il riepilogo di `leaf_dead_stores`; ogni altra passata dà gli stessi numeri |
| U4, stato iniziale | 2026-11-12 | (sotto) | `tools/ultimate/hook_census.py` (ogni indirizzo 82xxxxxx nel codice dell'app e dell'SDK, commenti e stringhe esclusi; compilato o no secondo i CMake dell'app e gli include; contro `tools/ultimate/gta4_hook_bindings.csv`, i 23 hook del renderer funzionante, ognuno ritrovato nel doc 18 e nella tabella X() di `gta4_d3d.h`): **335 hook vivi in file compilati, tutti agli indirizzi di Katamari**; 9 cadono su un inizio di funzione GTA IV e la sostituirebbero (`824D09C8` FindTextureSize, sei render-state, `824E11A0` InitializeApiState di Katamari...), gli altri 326 nominano indirizzi che in GTA IV non sono funzioni (il link fallirebbe); 0 dei 23 binding GTA IV legati. `tests/renut/test_ultimate_gta4_layout.py` (ogni costante di `d3d.h` e `d3d_native.h` della base contro il valore GTA IV del renderer funzionante, con le istruzioni rilette dal codice 0.10 rigenerato): **10 valori diversi, non 5**: i cinque di `d3d.h` (§2.4, ognuno ritrovato nelle istruzioni) e in `d3d_native.h` `CommandBuffer` 13.500 e `SideBuffer` 16.712 (ritrovati nelle istruzioni), `AutoDepthStencil` / `AutoFrontBuffer` / `AutoBackBuffer` 14.808 / 14.812 / 14.816 (spostati di +8 come i campi vicini; nessun codice GTA IV li legge, nota del renderer funzionante) |
| U4a | 2026-11-12 | `d3bf040`, `9e3e75a`, `e012b96`, `7fd9864` | i 10 valori del layout GTA IV nella base (test 0 differenze); censimento degli offset in linea (144, 56 diversi); script per sganciare i binding di Katamari (§2.4-§2.6) |
| U4b | 2026-11-13 | (questo commit) | 23 hook GTA IV legati ai corpi della base, percorso del device di sa2-nx, Swap / SetFence / gamma / clear / resolve / tiling adattati, 323 binding di Katamari sganciati; `hook_census.py --strict` 0 problemi; rigenerazione `gen-u4b-hooks.json` (`08e5df6f…`), 0 chiamate dirette ai binding; build PC completa con gli shader; **primo avvio completo sul PC** (D3D12, 120 s): scena d'apertura giusta come R3, 60 fps, nessun errore di memoria fisica o chiamata a guest 0. Aperto: 60.434 inoltri di `stack_forward.py` sbagliati su 7,7 M (da risolvere prima di ogni build Switch). §2.7 |
| U5a | 2026-11-13 | `5000be5` | `stack_forward.py` corretto alla radice (chiamate e salti nel corpo delle istruzioni); 4 punti sbagliati trovati col reporter + PDB; rigenerazione `gen-u5-stackfwd.json` (`3e76d58a…`, 67 inoltri in 39 funzioni); PC: 8,5 M inoltri controllati, **0 differenze**. §2.8 |
| U5b | 2026-11-13 | `db37f00` | correzioni del vecchio renderer confrontate con la base: 7e3, ripristino della depth e resolve MS coperti; scambio dei mezzi tile in pixel guest riportato; attesa delle fence D3D12 per valore e vsync PC spento (avvii 12/12 invece di 7/12); rampa gamma del gioco accesa; Zcull pronto per U7; percorso SA2 identico, GuestPtr della base corretto su Switch, chiamate in uscita senza problemi reali con `EXIT_ABI=0`. §2.9 |
| U6 | 2026-11-13 | `ede3a9b` | solo PC. Strumenti Switch della base integrati: uam e SPIRV-Cross per PC (`build_deko3d_tools.bat`, uam sull'ABI MSVC), contenitori coi nomi della base (`gta4_shader_dump.py`; i vecchi nomi nascondevano i pixel shader all'early alpha e avevano fatto copiare 1.250 duplicati in eu-private, ora spostati), cache e controlli (`gta4_dksh_cache.py`). 1.370 shader tradotti e compilati, 0 falliti; tabella PC = tabella Switch; cache di 4.287 varianti da due liste, 0 fallite, ogni chiave controllata; i 409 DKSH che la console aveva compilato nel run 72 rifatti identici sul PC. Manifest `dksh-u6.json`. §2.10 |
| U7 | 2026-11-13 | `12b20c2`, `aa88997` | build Switch, nessun run su console. Prima NRO di GTA IV sulla base (deko3d, nessun driver Vulkan), everyday: toml Switch di GTA IV (gamma accesa, niente di Katamari, nessuna modalità a 4 core), Zcull dopo il ripristino della depth, cartella SD propria; NRO `f42ae3e4…` riproducibile byte per byte; controlli del pacchetto e della cache superati; pacchetto `out/console-runs/u7-build`, manifest `switch-u7.json`. NRO guard non costruita (U10). §2.11 |
| U8, primo run | 2026-11-13 | (questo commit) | **primo run su console (Erista, utente)**, NRO `aa88997`: avvio, filmato e gioco per 14 minuti senza crash. 6,96 fps (mediana), GPU 29,8 ms (78,6 nel vecchio albero), CPU 299 % su 400; audio al 69,8 % del tempo reale per i frame che il mixer guest non produce in tempo (frequenze giuste: 100 % a 60 fps); filmato = cutscene a orologio audio (ipotesi); stutter = 88 compilazioni attese da un draw (77 s), generiche con 925 che la cache non aveva. Strumento della cache corretto (924/925, generiche come plume, unione della cache della SD): 7.490 record, passo 5 senza errori. `u8_run_report.py`. Clock letti a 2193 MHz nel boost (strumento sulla console). **Nessuna correzione dichiarata riuscita:** pacchetto `u8-build` (stessa NRO, cache nuova) per i run A e B. §2.12 |
| U8, run A e B | 2026-11-13 | (questo commit) | dal salvataggio, stessa NRO: la cache U8 **funziona sulla console** (compilazioni 495 -> 21 / 18, attese dei draw 88 -> 4 / 2; clock standard); le 94 attese dei tre run sono tutte generiche con 925 = 63 -> `--generic-with 925=63`, `--variants`. Audio: mixer identificato (thread 26510, DSP `sub_8216DBB0`/`828C…`), **limite di calcolo** (75-89 % di un core per il 57-69 % del tempo reale; non scheduling). Prestazioni: CPU piena, tre worker in attesa attiva su uno spinlock (~45 % di un core). Luce: **sbagliata solo su deko3d** (PC dallo stesso salvataggio giusto: 28 % contro 68 % di pixel quasi neri); bisezione sulla console col pacchetto `u8-light`. Salvataggi: `renut/B13EBABEBABEBABE/545407F2/…`. Strumenti `u8_capture_compare.py`. §2.13 |
