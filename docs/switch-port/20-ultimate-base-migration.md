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
