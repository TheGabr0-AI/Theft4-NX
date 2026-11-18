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

I pacchetti per la console `u12` e `u13` (§2.18-§2.19) e il prossimo `u14` proseguono il lavoro di U10-U11 (audio, luce, copie macchina) con la numerazione dei pacchetti; le fasi U12 (installer) e U13 (RAM 1600 MHz) di questa tabella non sono iniziate.

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

**Build Switch U9** (`c9c511e` everyday, incrementale: 7 passi, `threading_posix.cpp` ricompilato; manifest
`switch-u9.json`): NRO SHA-256 `0012aa01…` (56.416.453 byte, la stessa dimensione di U8-C per allineamento; la riga
`[gta4] semaphores: ...` e la chiave sono nella NRO). Pacchetto `out/console-runs/u9` (fuori da git), con la cache U8c (va
tenuta quella della SD, che la contiene) e due toml:
- **G0:** i default della NRO, con i campioni dello stack come nel run B;
- **G1:** G0 con le 51 ottimizzazioni solo deko3d spente.

`README-U9.txt` dice cosa portare indietro, anche `deko3d_variants.txt` per la cache successiva.

### 2.16 U10: G0 e G1 letti; copie macchina, nativo del mix XAudio, layout caldo, cache con le varianti di G0; prova A (2026-11-14)

**Stato: baseline tecnica verificata, NON una build giocabile.** Sulla console la prova U10-A non ha portato un guadagno
misurabile di fps e l'audio resta robotico; il lavoro resta come punto di partenza riproducibile (NRO `d24db83b…`,
pacchetto `out/console-runs/u10`, fuori da git).

**G0 e G1 (U9).** Dati in `out/console-runs/u9/COPY_TO_SD/switch/theft4-ultimate/G0` e `G1` (fuori da git). G1 (51
ottimizzazioni solo deko3d spente): nessun cambiamento della luce (giudizio dell'utente), prestazioni peggiori.

**I semafori su indirizzo (U9) sulla console, G0 contro B, L0 e C2 (stessa stanza dal salvataggio, poi la strada):**

| Tratto | B (U8) | L0 (U8) | C2 (U8-C) | G0 (U9) |
|---|---|---|---|---|
| stanza, fps per 10 s | 13,9-14,8 | 13,0-13,3 | 11,4-13,3 | 14-18 durante le 145 compilazioni (60-160 s), **25,6-26,5** dopo (210-340 s) |
| strada, fps per 10 s | 5,7-7,9 | 5,4-8,5 | 4,7-9,1 | 8,8-11,9 (mediana ~10,7; esclusi il caricamento a 394 s e 485-505 s) |

Il thread principale aspetta nel kernel il 55 % dei campioni (B: 71 %); l'attesa sul mutex di
`PosixSemaphore::Release` (44,6 % delle sue attese in B) **non compare più**. Durante le 145 compilazioni di G0 (varianti
con 901 = 65520, 903 = 6/8, 926, 950, 977) il thread di prebuild/uam occupa fino all'89 % del core 2 e i draw usano le
varianti generiche (GPU della stanza 55-72 ms a frame contro 37 dopo).

**Correzioni U10 (CPU, dalla pipeline della base, `docs/hand-and-machine-optimising-guide.md`):**
1. **Copie macchina in place** (`localize_native.py`): `localized.txt` = `8216DBB0` (il mix XAudio, col nativo);
   `localized_bulk.txt` dai profili console G0 e U8-B (`switch_hot_bulk.py`, almeno 12 campioni, `--min-instructions 40
   --small-keep-samples 50`): **296 copie bulk + 1 = 297**, le 7 funzioni DSP del mixer incluse. Lo strumento ne rifiuta
   3 (`sub_82775180`, `sub_8277D778`, `sub_8277B0A0`: una riga di switch che non conosce).
2. **Nativo del mix XAudio** (`hand.txt`: `8216DBB0 id=rex_XAudioMixFrameBufferChannel`). **Correzione al nativo della
   base:** era di Banjo e faceva `vmaddfp` come moltiplicazione + somma; il codice generato di questa build lo fa fuso
   (`REX_VMADDFP` con `SASRT_VMX_FUSED_MADD`, una sola arrotondatura come lo Xenon): ora usa `vfmaq_f32` sotto
   `REX_VMX_FUSED_MADD` (il PC, senza fusione, resta moltiplicazione + somma come il suo codice generato). Il nativo non
   riscrive registri: le due chiamate (entrambe in `sub_8216D7E0`, da `__imp__`) scrivono ogni registro volatile prima di
   leggerlo.
3. **Layout caldo** (`hot_layout.py`): `app/config/hot_functions.txt` = 1.600 funzioni campionate in G0 e B.
4. **Cache DKSH U10** `deko3d_shaders-u10.bin` (manifest `dksh-u10.json`): U8c + lista, log e varianti di G0: **9.174
   record** (1.158 compilati, 0 falliti).
5. `tools/ultimate/gta4_dksh_cache.py --glsl --package`: la cache per un altro insieme di testi GLSL (prova della luce
   senza passate, non ancora eseguita).

**Verifica sul PC (guardia densa, `--banjo_hot_guard_calls=4000 --banjo_hot_guard_period=64`, 240 s nella stanza):**
- run 1: **4 differenze**, tutte nel segno o nel payload di un NaN (lane di `0 x inf` o `rsqrte(0)`: copia `7fff0000` /
  originale `ffff0000`, copia `ffc00000` / originale `7fc00001`, un byte `7f` / `ff`): il compilatore commuta gli operandi
  float in modo diverso quando i registri sono locali, e quale NaN esce dipende dall'ordine. **Escluse** con le copie che
  le portavano come membri inline;
- esclusioni per copertura: 4 copie mai eseguite sul PC (`banjo_hot_count`) e `sub_828B06C8` (scritture prima della
  fermata diverse, non dimostrate concorrenti), con `sub_82259B30` che portava una delle quattro. **11 funzioni in tutto
  restano codice generato** (motivo per ciascuna nei commenti di `localized_bulk.txt`): `824A5688`, `8290EC90`,
  `821AAFD8`, `82236FB0`, `823EC040`, `8249B618`, `82743880`, `828E1F68`, `82257D48`, `828B06C8`, `82259B30`;
- run 3 (insieme finale): **0 differenze su 1.389.924 chiamate confrontate**, 274 agganci raggiunti; le 667 chiamate
  fermate con scritture precedenti diverse sono tutte in `sub_82221530`, che incrementa senza atomicità un contatore
  globale (`0x82BD1A10`) eseguito da tre worker: scrittura concorrente dimostrata, non una differenza della copia;
- copertura (run di conteggio): **251** copie confrontate dal proprio aggancio, **45** solo come membri inline di copie
  confrontate (il loro blocco d'ingresso eseguito), 1 (`8216DBB0`) servita sempre dal nativo; il ramo Switch del nativo
  (fuso) non è eseguito da nessun test (corretto per costruzione: la stessa istruzione della macro). Codice usato solo in
  strada: non raggiunto sul PC.

**PCH e timestamp.** `keep_unchanged_times()` (`post_codegen.py`) dà a un file di contenuto invariato la sua data
vecchia, alzata almeno alla data di `rexglue.exe` (`renut_pch.h` è un byproduct del passo del generatore, che ninja
rilancerebbe con un output più vecchio di un input). Ogni build PC ricollega `rexglue.exe`: `renut_pch.h` identico
riceveva una data nuova, clang rifiutava il PCH ("mtime changed") e ninja, vedendo l'header più vecchio del `.pch`, non
lo rifaceva. Per U10: contenuto verificato (SHA-256 `34153BE1…`), data riportata a quella registrata nel PCH, coerenza
degli oggetti controllata (ogni sorgente cambiata ricompilata, ogni oggetto più recente della sorgente e del PCH, hash
dei 930 file generati invariati fino alla fine della guardia). Correzione alla radice preparata e provata (un caso
minimo), non ancora applicata: `-fpch-validate-input-files-content` per clang (stesso contenuto con data nuova accettato,
contenuto cambiato rifiutato). Su GCC (Switch) la data solo in avanti rifà il `.gch`: corretto, a costo di una
ricompilazione completa (quella di U10).

**Build e pacchetto U10.** NRO everyday `d24db83b373aaa6b2f06607d65d7b9400bb08b3e4323f00c9700f9e85c6b93f1` (59.132.101
byte, stampo `075f2ecd4142-dirty`: costruita prima di questo commit, lo stesso codice), 297 copie in place (594 record
`g_in_place`), nessuna delle 11 escluse; manifest `switch-u10.json`; pacchetto `out/console-runs/u10` con la cache U10 e
`gta4-A.toml` (= G0).

**Prova U10-A sulla console (Erista, clock standard).** Dati in `out/console-runs/u10/COPY_TO_SD/switch/theft4-ultimate/A`.
- **"La stanza scende da 25 a 8-9 fps" è l'uscita in strada**: i draw per frame passano da 691 (60 s) a 5.705 (100 s) e le
  catture mostrano Niko fuori a 76 s. Nella stanza ~25 fps come G0 a cache calda.
- **Strada A contro G0:** mediana ~10,4 contro ~10,7 fps, render thread 35-70 contro 41-60 ms a frame, GPU 33-45 contro
  31-40 ms: **nessuna regressione, nessun guadagno**. CPU 336 % su 400 (i tre core del gioco saturi): mixer 79 %, render
  thread 43 %, thread principale 39 %, invio del render 34 %, un thread del gioco 25 %, decoder XMA 23 %. Il frame
  (~95 ms) supera il render thread (45) e la GPU (36): conta il CPU totale.
- **Shader:** 12 compilazioni uam in tutto il run (G0 145), nessun draw in attesa.
- **Audio: 65,8 % del tempo reale (G0 66,6 %).** Costo del mixer per blocco (CPU del thread / blocchi mixati): 5,73 ms in A
  contro 6,02 in G0 (-5 %, scene diverse); il blocco dura 5,33 ms. Il mixer lavora il 79 % e aspetta il suo evento di
  lavoro il 18-28 % del tempo mentre manca il 34 % dei frame: si perde capacità nel modo in cui viene chiamato, non solo
  nel calcolo (sotto).

**Il percorso delle richieste audio (letto nel codice, U10).** Il thread d'uscita dell'SDK (`switch_audio_system.cpp`)
rilascia un'unità del semaforo del client ogni 5,33 ms (più una quando la coda d'uscita scende sotto 2, cioè quando il
gioco è in ritardo); il semaforo parte con 8 unità e ha massimo 8 (`audio_maxqframes`). L'"Audio Worker" chiama la
callback del gioco (`82172BE8` -> `sub_82169B00`) una volta per unità; la callback segnala l'evento del mixer
(`KeSetEvent`) e controlla la fine del lavoro con timeout 0, senza aspettare; il thread del mixer (`sub_82169400`) aspetta
l'evento, fa una passata del grafo XAudio e consegna il frame alla fine (`sub_8216DEA0` ->
`XAudioSubmitRenderDriverFrame`). Più richieste che arrivano durante una passata si fondono in un solo segnale
(ipotesi coerente coi numeri, non ancora dimostrata sulla console). Prossimo passo: A/B col ritmo delle richieste legato
alle consegne (U11).

**Luce: ristretta alla luce del sole e ambientale.** Il PC dallo stesso salvataggio (B) è di giorno, e di giorno è anche
la console (U8-B, strada col venditore di hot dog: cielo chiaro). Sulla Switch sono giusti le luci locali (lampade,
lampioni), le superfici emissive (insegne) e il cielo; manca la luce direzionale del sole e ambientale su tutto il resto.
In un renderer differito è il passaggio a schermo intero del sole, che legge la shadow map (il resolve di depth
1280x1280 `DA1A78E0`). Esclusi finora: ogni chiave a run time (L0-L5, G1), la regola di Halo 3 (C2), la rampa gamma (la
stessa), le liste di shader della base. Prossima prova (U11): la shadow map e il G-buffer a schermo con la diagnostica
della base (`banjo_renut_diag_show_texture`), PC contro console.

**Test sul PC con movimento.** `input_script` dell'SDK muove Niko (pressioni e levette temporizzate, verificato: porta,
corridoio, pianerottolo); sessione manuale dell'utente con la tastiera (`--mnk_mode=true`) e catture ogni 5 s: dalla
stanza alla strada sotto la sopraelevata, di giorno.

### 2.17 U11: la priorità dei thread XAudio (audio), il registratore degli ingressi, la luce ristretta al post-processing (2026-11-14)

**Prove sulla console (Erista, clock standard; NRO `59bb662a…`, stampo `0aff3265ecae-dirty` = il codice di U10 più le due
chiavi sotto, spente di default; pacchetto `out/console-runs/u11`).** Sei run in
`out/console-runs/u11/COPY_TO_SD/switch/theft4-ultimate`: X0, X1 (audio), `hdr`, `shadow`, `gb0`, `gb1` (viste dei buffer).
Ogni log conferma build e chiavi (X0 e viste: XAudio a -1; X1: "priority 0x2D ... set"); 0-1 compilazioni uam e nessun
draw in attesa in tutte.

**`pipeline_list.bin` mancante in tutte le run tranne X1 (errore di preparazione, annotato).** Il file
(`renut_pipeline_list`, `render/pipeline.cpp`) è l'elenco delle pipeline che il renderer ha costruito: all'avvio i thread
di prebuild (core 2, 0x3B) le costruiscono in anticipo (variante generica, poi la specializzata letta dalla cache DKSH e
scambiata); senza, lo stesso lavoro si fa al primo uso durante il gioco, con la variante generica nel frattempo. Nessun
dato d'immagine: la generica dà la stessa immagine, più lenta sulla GPU. Misurato: a fine run X0 768 stadi generici / 752
letti dalla cache / 1 compilazione, X1 864 / 799 / 0; CPU dei thread del core 2 in strada mediana 0,5 % (X0) contro 0,2 %
(X1). La sua assenza non spiega né l'audio né la luce; le viste (tutte senza lista) restano confrontabili tra loro e col
PC per il contenuto dei buffer.

**Audio: `gta4_xaudio_thread_priority` (X1 = 0x2D) contro X0 (-1), stesso percorso stanza -> strada.**

| | X0 | X1 |
|---|---|---|
| quota audio dai 60 s | 69,1 % | **86,0 %** |
| peggiori 10 s | 57,4 % | **81,1 %** |
| strada (finestre >= 80 s, < 20 fps): quota / CPU del mixer / ms per blocco | 66,0 % / 81,8 % / 5,70 | **84,6 %** / 104,8 % / 5,40 |
| fps dai 60 s (mediana) | 10,2 | 9,3 |

Il costo del mixer per blocco non cambia (5,4-5,7 ms per un blocco da 5,33): cambia il tempo di core che ottiene. Il
meccanismo, dal codice del gioco: la libreria XAudio crea i propri thread (`sub_82169578`, ingressi `82169400` e
`821694C8`) e chiede `KeSetBasePriorityThread(thread, 15)` (time critical); la base porta ogni thread guest a 0x3B, che
Horizon divide in fette da 10 ms; la callback del driver (`sub_82169B00`) segnala il mixer e **lo aspetta**
(`KeWaitForMultipleObjects` senza timeout), poi consegna il frame. **Miglioramento misurato, non una soluzione**: il 14 %
dei frame manca ancora (il calcolo supera il blocco) e la strada perde ~8 % di fps. Default ancora -1.

**A/B scartato (PC):** chiamare la callback solo dopo la consegna del frame precedente (`audio_pace_by_submit`): l'audio
del PC crolla (300 frame su 1.875 ogni 10 s, ogni attesa scaduta), perché il frame di una richiesta arriva solo con la
callback successiva. Codice tolto.

**Luce: il gioco rende il giorno col sole; il buio nasce dopo l'illuminazione.** Struttura del frame (trace dei resolve
sul PC): G-buffer in 3 bande (`40226620`, `402266B0`, `40226780`, depth `DA1A6320`), luce in `40076EB0` -> `40079D00`,
la texture 1280x1280 `DA1A78A0`, poi la catena di riduzione (640x360 `DA1A63E0`, 320x180, ... 1x1) e la composizione
finale. Le viste della console (`banjo_renut_diag_show_texture`): **`hdr` è piena di luce** (tromba delle scale a 61 s;
in strada a 123 s sole diretto con le ombre nette delle auto), come il riferimento PC; `gb0` (colore delle superfici) e
`gb1` (normali) plausibili; `shadow` simile al PC. L'utente si è mosso in tutte le viste: inquadrature diverse dal PC,
confronto qualitativo. Conclusione: lo stato del gioco è diurno e il passaggio della luce funziona; il buio è nella catena
di post-processing (riduzione, esposizione, bloom, tone mapping, correzione colore) tra `40079D00` e l'immagine finale,
coerente con la luminanza riletta dalla CPU uguale al PC (U9). I fari accesi di alcune auto non bastano a dire notte.

**Strumenti U11.** `input_record` (SDK, `input_system.cpp`): il pad dell'utente 0 scritto come `input_script`
(verificato sul PC: lo script registrato e quello originale coincidono entro 10 ms); sessione manuale PC con tastiera
(`--mnk_mode=true`).

### 2.18 U12: tre copie NaN escluse, la guardia ripete le differenze di prefisso, il prefetch del mix (A/B, tolto), la catena del post-processing uguale al PC (2026-11-15)

**Copie macchina (PC, guardia densa `banjo_hot_guard_calls=4000`, `banjo_hot_guard_period=64`, 120-240 s dal
salvataggio).** Tre run di guardia sul codice di U11 hanno trovato tre copie del gruppo con la stessa classe di
differenza di U10 (§2.16), tre passaggi di fila:
- `828DDFD8`: corsia 0 di v8, copia `fff0eeeb` / originale `7fc00001`;
- `827F4BB0`: corsia 0 di v9, copia `7fffffff` / originale `ffffffff`;
- `828DC908`: la stessa differenza, perché porta `827F4BB0` come membro inline (`n_828DC908.blocks.json`).

Tutte e tre **escluse** (motivo nei commenti di `localized_bulk.txt`); il gruppo scende da 296 a 293 copie.

**La guardia ripete anche le differenze di prefisso** (`banjo_hot_guest.cpp`, `Check`/`Attempt`, `kPrefixDiffer`): una
differenza delle scritture fatte prima di una fermata comune (`banjo_hot_guard_prefix`) si rifà dagli stessi registri fino
a tre volte, come le differenze complete. Run di 240 s con la lista finale: **0 differenze su 4.135.393 chiamate**, 270
copie del gruppo su 293 raggiunte. Prefissi sparsi e non ripetuti in `828AA448` (18), `828BEBE0` (1), `8217B870` (1):
concorrenti, restano copie. `82221530` (253 ripetute su 15.510 uguali) è il contatore globale non atomico già visto in
U10 (`0x82BD1A10`, tre worker): l'originale gira per secondo e vede l'incremento di un altro worker; resta.

**Prove sulla console (Erista, clock standard; NRO `d3642faf…`, stampo `524c9a8f62f8-dirty`; pacchetto
`out/console-runs/u12`).** Tre run, nessun `note.txt`. M0 (prefetch spento) è partita con la `pipeline_list.bin` di U11
(897 pipeline costruite in anticipo); **M1 è partita senza** (sparita dalla scheda tra M0 e M1; ricreata durante la run,
nessun draw in attesa di una compilazione); V-post ha letto quella di M1. Percorsi diversi: M0 è uscita dalla stanza a
~60 s, M1 a ~90 s ed è finita in un filmato. **Gli fps di M0 e M1 non sono confrontabili**; il costo del mixer per blocco
sì.

**Audio: il prefetch del mix non serve (`gta4_hand_xaudio_mix_prefetch`, 70 `prfm` nella copia del router).** Finestre
60-140 s, entrambe con XAudio a 0x2D:

| | M0 (spento) | M1 (acceso) | U11-X1 (senza) |
|---|---|---|---|
| ms del mixer per blocco da 5,33 ms (mediana / max) | 5,42 / 5,74 | 5,32 / 5,80 | 5,39 / 5,61 |
| quota di audio consegnato | 85,8 % | 87,3 % | 87,1 % |
| quota del router `sub_8216D7E0` nel thread del mixer | 27,1 % | 27,1 % | - |

Le differenze stanno dentro la variazione tra una run e l'altra: **nessun effetto misurabile, codice tolto** (la patch
resta in `out/console-runs/u12/u12-uncommitted.patch`). La scrittura di FPCR nel router resta: toglierla richiederebbe
la modalità unica (`BLUR_FPCR_ONE_MODE`), non bit-exact.

**Luce: la catena del post-processing è uguale al PC; la prima anomalia è la scena HDR.** Due strumenti nuovi:
`gta4_diag_ps_constants = "HASH:PRIMO-ULTIMO"` scrive ogni 5 s le costanti float che i draw di quel pixel shader leggono
(`[psconst]`; sul deko3d manda i draw per la via completa: solo nelle run di viste) e `banjo_renut_diag_show_texture`
accetta una lista `HEADER@GAMMA,...` mostrata a turno (`banjo_renut_diag_show_cycle_s`, `[showtex]`). V-post (4 minuti
da fermi nella stanza): le costanti della composizione finale `CC0C2F3146CCC96E` coincidono con il PC (c208 1,92 contro
1,91; c212 (0,946 0,690 0,533) contro (0,946 0,692 0,541); c209, c210, c215 uguali), la luminanza 1x1 è bianca su
entrambi, il frame finale ha la stessa luminosità media (15,5/14,3/12,5 contro 15,9/14,5/8,8). **La scena HDR `40079D00`
no**: nella stanza 148/143/99 sulla console contro 221/212/193 sul PC; sulla console solo le lampade (gialle, a
decadimento radiale), muri, Niko e mobili scuri fuori dai coni. Il difetto nasce prima del post-processing: **la
conclusione di U11 ("il buio è nel post-processing", §2.17) era sbagliata**; le viste U11 erano state prese con l'utente in
movimento e senza un riferimento della stessa inquadratura.

### 2.19 U13: la luce ambientale persa nel ripristino EDRAM della base; lo stencil ripristinato come nel vecchio renderer (2026-11-15)

**Prova sulla console V-light (Erista, clock standard; NRO `3ae9d456…`, stampo `524c9a8f62f8-dirty` = U12 senza prefetch
più la riga `[pstex]`; cache U10 `438346a3…`; pacchetto `out/console-runs/u13`).** Una run, ferma nella stanza da ~46 s
fino all'uscita. Log: build giusta, GPU 460,8 MHz, RAM 1331,2 MHz; 108 righe `[psconst]`, 108 `[pstex]`, 31 `[showtex]`;
80 catture; `pipeline_list.bin` identico a quello di M1 (`0431E1A9…`). Nessun `note.txt`.

**Gli ingressi del passaggio della luce ambientale sono uguali al PC.** Il draw a schermo intero `97AC30BA76D5515F` (VS
`D6F569E53F3F319C`) scrive per primo in `40076EB0`: ambientale = (c37 + c38 · normale) · `40226780`.z, per l'albedo, più
il sole; il byte dello stencil delle parole della depth (`DA1A63A0`.z) sceglie tra due modelli.
- costanti c36-c39: **identiche** al PC in ogni campione (es. c37 (1,55134 1,29404 1,1881), c38 (4,38946 4,54775
  4,50517)); prima dell'aggiornamento entrambi hanno c36 = 3,4e38 e c37 (1,38 1,14 1,04667);
- texture degli slot 0, 1, 2, 4, 5, 6: stessi header, basi, formati, dimensioni, esponenti e segni (`[pstex]`; cambiano
  solo gli indici dei descrittori, propri di ogni back end);
- viste (medie su 255, console contro PC): `40226780` 13,7/65,1/121,5 contro 13,7/65,1/112,9 (canale z compreso);
  `40226620` 100,2/96,3/84,8 contro 100,7/96,7/85,6; `402266B0` 90,4/192,2/136,0 contro 89,6/192,3/135,8;
  `DA1A63A0` nera su entrambi (la vista non sa mostrare le parole della depth: non è un confronto);
- shader: il GLSL eseguito (`ultimate/out/deko3d_shaders/glsl_opt`) e quello del vecchio renderer funzionante (run 70,
  `out/renut-switch/deko3d/glsl_opt`) fanno lo stesso calcolo; le costanti di specializzazione della variante della
  console (`deko3d_variants.txt`: conversioni dei fetch note e nulle, `926` fetch semplici, `990` clamp 7e3, `909` early
  Z) non lo cambiano.

Il passaggio quindi, quando gira, dà lo stesso risultato: sulla console **non scrive**.

**La causa: il test di stencil del passaggio e il ripristino EDRAM della base.** Il drawlog (ora con il target di depth
e ref/maschere dello stencil) del frame della stanza:

| draw | stato | stencil |
|---|---|---|
| G-buffer (380-480) | depth `402265E0` | ALWAYS, REPLACE, ref 0x08 (la maggior parte), 0x89, 0x06 |
| 500, PS `B3D105989610E354` | colore `40226810`, nessuna depth | - (copia le parole della depth come colore) |
| 501-502 | depth `400007C0`, ripristinata da `40226810` | 502 (ambientale): **EQUAL ref 0x01, maschera 0x01** |
| 503, 505, ... (volumi) | depth `400007C0` | ALWAYS, INCW/DECW sulle facce |
| 504, 506, ... (luci) | depth `400007C0` | **NOTEQUAL ref 0xFF**, REPLACE |

Il copy shader del gioco (draw 500) riscrive le parole della depth come colore sopra l'EDRAM della depth della luce e
mette nel **byte basso** (lo stencil) **0xFF dove la depth c'è e 0x80 dove è nulla**. Sulla Xbox 360 l'EDRAM è una sola:
la depth ripristinata porta quello stencil, 0xFF è il valore neutro della luce. Quindi l'ambientale (EQUAL 1/1) passa
sulla geometria e non sul cielo; ogni luce passa solo dove i suoi volumi hanno cambiato lo stencil e lo rimette a 0xFF.
Il ripristino della base (`RestoreDepthFromColorLocked`, porta 29 di sasrt-nx, `sasrt_renut_depth_restore`) prende la
depth dalla parola ma lascia lo **stencil a 0** ("a game's restore copies a depth whose stencil it cleared", vero per
Sonic). Su deko3d, che usa il ref vero, ne segue:
- l'ambientale (EQUAL 1 su 0) **fallisce ovunque**: muri, Niko e mobili senza luce (U10-U13);
- le luci (NOTEQUAL 0xFF su 0) **passano su tutta la loro sagoma**: le "luci locali più forti del PC" annotate in U10 e i
  coni gialli di V-post;
- in strada il sole diretto arriva da altri passaggi, quindi resta: "ombre nere" (U11).

**Perché il PC era illuminato: due errori che si compensavano.** plume D3D12 non assegnava mai
`D3D12GraphicsPipeline::stencilRef` da `desc.stencilReference` (`OMSetStencilRef` sempre con 0, anche nel vecchio
albero): sul PC ogni test di stencil confrontava con 0, e l'ambientale (EQUAL 0, maschera 1) passava proprio sullo stencil
0 del ripristino. Il deko3d imposta il ref della pipeline (`dkCmdBufSetStencil`). Il PC D3D12 non era un riferimento valido
per lo stencil.

**Il confronto con il vecchio renderer.** Il commit `6921903` ("GTA IV's lighting depth and stencil restored from
colour") ripristinava la depth **e lo stencil bit per bit** (un passaggio per bit: scarto dove il bit è 0, scrittura di
quel bit), perché un pixel shader non può scrivere lo stencil su ogni back end (deko3d); il run 70 (`cc64728`) aveva la
stanza giusta sulla console. In U5b (`db37f00`, §2.9) il ripristino della base era stato giudicato equivalente ("ripristino
della depth coperto") e se ne era riportato solo lo scambio dei mezzi tile (`00683a5`): **lo stencil è la regressione della
migrazione**. G1 (§2.16) non poteva trovarla: il ripristino non è un'ottimizzazione solo deko3d con una chiave.

**Correzione (PC verificato, console da provare).**
- `gta4_renut_depth_restore_stencil` (**accesa di default**): il passaggio della depth scrive anche lo stencil 0 (ref 0,
  maschera 0xFF, REPLACE), poi 8 passaggi (`depth_restore_ps.hlsl`, `param1` da -1 a -8) scrivono ciascuno un bit del byte
  basso della parola (ref 0xFF, maschera di scrittura quel bit, scarto dove il bit è 0); Zcull invalidato dopo tutti i
  passaggi (deko3d).
- plume D3D12: `stencilRef = desc.stencilReference` (`plume_d3d12.cpp`): il PC fa ora i test di stencil come la
  console, ed è di nuovo un riferimento.
- drawlog: target di depth (`depth 33 at 400007C0`) e `sref` (ref, maschera di lettura, di scrittura; fronte/retro).

Verifica sul PC (D3D12, stesso salvataggio, 100 s, medie su 255; HDR = vista `40079D00`):

| run | ref D3D12 | stencil del ripristino | HDR | frame finale |
|---|---|---|---|---|
| A (prima) | 0 (difetto) | 0 | 224,5 / 218,1 / 197,7 | 15,6 / 14,3 / 12,5 |
| A2, A3 | giusto | 0 (la base) | **8,4 / 7,1 / 6,1** | **0,4 / 0,4 / 0,4** |
| B2, B3 | giusto | dai bit della parola | 224,5 / 218,0 / 197,7 | 15,6 / 14,3 / 12,6 |
| B4 | giusto | zero col draw + bit | 224,5 / 218,1 / 198,0 | 15,6 / 14,3 / 12,6 |
| D (default) | giusto | chiave accesa di default | non misurata (run senza viste) | 15,6 / 14,3 / 12,7 |

Con il ref corretto il PC **riproduce il difetto della console** (A2/A3; più buio della console, dove le lampade si
vedono: differenza non indagata, sparisce con la correzione) e la correzione lo toglie (B, D), con lo stesso frame del riferimento
precedente entro 0,7/255. Nessun errore di plume. **Costo:** 9 passaggi a schermo intero per ripristino (uno nel frame
della stanza) contro 1; il vecchio renderer li aveva. In compenso le luci tornano mascherate dai loro volumi (oggi sulla
console coprono tutta la sagoma): il bilancio GPU si misura sulla console.

**Prestazioni e audio dalla V-light (ferma nella stanza, 0x2D):** 28-30 fps; mixer 5,43-5,49 ms per blocco (mediana),
~100 % di un core, audio 83-86,5 % (V-post 89 %). Il thread del mixer, per funzione: router `sub_8216D7E0` 28,3 % (con il
nativo del mix inline), `sub_828C2D28` 16,8 %, `sub_828C6740` 13,9 %, `sub_828C5A58` 11,0 %, `sub_82185E38` 7,6 %,
`sub_828C7848` 6,6 % (una copia in place: il profilo la nomina `sub_828C7848`; U16), `sub_82178398` 4,3 %. Render thread ~37 % di un core (`DrawStateLocked` 25 %), thread
principale ~55 %, worker `XThreadFFED0F60` ~27 %, decoder XMA ~26 %.

**Prossima prova (una sola, da autorizzare):** pacchetto `u14`, la stessa run ferma nella stanza con la correzione accesa
(nessuna chiave diagnostica che cambi il percorso dei draw), catture ogni 5 s. Esito atteso: la stanza come il PC B; fps
e tempo GPU contro V-light.

### 2.20 U14: la luce ambientale torna sulla console; il suo costo GPU; i glitch rimasti (2026-11-15)

**Prova sulla console (Erista, clock standard; NRO `59481f7d…`, stampo `524c9a8f62f8-dirty` = U13 più la correzione
dello stencil di §2.19, `gta4_renut_depth_restore_stencil` accesa di default; cache `438346a3…`; toml = V-light senza le
sue chiavi diagnostiche; pacchetto `out/console-runs/u14`).** Una run (`.../theft4-ultimate/U14`), nessun `note.txt`.
Log: build giusta, restauri EDRAM disegnati, 4 compilazioni uam in background, nessun draw in attesa. La run è partita in
un altro punto dell'appartamento (la cucina con la lampada), non nella stanza di U13, e cambia scena durante la run (finestre
da 650, 930 e 3.700-4.100 draw a frame).

**Luce: corretta.** L'utente conferma la luce diurna ripristinata; le catture mostrano muri, pavimento, mobili e Niko
illuminati dall'ambientale, come il PC (§2.19). **Restano due glitch** (non confusi con le prestazioni):
- **alone della lampada della cucina**: un disco bianco uniforme dal bordo netto attorno alla lampadina, che in alcune
  catture manca. Ipotesi escluse: le query di occlusione di D3D (la costante sentinella 0xFFFFFEED di `D3DQuery_Issue`
  non c'è nel codice di GTA IV: il gioco non le usa, quindi la risposta della base, port 76, non c'entra). Ipotesi aperte,
  da distinguere con un riferimento PC della stessa cucina (serve il salvataggio dell'utente): la visibilità della corona
  calcolata dallo shader sulla depth risolta (`40079D50`, la depth ripristinata) e la sfumatura del disco;
- **ombre**: segnalate dall'utente, da localizzare nelle catture con un riferimento PC.

**Prestazioni: limitate dalla GPU.** Nella prima finestra con 650 draw a frame (60-70 s), contro la V-light di U13 (stessa
NRO meno la correzione, ~640 draw):

| | U13 V-light | U14 |
|---|---|---|
| fps | ~26 | 16 |
| GPU per frame | 35,5 ms | **61,5 ms** |
| render thread per frame | 15 ms | 13,7 ms |

Il calo è tutto GPU. Sul PC (D3D12, `renut_gpu_profile` + `banjo_renut_diag_gpu_shaders`, stesso salvataggio) la
correzione costa 11,42 -> 13,57 ms (+19 %): l'etichetta del copy shader `B3D10598` (che contiene i 9 passaggi del
ripristino) 0,64 -> 1,63 ms, il passaggio ambientale ora disegnato 0,35 ms, le luci 2,19 -> 2,64 ms. Sulla console +73 %:
la parte in più, specifica del deko3d, si misura con il profilo GPU per passaggio sulla console (U15-P). **Audio:** 91,4 %
consegnato, mixer 5,25 ms per blocco (finestre 60-240 s; U13 86,5 %, 5,43): coerente con gli fps più bassi, che lasciano
più core al mixer; nessun cambio del codice audio, nessun merito attribuito.

### 2.21 U15: i port 129-141 della base nel nostro albero; la prova console preparata (2026-11-15)

**La base è andata avanti.** `ChanseyIsTheBest/renut_nx_ultimate_base` è passata da `8de5d51a` (U1) a `3e6ae026` (14
novembre): quattro commit, port 129-141 (da sasrt-nx e blur-nx). Presi con lo strumento della base,
`tools/ult_port_sync.py` (merge a tre vie per file di ogni commit di port: il cambiamento del port entra, il nostro resta):
66 file puliti, 4 nuovi, **1 conflitto** (`banjo_hot_guest.cpp`, la riga di riepilogo della guardia: la nostra versione
U12 e il conteggio del port 141, unite) e **3 file a mano** (quelli che il nostro albero tiene nella forma di GTA IV):
- `d3d.h`: `kDeviceOffsetViewport = 0x3160`, l'offset del viewport che i nostri clear e le letture del viewport usano già
  per GTA IV (`d3d_native_state.cpp`, `hooks/draw.cpp`, `hooks/state.cpp`);
- `hooks/resource.cpp`: `ForgetInPlaceShader` anche nel nostro hook di `D3DResource_Destroy` (`sub_829D8D40`);
- `hooks/shader.cpp`: la mappa degli shader in place (`InPlaceShaderEntry`, `NoteInPlaceShader`).

| port | cosa | esatto | in U15 |
|---|---|---|---|
| 129 | deko3d: la decompressione differita della depth tiene lo stato della lista (un draw poteva restare senza target) | sì | sì |
| 130 | una destinazione di resolve rifatta tiene la sonda delle parole della depth | sì | sì |
| 131, 132 | diagnostiche dei resolve; `ult_port_sync.py` | - | sì (spente) |
| 133 | CG-13, una sola modalità FPCR (`BLUR_FPCR_ONE_MODE`) | **no** (denormali scalari) | **spento** anche sulla Switch (`app/CMakeLists.txt`) |
| 134 | cache XMA v2, un frame di recupero, apertura pigra del codec | sì | sì |
| 135 | pompa audio a coppie (spenta), configurazione degli altoparlanti, censimento | sì | sì (spenti) |
| 136 | passaggi di codegen (float loads a ogni modo, fmadds noti) | sì | **no**: serve una rigenerazione (U16) |
| 137 | memo delle estensioni delle texture a due vie | sì | sì |
| 138 | catture dei Begin*Vertices ristrette, resolve e clear in coda con il loro stato | sì | sì |
| 139 | ricerche degli handle col lock della tabella, non quello globale | sì | sì |
| 140 | salto delle scritture FPSCR uguali; store card per le viste (`BLUR_CARD_VIEWS_ONLY`); interruttori quasi esatti (spenti) | sì / no (spenti) | sì, tranne i quasi esatti |
| 141 | la guardia prende un NaN diverso solo nel segno come uguale | sì | sì |

`sdk/thirdparty` non è nel repository: il port 134 cambia anche `FFmpeg/libavcodec/wmaprodec.c` (copiato nel pacchetto,
`u15-new-files/`; va aggiunto a `tools/ultimate/fetch_sdk_thirdparty.ps1` prima del commit dei port).

**Verifica sul PC (D3D12, stesso salvataggio, build completa: le intestazioni SDK cambiano tutto il codice generato).**
- Immagine: uguale a U14 (differenza media 0,4-1,9 su 255 sulle stesse catture).
- **Cache XMA**: frame serviti dalla cache 6,5 % (v1, 50.977 decodificati in 100 s) -> **92,1 %** (4.071 decodificati);
  `banjo_xma_frame_cache_chain_verify`: 53.824 frame confrontati con la catena completa, **0 diversi**.
- Guardia densa delle copie (`banjo_hot_guard_calls=4000`, periodo 64, 200 s): 271 copie raggiunte, **0 differenze su
  3.554.875 chiamate**, 0 NaN di solo segno; controlli del codice generato 0 differenze.
- Render thread 3,82 -> 3,69 ms a frame (PC, indicativo).

**Prova console preparata (non eseguita):** NRO `063dcbb7…`, pacchetto `out/console-runs/u15`. Due run brevi dalla stessa
posizione di U14 (la cucina): **A** = il toml di U14 invariato (il confronto: fps, GPU, render thread, mixer, audio, CPU del
decoder XMA); **P** = A più il profilo GPU per passaggio e per pixel shader (`renut_gpu_profile = 10`,
`banjo_renut_diag_gpu_shaders`): separa il costo del ripristino dello stencil, dell'ambientale, delle luci e delle ombre
sul deko3d (il +73 % di U14 contro il +19 % del PC).

**Risultato sulla console (U15-A e U15-P, Erista, clock standard; NRO `063dcbb7…`).** Nessun `note.txt`; osservazioni
dell'utente: prestazioni come U14, audio ancora robotico (forse un po' meglio), ombre che lampeggiano leggermente, l'alone
della lampada sempre presente e a tratti spento, un triangolo luminoso più evidente. Le due run hanno di nuovo cambiato
scena (stanza, cucina, strada, guida, un filmato nell'ufficio dei taxi): si confrontano le finestre con gli stessi draw a
frame.

| finestra (draw a frame) | U14: fps / render thread / GPU | U15-A: fps / render thread / GPU |
|---|---|---|
| cucina (~650) | 16,0 / 13,7 / 61,5 ms | 16,1 / 13,4 / 61,2 ms |
| interno (~950) | 19,6-20,8 / 16,2-20,0 / 47-50 ms | 19,5-20,7 / 16,4-19,4 / 48-50 ms |
| strada (3.200-4.700) | 8,3-9,3 / 45-50 / 48-59 ms | 7,7-10,3 / 46-62 / 38-65 ms |

- **Prestazioni: nessuna differenza misurabile**, come percepito.
- **CPU: il thread del decoder XMA scende da ~30 % a ~10 % di un core** in ogni finestra confrontabile (cache XMA v2:
  94,2 % dei frame serviti, 2.639 decodificati contro 43.180 di U14). Il core liberato non cambia gli fps: in cucina il
  limite è la GPU, in strada il render thread e il thread principale.
- Audio: 89,8 % consegnato dai 60 s (U14 89,9 %), mixer 5,56 ms per blocco nell'interno (U14 5,56): **invariato**; il
  salto delle scritture FPSCR (port 140) non si vede nel costo per blocco.
- **U15-P, il profilo GPU per passaggio (la prima misura per passaggio sulla console):** in cucina 60,9 ms a frame, di cui
  **25,6 ms nell'etichetta del copy shader `B3D105989610E354`**, che contiene i nove passaggi del ripristino dello stencil
  (due ripristini a frame: ~12,8 ms l'uno; nell'interno, uno: 12,95 ms; sul PC 1,6 ms). Poi i resolve 6,1 ms, i volumi
  delle luci senza pixel shader 3,8, le luci `23C88DA6` 2,9, l'ambientale `97AC30BA` 2,3, i resolve della depth 2,3, la
  composizione `CC0C2F31` 2,2 ms. **Il +26 ms di U14 è il ripristino dello stencil a nove passaggi.**


### 2.22 U16: il ripristino dello stencil in due passaggi, la rigenerazione con il codegen della base, la risoluzione dinamica (2026-11-15; PC, console da autorizzare)

**GPU: lo stencil del ripristino in due passaggi (`gta4_renut_depth_restore_stencil_fast`, accesa).** U15-P ha messo i nove
passaggi a schermo intero del ripristino a ~12,8 ms l'uno (due a frame in cucina). Il copy shader del gioco
`B3D105989610E354` (draw 500 e 621) scrive nel byte basso della parola `(src.w, src.y, src.x tutti 0) ? 0.5 : 1.0`, cioè
0x80 dove i tre byte di depth della parola sono 0 e 0xFF altrove: lo stencil è una funzione della depth ripristinata.
- `NoteGta4DepthWordsCopy` (percorso completo dei draw) segna sulla superficie di colore la scrittura di quella copia solo
  se niente ne cambia l'uscita: un target, i quattro canali, nessun blend (ONE/ZERO), test alfa sempre, nessun blend skip,
  nessun trim, gamma o bias del target, nessun alpha to coverage, scissor su tutta la superficie, formato RGBA8; ogni altra
  scrittura successiva (draw, clear, eredità EDRAM) cambia `write_seq` e il segno non vale più.
- Quando la sorgente del ripristino è quella copia: il passaggio della depth scrive lo stencil 0xFF; Zcull invalidato
  (deko3d: non vede la depth scritta dallo shader); un passaggio senza pixel shader con il test di depth EQUAL a z 0
  (`present_vs`, depth da 0 a 1 su deko3d e D3D12; la depth ripristinata di una parola 0 è esattamente 0,0) scrive 0x80.
  Lo stesso stencil dei passaggi a bit, due passaggi invece di nove, il secondo scartato in anticipo dalla depth. Altrimenti
  i passaggi a bit, come in U14.

PC (D3D12, stesso salvataggio, profilo GPU per passaggio): 8.001 ripristini su 8.001 nella forma veloce; l'etichetta della
copia + ripristino 1,64 -> **0,83 ms** (la copia da sola 0,64), GPU a frame 13,59 -> 12,29 ms; immagini uguali alla forma a
bit entro il rumore tra due run (differenza media ~1/255, la stessa che separa due run identiche). Atteso sulla console: il
ripristino da ~12,8 ms a pochi ms, cioè ~20 ms in meno a frame in cucina (GPU 61 -> ~40 ms, se resta il limite).

**CPU: la rigenerazione con i passaggi di codegen dei port 136 e 140** (`post_codegen.py` sull'albero generato di U15, che
resta in `ultimate/out/generated-u15-backup`): 314 file su 628 cambiati; `float_loads` 19.798 letture float riscritte e,
nella forma a ogni modo (M-23), 11.410 letture e 8.346 scritture; `near_exact.py` nella sua forma esatta (VMaxFp/VMinFp);
store card per le viste (`BLUR_CARD_VIEWS_ONLY`, già acceso). Verifica PC: guardia densa delle copie **0 differenze su
3.126.347 chiamate** (270 copie raggiunte), controlli del codice generato 0, flusso della modalità flush 0 modifiche,
inoltri dello stack 0, auto-verifica delle forme 44.525/44.525; immagine uguale. Il guadagno è delle forme ARM: si misura
solo sulla console. (Correzione: `sub_828C7848` è già una copia in place; il profilo la nomina come la funzione generata.)

**Risoluzione dinamica: quella della base** (`banjo_renut_dynamic_resolution`, `render/surfaces.cpp`
`NoteGpuFrameForDynamicResolution`; sasrt-nx, vivapinata-nx): l'immagine 3D tra un'altezza bassa e alta per modalità su una
griglia (36 righe: 576 612 648 684 tra 540 e 720), la media mobile del tempo GPU (o del lato CPU quando più lungo:
`sasrt_dynamic_cpu_aware`), giù dopo 5 s sopra il budget al passo previsto, su di un passo dopo `up_s` s con 1 ms di margine
e la memoria per i nuovi target; HUD e uscita a 1280x720 (scala bilineare, bicubica con `banjo_renut_upscale_filter 1`);
ogni cambio nel log. Sul PC agisce solo il deko3d; GTA IV a una scala diversa da 1x è verificato sul PC con
`renut_resolution 480p` (stanza illuminata come a 720p: ripristino EDRAM, mezzi tile, copia in pixel guest reggono).

**Glitch (diagnosi, nessuna correzione):**
- il "triangolo luminoso" è il fascio della lampada della cucina: un cuneo bianco dal bordo netto che parte dalla lampadina
  (catture U15-A); con l'alone (un disco bianco uniforme) condivide i bordi netti dove il gioco sfuma. Ipotesi aperta comune:
  la sfumatura di questi effetti (particelle "soffici") legge la depth risolta della luce `40079D50` (resolve della depth
  ripristinata `400007C0`); esclusa: le query di occlusione (§2.20). Serve il salvataggio dell'utente per la cucina sul PC;
- ombre che lampeggiano: da localizzare con lo stesso salvataggio (le ombre del sole in strada sono uguali al PC, §2.20).

**Prova U16 proposta (non costruita):** un pacchetto, tre run brevi dalla stessa posizione:
**A** (720p fissi, il toml di U15-A: il codice U16 contro U15-A), **D** (A + risoluzione dinamica 540-720, budget 33,3 ms,
su dopo 2 s: contro A), **P** (A + profilo GPU per passaggio: il costo del nuovo ripristino); più una copia del salvataggio
per il PC.

### 2.23 U16 letto sulla console; U17: il collo di bottiglia CPU, i vetri intermittenti, il ripristino veloce mai attivo (2026-11-16; PC, console da autorizzare)

Materiale: `out/console-runs/u16/COPY_TO_SD/switch/theft4-ultimate/{A,D,P}` (NRO `4424ac8a…`, build
`6be295853eed-dirty`, clock standard 1020/460,8/1331,2 MHz in tutti i blocchi del profilo), salvataggio
`out/console-runs/u16/save-copy/renut`. Nessuna nota scritta del run. Il percorso dei tre run è lo stesso e si legge dalle
catture: corridoio e cucina (47-89 s), strada a piedi e in auto (90-180 s), filmato nell'ufficio del taxi (190-245 s),
garage del taxi (250-300 s). Il log del renderer e il profilo hanno orologi diversi: le finestre sono scelte a mano su
ciascuno (catture, draw a frame). Strumenti nella cartella di lavoro: `scenes.py` (tabella sotto), `prof.py` (tempo per
funzione per thread e finestra, simbolizzato con `nm` dell'ELF U16), `cores.py` (CPU per maschera dei core), `cats.py`,
`regions.py`/`diffimg.py` (catture).

**Per scena (mediane; U16-P solo per il profilo GPU).** CPU dei thread in % di un core.

| Run | Scena | fps | render thread ms | GPU ms | draw/frame | CPU tot % | audio % | mixer | main | render guest | render ReNut | audio 2 | XMA |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| U15-A | cucina (1 finestra) | 16,1 | 13,4 | 61,2 | 625 | 266 | 99,8 | 107 | 35 | 21 | 28 | 31 | 8 |
| U15-A | strada | 9,0 | 61,2 | 56,7 | 4.351 | 354 | 90,7 | 109 | 37 | 50 | 63 | 15 | 11 |
| U15-A | filmato | 20,3 | 18,6 | 48,4 | 976 | 276 | 92,0 | 104 | 37 | 28 | 40 | 23 | 10 |
| U15-A | garage | 9,3 | 53,0 | 57,5 | 3.767 | 348 | 85,2 | 108 | 40 | 42 | 57 | 16 | 11 |
| U16-A | cucina | 16,5 | 13,2 | 59,5 | 650 | 246 | 100,0 | 94 | 35 | 19 | 24 | 36 | 8 |
| U16-A | strada | 9,3 | 56,0 | 56,8 | 4.295 | 359 | 95,0 | 108 | 38 | 50 | 62 | 18 | 13 |
| U16-A | filmato | 20,0 | 17,6 | 49,0 | 922 | 272 | 98,8 | 104 | 38 | 26 | 36 | 24 | 9 |
| U16-A | garage | 10,7 | 49,2 | 62,9 | 3.946 | 338 | 91,6 | 103 | 40 | 44 | 58 | 18 | 11 |
| U16-D | cucina | 16,6 | 13,0 | 59,1 | 649 | 241 | 100,0 | 94 | 34 | 19 | 23 | 32 | 8 |
| U16-D | strada | 9,1 | 56,2 | 55,7 | 4.073 | 355 | 96,4 | 106 | 39 | 50 | 59 | 19 | 12 |
| U16-D | filmato | 20,1 | 18,2 | 48,7 | 970 | 282 | 99,2 | 104 | 38 | 29 | 39 | 23 | 9 |
| U16-D | garage | 10,6 | 50,0 | 64,8 | 4.044 | 344 | 95,2 | 105 | 40 | 49 | 61 | 20 | 11 |

Lettura: U16 contro U15-A **fps invariati** in ogni scena (le differenze stanno nel rumore e nei percorsi non identici);
**audio migliore** (strada 90,7 -> 95,0 %, filmato 92,0 -> 98,8 %, garage 85,2 -> 91,6 %) con la stessa CPU del mixer:
il guadagno è delle forme di codegen 136/140 (§2.22), non di un thread in più. Nessuna compilazione attesa da un draw.

**Anomalia U16: il ripristino in due passaggi non è mai partito sulla console.** Log di A e P:
"`0 restores in two passes (GTA IV's depth-words copy), 4001 in nine`" (sul PC, stesso codice: tutti in due). Il profilo P
attribuisce ancora all'etichetta della copia `B3D105989610E354` **25,6 ms su 60,0** in cucina (U15-P 25,6 su 60,9) e
**20,6 su 58,0** in strada. **Causa, dimostrata nel codice:** su deko3d quasi ogni draw passa da `FastDrawLocked` (A: 421.257
veloci, 178 completi, 100,0 %), che aggiorna `write_seq` delle superfici ma non chiama `NoteGta4DepthWordsCopy` (che sta
solo nel percorso completo `DrawStateLocked`); il segno della copia resta vecchio e il ripristino sceglie i nove passaggi.
Il D3D12 del PC non ha il percorso veloce (`FastDrawLocked` esce subito se non è deko3d): per questo il PC li prendeva.
**Correzione U17 (in `render/draw.cpp`, non committata):** con `gta4_renut_depth_restore_stencil_fast` accesa, la copia
`B3D105989610E354` (due draw a frame) resta sul percorso completo, come già gli shader "fur" (`Gta4WordsCopyStaysFull`
nelle due liste: il controllo degli shader di `FastDrawLocked` e `shaders_fast` del percorso completo). PC: compila; nessun
effetto sul PC (nessun percorso veloce), log 8.001/8.001 in due passaggi, 67 fps nel corridoio. Atteso sulla console (da
misurare): l'etichetta da ~25 ms verso pochi ms in cucina e in strada.

**Dove va il tempo CPU (U16-A).**
- **Cucina e filmato: limite GPU.** Il thread del gioco aspetta le fence della GPU ~780 ms al secondo (riga "game thread's
  waits for the renderer": fence 16/s, 778 ms); i core 0-2 non sono pieni (cucina 247 % di richiesta sui core del gioco).
- **Strada e garage: core 0-2 saturi.** Richiesta dei thread ammessi sui core 0-2: guest (maschera 0x7) 296 % + render
  thread di ReNut (solo core 1) 61 % = **~357 % su 300 %**; il gioco aspetta le fence solo 30-60 ms al secondo. Il **core 3
  è concesso al processo (maschera 0xF) ma non usato** (`banjo_system_core = 0`, scelta di U7). Nessun thread del fotogramma
  è al 100 % (main 38-40 %, render guest ~50 %, render ReNut ~60 %, GPU ~57 ms su un frame di ~107 ms): la catena main ->
  render guest -> render ReNut -> GPU è in serie e i thread pronti aspettano un core (il profilo non misura il tempo in
  coda: lo si deduce dalla richiesta oltre la capacità).
- **Il mixer XAudio (supervisor, entry `0x82169400`, priorità 0x2D) occupa da solo ~105 % di un core** in ogni scena, sopra i
  thread del gioco (0x3B): router `sub_8216D7E0` 27,6 %, `828C6740` 16,9 %, biquad `828C2D28` 15,4 %, `828C5A58` 11,3 %,
  `82185E38` 8,0 %, `828C7848` 5,5 % dei suoi campioni. Con il thread "audio 2" (`sub_828A…`, 18-24 %) e l'XMA (9-13 %)
  l'audio prende ~1,3-1,4 core su 3.
- **Render thread di ReNut** (core 1, 0x2C), strada: 38 % in attesa di lavoro, 12 % in attesa di uno slot (fence della GPU,
  `WaitSlotLocked`), 3 % acquisizione dell'immagine, ~46 % di calcolo: `FastDrawLocked` 21,9 %, `PushFloatConstants` 6,7 %,
  `WholeGeometryTry` 6,6 %, `QueueCapture` 5,1 %, `FillTextures` 4,6 %, `PagesClean` 3,6 %.
- **Render thread del gioco** (~50 %): `CaptureDelta` 9,7 %, `Push` 4,2 %, `sub_82871130` 3,9 %, `_malloc_r` 3,4 %; attese:
  semaforo (`sub_829A96E8`) 29 %, `BlockOnFence` 11,5 %.
- **Thread principale** (~39 %): attese su semaforo 28 %, `KeDelayExecutionThread` 10 %, WaitMultiple 18 %; il 40 % degli
  stack si ferma nel codice guest (senza catena dei frame).
- Spin e polling: 4.000-5.800 yield/s e ~1.800 sleep brevi/s (~280 ms/s); i tre worker del job system a ~11 % ciascuno
  (la cessione di U9 regge).

**Risoluzione fissa contro dinamica (D).** 14 cambi in ~6 minuti, nessuna oscillazione rapida (salita di un passo ogni 2 s).
In cucina 720 -> 684 -> 540 righe in 5 s, ma **la GPU resta a 59-60 ms a 540**; nel filmato a 540 righe 48,7 ms contro 49,0
di A: il tempo GPU di queste scene non dipende dall'altezza 3D (ripristino della depth, ombre 1280x1280, passaggi a misura
fissa). In strada e garage il limite è la CPU e il budget con il lato CPU (89-103 ms) la riporta a 720. Nessun costo CPU
misurabile (render thread 13,0 contro 13,2 ms, 56,2 contro 56,0). Si tiene spenta finché i passaggi fissi non calano.

**Vetri, fari e ombre intermittenti (dimostrato nelle catture, causa non dimostrata).** Garage, auto e telecamera ferme
(A, 262-289 s): fra due catture la differenza è **solo** nel parabrezza, nei vetri dei fari (e nei due passeggeri che si
muovono); il resto è identico al pixel. Lo stesso vetro appare chiaro e riflettente in una cattura, scuro nella successiva.
Vetri, fari, finestre e ombre campionano texture prodotte nello stesso frame (mappa di riflessione 256x256: 255 draw a
frame; shadow map 1280x1280: 1.582 draw a frame in strada). Le differenze PC/console in gioco qui sono tutte solo deko3d:
- `FastDrawLocked` (100 % dei draw sulla console, assente sul PC): riparte dallo stato del draw precedente e rifà solo ciò
  che i bit di campo dicono cambiato; il caso del ripristino veloce mostra che quello che vive solo nel percorso completo
  non lo vede (la stessa famiglia del difetto delle ombre di kabba536: stato non ripristinato);
- `banjo_deko3d_barrier_lean` / `_merge` (cache delle texture non invalidata in certe transizioni, barriere fuse);
- `banjo_deko3d_texture_skip` (un draw con gli stessi descrittori non rilega le texture).
Sul PC il salvataggio arriva nello stesso corridoio della console (catture 40-100 s uguali), ma il D3D12 non esegue nessuno
dei tre percorsi: **il confronto draw per draw non può mostrare la divergenza**, che nasce in codice che il PC non ha (il
Vulkan del PC si chiude in amdvlk64, §2.13). Il garage del taxi si raggiunge solo dopo il filmato della missione (~4 minuti
di gioco): sul PC non riprodotto. Procedura proposta sulla console (sotto): bisezione a interruttori, un'auto ferma,
catture ogni secondo.

**Ottimizzazioni candidate (esame dei riferimenti).** Le 141 righe di `docs/ultimate-fixes.md` della base (`3e6ae026`)
sono già nel nostro albero (U15). Restano: le sezioni "needs the game's own address" (le query di occlusione: GTA IV non le
usa, §2.20), "what base ports cost sasrt-nx" (porte 76/215 e 101: -0,4 ms a frame del render thread su sasrt-nx, da
provare spente se GTA IV non ne ha bisogno) e "off-by-default". Dal port di kabba536 (`gta4nx-switch.patch`): il thread
XAudio2 sul core 3 ("~85 % di un core tolto ai core 0-2", non rigido: preferisce il core 3 e può migrare), il thread
principale a 46 sopra gli altri guest ("pronto ma fermo ~24 %"), l'audio host a 43.

| Candidata | Problema | Codice | Beneficio atteso | Rischio | Verifica | Stato |
|---|---|---|---|---|---|---|
| Ripristino veloce anche su deko3d | 20-25 ms di GPU a frame nell'etichetta della copia | `render/draw.cpp` (`Gta4WordsCopyStaysFull`) | GPU -10..-20 ms a frame in cucina/strada (da misurare) | basso: due draw a frame sul percorso completo, già PC-verificato in U16 | log "N restores in two passes", profilo GPU, luce e cielo nelle catture | **fatto**, PC compilato |
| Mixer XAudio sul core 3 | core 0-2 saturi in strada, core 3 libero | `cpu/gta4_xaudio_threads.cpp` (`gta4_xaudio_supervisor_core3`, 0 di default) | fino a ~1 core in più per main/render in strada | audio se il sistema occupa il core 3 (modalità 2); nessun effetto sul codice guest | A/B sulla console: fps strada, audio %, CPU per maschera | **fatto**, spento; ramo Switch non compilato (nessuna build Switch autorizzata) |
| Thread principale sopra gli altri guest | main pronto ma senza core | nessun codice: `switch_thread_priorities = "Main XThread=0x2E"` | qualche ms del frame in strada | i worker e lo streaming scendono sotto | A/B sulla console | da provare |
| Nativi esatti del mixer (biquad `828C2D28`, `828C6740`, `828C5A58`) | mixer ~1 core | copie a mano con guardia bit-exact | -20..-40 % del mixer (stima) | correttezza DSP | guardia densa sul PC + ms per blocco sulla console | non iniziato |
| Porte 76/215 e 101 spente | lavoro del render thread a ogni op | toml | ~0,4 ms a frame (sasrt-nx) | GTA IV potrebbe averne bisogno | catture + render thread | da provare |
| Risoluzione dinamica | GPU nelle scene GPU-bound | base | nullo oggi (GPU non proporzionale) | - | dopo il ripristino veloce | spenta |

**U17 nell'albero (non committato):** la correzione del ripristino veloce e la chiave del core 3 (spenta). Build PC
incrementale riuscita (`draw.cpp`, `gta4_xaudio_threads.cpp`), run PC dal salvataggio U16: avvio, corridoio, due passaggi.

**Prova U17 proposta (non costruita):** una build Switch con un job (U16 + le due modifiche), stessa cache DKSH:
**A** (toml di U16-A: il ripristino veloce contro U16-A), **C1** (A + `gta4_xaudio_supervisor_core3 = 1`), **C2** (A + `= 2`),
e per i vetri, fermi accanto a un'auto (garage del taxi o un'auto parcheggiata) con una cattura al secondo per 60 s:
**G0** (A), **G1** (A + `banjo_renut_fast_draw = false`), **G2** (A + `banjo_deko3d_barrier_lean = false` +
`banjo_deko3d_barrier_merge = false`), **G3** (A + `banjo_deko3d_texture_skip = false`). Il primo G in cui il vetro smette di
alternarsi localizza la causa; se G1, `banjo_renut_fast_draw_verify = true` dice quale parte dello stato è sbagliata.

### 2.24 U17: la build Switch e il pacchetto A/C/G; TW2DE-NX, i port 142-147 della base, gli altri port (2026-11-16; build sul PC, nessun run su console)

**Build (fatto verificato sul PC, non sulla console).** `tools\ultimate\build_switch.bat everyday` con
`THEFT4_SWITCH_JOBS=1`, nessun'altra build in corso. Nuova variabile facoltativa `THEFT4_BUILD_LABEL` dello script: aggiunta
allo stamp (`6be295853eed-dirty+u17`), così il log distingue U17 da U16 (stesso commit, entrambe non committate); ricompila
solo `main.cpp`. Ninja ha ricompilato 3 file (`main.cpp`, `cpu/gta4_xaudio_threads.cpp`, `render/draw.cpp`) e collegato: il
resto dell'NRO e `app/generated` sono quelli di U16. Nessun errore; l'unico avviso è quello di sempre (`renut_pch.h:184`,
attributo su `asm`). Il ramo Switch di `gta4_xaudio_supervisor_core3` (mai compilato prima) compila e collega
(`__real_svcSetThreadCoreMask` risolto dal `--wrap` dell'SDK). Nell'ELF: lo stamp `+u17`, le chiavi
`gta4_xaudio_supervisor_core3` e `gta4_renut_depth_restore_stencil_fast`, la riga del log "restores in two passes", la riga
"XAudio supervisor on core 3"; ogni chiave dei toml del pacchetto esiste come cvar nell'ELF.

| File | Byte | SHA-256 |
|---|---|---|
| `out/console-runs/u17/COPY_TO_SD/switch/theft4-ultimate/theft4-ultimate.nro` | 65.177.797 | `15da0fff66b66463c00f547bcebb79d14734211ea27ad8594692f25e1137399e` |
| `out/console-runs/u17/theft4-ultimate.elf` | 82.188.808 | `6755dbd136997efbb613a1b36ca73cef2ae45385447705b4299de1ffc8c23053` |
| `.../deko3d_shaders.bin` (cache U10, invariata) | 51.766.560 | `438346a32d17b81a9d2032c782ab6d0a95317e63eb08d7b34587138d78639ae1` |

Il pacchetto (`tools/ultimate/package_switch.py --allow-dirty`, manifest `ultimate-manifests/switch-u17.json`) ha
`README-U17.txt`, `SHA256SUMS-U17.txt` e `u17-uncommitted.patch` (il diff di `ultimate/` e `tools/ultimate/` contro `6be2958`,
verificato con `git apply --check --reverse`). L'NRO U16 (`4424ac8a…`, `out/console-runs/u16`) e i suoi run non sono toccati.

**Le varianti (un solo cambiamento ciascuna; toml letti con `tomllib`, chiavi confrontate).** Base comune: il toml di U16-A.

| Toml | Run | Rispetto a | Cambiamento |
|---|---|---|---|
| `gta4-U17-A.toml` | A | U16-A | `renut_resolution = "720p"`, `banjo_renut_dynamic_resolution = false` espliciti (già i default: 720p fissi per tutto il confronto); l'NRO U17 |
| `gta4-U17-C1.toml` | C1 | A | `gta4_xaudio_supervisor_core3 = 1` (il supervisor preferisce il core 3, può migrare) |
| `gta4-U17-C2.toml` | C2 (facoltativa) | A, C1 | `gta4_xaudio_supervisor_core3 = 2` (solo core 3; modalità esclusiva, mai di default) |
| `gta4-U17-G0.toml` | G0 | A | catture a raffica: 3 ogni ~2 s, 200 raffiche (`nfsmw_captura_max` conta le raffiche: 600 PNG, almeno 400 s) |
| `gta4-U17-G1.toml` | G1 | G0 | `banjo_renut_fast_draw = false` |
| `gta4-U17-G2.toml` | G2 | G0 | `banjo_deko3d_barrier_lean = false`, `banjo_deko3d_barrier_merge = false` (una leva: l'ottimizzazione delle barriere) |
| `gta4-U17-G3.toml` | G3 | G0 | `banjo_deko3d_texture_skip = false` |

Mixer, DSP, priorità (0x2D, U11) e qualità audio sono gli stessi in ogni variante: C1/C2 cambiano solo il core del
supervisor. Da leggere nei log: "build 6be295853eed-dirty+u17"; "N restores in two passes" con N > 0 (A: il ripristino
veloce ora attivo sul deko3d; da misurare il costo dell'etichetta `B3D105989610E354` contro i 20-25 ms di U16, nessuna stima
presentata come misura); C1/C2: "XAudio supervisor on core 3 … now preferred core 3 mask 0x…".

**Vetri (ipotesi, non dimostrata).** Lo studio di LibertyRecomp (`refs/LibertyRecompAndroid/docs/reflection-rendering-study`)
documenta che i vetri dei veicoli (`gta_vehicle_vehglass_ps0`, `EnvironmentTex` legato da `sub_8250B190`) e la vernice
campionano la mappa paraboloide 256x256 (`REFLECTION_MAP_COLOUR/DEPTH`, creata da `sub_822D93E8`) prodotta nello stesso
frame. Un vetro "chiaro, poi scuro" a scena ferma è compatibile con una lettura di quella mappa prima della scrittura o con
una cache delle texture non invalidata (`barrier_lean`): G2 è un indiziato forte, ma la causa la dice solo la bisezione.

**TW2DE-NX (`recomp-nx/tw2de_nx`, `e24ac6a`, 2026-11-15; un solo commit di codice "Init #1" più README e la lista delle pipeline
in testo).** Esaminati: README, `build_switch.py` (893 righe), i tre `.bat`, `app/CMakeLists.txt`, `tools/` (51 file),
`app/src/pgo_switch.*`, `render/fsr1|fsr3|early_upscale|dynamic_resolution.cpp`, la toolchain.

| Tecnica | Codice TW2DE | Da noi | Stato | Beneficio / costo | Prova |
|---|---|---|---|---|---|
| PGO (GCC, `-fprofile-generate/use`) | `build_switch.py --pgo`, `pgo_switch.cpp` (un `.fxpg` a run, `--wrap=__gcov_init`), `pgo_unpack.py`, `pgo_functions.py` (id delle funzioni statiche fra cartelle) | la base ha già la sua catena: `BANJO_PGO=generate/use`, flussi `pgo/banjo_pgo_<N>.stream` ogni 2 min, `tools/pgo_merge.py`, `build_switch_release.bat`, profilo `pgo-inplace/` per le copie in place | **presente, mai usato per GTA IV** | non stimabile senza misura (dipende dalla copertura); costo: build strumentata completa (~2 h a 1 job), run rappresentativi, build release completa | NRO strumentato: cucina, strada, garage, filmato; release contro U17-A, stesse finestre |
| LTO + `-fipa-pta` | `TW2DE_LTO`, `-flto=auto` | `BANJO_SWITCH_LTO`, `BANJO_SWITCH_IPA_PTA` (spenti); 4 file SDK fuori LTO | **presente, spento** | rischio memoria al link (16 GB, ~9 GB liberi): da provare prima da sola | build LTO senza profilo, memoria massima del link, poi A/B |
| `-O3`, flag ARM64 | `-O3`, `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft`, `-mearly-ra=none -mno-outline-atomics` | **identici** (letti in `build.ninja`) | presente | - | - |
| Codegen "batched" (file da ~1 MB) | `--batched`, `one_function_per_file=false` | il nostro generato è già in 159 file `renut_recomp.N.cpp` | presente | - | - |
| `msr_fences.py` (fence `dmb ish` attorno a `mfmsr/mtmsrd`) | `tools/msr_fences.py` | il generatore 0.10 della base non emette fence lì (solo commenti) | equivalente presente | - | - |
| `pch_no_volatile.py` + `loop_barriers.py` (+6 % fps in TW2) | `post_codegen_switch.py` | applicati da `post_codegen.py` (sa2-nx) | presente | - | - |
| `vec_neon_forms.py` (vslw, vupkd3d128 0/4, vpermwi128, vcsxwfp scalati; esatti, provati con qemu) | `tools/vec_neon_forms.py`, `sdk/tests/vec_neon_forms_test.cpp` | NORMSHORT2 arriva col port 144; vslw 179 siti, vupkd3d128 16, vpermwi/vcsxwfp/vcuxwfp 2.222; nel mixer solo `828C5A58` (3 vslw, 1 vupkd3d) | **trasferibile** (il resto dopo il 144) | piccolo (stima, non misurato); costo medio: porto + test qemu + guardia | guardia densa sul PC, ms del render guest sulla console |
| Shader DKSH anticipati | pre-passaggio sulla console al primo avvio, `pipeline_list.txt` da unire fra giocatori; host shader con uam PC nel romfs | cache DKSH offline (`deko3d_shaders.bin`, U6-U10) + 33 host shader nell'NRO; unione dei run con `gta4_dksh_cache.py --console-merge` | presente (più avanti) | - | - |
| FSR 1 (EASU+RCAS), Lanczos-2, upscale precoce (UI a risoluzione d'uscita) | `fsr1.cpp`, `early_upscale.cpp`, `lanczos2_ps.glsl` | base: bilineare/bicubico (`banjo_renut_upscale_filter`) | adattabile | oggi nullo: la GPU di GTA IV non scala con l'altezza 3D (U16-D) | dopo che i passaggi fissi calano: D con FSR1 contro bicubico |
| FSR 3.1 (temporale) | `fsr3.cpp` (1.152 righe), vettori di moto del gioco | GTA IV non ha un target di vettori di moto noto | **incompatibile** senza vettori sintetici | costo alto | - |
| Risoluzione dinamica | `dynamic_resolution.cpp` (budget 30,9 ms) | quella della base (U16-D) | presente | - | - |
| Thread | profiler e watchdog sui core 0-2 | nulla di nuovo per il core 3 | - | - | - |

**La base è andata avanti (`bf6ccc1e`, 2026-11-16): port 142-147, i "speed-up" di TW2DE accesi, le sue correzioni spente.**
Le righe 142-147 di `docs/ultimate-fixes.md` lette per intero (`git show origin/main:` nel clone, il cui checkout resta
`8de5d51a`). Non sono nel nostro albero (U15 ha preso fino a 141); **non portati in U17** (le varianti U17 restano
pulite). Rilevanza per GTA IV (dal codice generato e dai profili U16, da misurare):
- **145 renderer** (acceso): `banjo_renut_stream_redeem` (TW2 -7 ms di render thread a frame), `keep_unused_targets` 250 ms,
  `dynamic_texture_rows`, `native_16bit` (deko3d), `banjo_async_present` (da confrontare sulla console). Il nostro render
  thread ReNut in strada è al 46 % di calcolo (`FastDrawLocked` 21,9 %, `WholeGeometryTry` 6,6 %, `FillTextures` 4,6 %).
- **146 runtime**: `thread_mutant_release_lockfree`, `filesystem_bounce_piece_mb` 4 (ogni pezzo ~2,3 ms fissi sulla console:
  lo streaming di GTA IV legge molto), `filesystem_bounce_guest_only`.
- **144 codegen**: `spin_hint_yield` (GTA IV ha 12 `db16cyc`: in `sub_8216C750` e `sub_82169D90` dell'XAudio, `sub_829CFE80`
  (8 in fila) e `sub_829DD928` del D3D) e NORMSHORT2 in NEON (8 siti). Serve una rigenerazione (`post_codegen.py`) e la
  guardia.
- **143 kernel**: niente attesa di 100 ms prima di ogni chiamata overlapped differita (salvataggi, enumerazioni).
- **142**: la chiave `kernel_timestamp_timer_on_import` tolta (era spenta).
- **147**: test delle istruzioni PowerPC (NRO di test), nessun effetto sul gioco.

**Gli altri port in `refs/` (scansione per parole chiave di sorgenti e documenti, `refs_scan.py`; lettura completa solo dove
indicato).** I port di Chansey (katamari, rr6, sa2, nfsmw, bejeweled3, torchlight, renut-nx, mgr-nx-deko, masseffect-nx)
arrivano nel nostro albero attraverso la base, che li ha già fusi (ultimate-fixes 1-147): nessun duplicato da prendere.
Nuovi per noi: **LibertyRecompAndroid** (GTA IV su PC/Android; studio dei riflessi letto: la mappa paraboloide, sopra; i
documenti su streaming, collisioni, handle UAF non letti per intero), **gta4-kabba536** (già in §2.23: XAudio2 sul core 3,
main a 46). `unleashed-*` e `UnleashedRecomp`: renderer nativo D3D12/Vulkan per PC; la scansione non vi trova PGO, LTO né
codice di core o Switch (solo la scansione, non una lettura). Non letti per intero: `masseffect-nx`, `mgr-nx-deko`,
`unleashed-*` e il resto di LibertyRecomp: restano da leggere (aperto).

**Prossimo ciclo (U18), in ordine, dopo i run U17** (le misure U17 sono il riferimento):
1. Leggere A, C1/C2, G0-G3. Se G localizza il vetro: correzione mirata e una prova sola.
2. Port 142-147 con `ult_port_sync.py` (le tre parti di app/sdk/tools), rigenerazione (144), guardia densa sul PC, build
   Switch a 1 job, A/B sulla console contro U17-A (render thread, fps strada/garage, `[latency]` per l'async present).
3. Se C1 aiuta senza peggiorare l'audio: renderla default per GTA IV; poi il main thread a 0x2E (kabba536) come A/B.
4. Build LTO da sola (memoria del link a 1 job), poi il ciclo PGO della base con profili di cucina, strada, garage e
   filmato (`pgo-inplace/`).
5. Nativi esatti del mixer (biquad `828C2D28`, `828C6740`, `828C5A58`) con la guardia bit-exact; `vec_neon_forms` dopo il 144.

### 2.25 U17 letto sulla console; C1/C2 spiegati; lampeggiamenti localizzati fuori da G1-G3; nuova partita sul PC; port 142-147 divisi per gruppi (2026-11-16; console letta, PC provato, nessuna build Switch)

**Materiale e attribuzione (fatti).** `out/console-runs/u17/COPY_TO_SD/switch/theft4-ultimate/{A,C1,C2,G0,G1,G2,G3}` (log,
`logs/rex/rex_profile.log`, catture; ordine dei run dalla crescita di `pipeline_list.bin` e dagli orari: A, C1, C2, G0, G1,
G2, G3). Ogni log dice `build 6be295853eed-dirty+u17`; NRO sulla scheda ancora `15da0fff…`; clock 1020/460,8 MHz in ogni
blocco del profilo. La leva di ogni run si legge nel suo log: C1 "now preferred core 3 mask 0xF", C2 "mask 0x8, running on
core 3"; G1 0 draw veloci su 370.863 (G0 99,9 %); G2 `sasrt_gpu_lean_barrier_skip` 0 (G0 4.189) e `readback_barrier_merge` 0
(G0 252); G3 0 texture saltate (G0 276.499). G0 differisce da A **solo** per le catture (toml confrontati: raffiche di 3 ogni
~2 s al posto di una ogni 5 s). Strumenti nella cartella di lavoro: `u17_scenes.py` (scene per draw a frame, profilo allineato
al log del renderer sugli fps), `u17_io.py` (letture lente), `flicker.py` (differenze dentro una raffica), `u17_threads.py`.

**Per scena (misure Switch, mediane; il profilo e il log del renderer hanno orologi separati, allineati sugli fps).** CPU dei
thread in % di un core; "audio" = blocchi mescolati / chiesti. Le scene sono riconosciute dai draw a frame, i percorsi non
sono identici fra i run (G0 a 240-279 s guida, non è fermo accanto all'auto).

| Run | Scena | fps | p10 | render thread ms | GPU ms | draw/frame | CPU tot % | audio % | mixer | main | render guest | render ReNut |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| U16-A | cucina | 16,6 | 16,5 | 13,1 | 59,3 | 652 | 244 | 100,0 | 94 | 34 | 35 | 23 |
| U16-A | strada | 8,9 | 7,6 | 57,8 | 56,4 | 4.605 | 353 | 94,4 | 109 | 38 | 50 | 59 |
| U16-A | filmato | 20,0 | 19,8 | 17,8 | 49,0 | 938 | 333 | 94,2 | 105 | 40 | 38 | 52 |
| U16-A | garage | 10,7 | 9,5 | 49,1 | 63,7 | 3.961 | 336 | 90,9 | 103 | 38 | 45 | 59 |
| **U17-A** | cucina | **24,6** | 23,2 | 14,0 | **39,2** | 673 | 320 | 98,4 | 102 | 59 | 35 | 38 |
| U17-A | strada | 9,4 | 7,8 | 53,9 | **35,8** | 4.747 | 350 | 98,0 | 106 | 38 | 47 | 58 |
| U17-A | filmato | **25,1** | 24,6 | 17,3 | **38,6** | 897 | 321 | 96,9 | 102 | 47 | 37 | 48 |
| U17-A | garage | 11,1 | 8,9 | 43,7 | **40,0** | 3.140 | 367 | 95,5 | 111 | 37 | 46 | 56 |
| U17-C1 | cucina | 26,0 | 25,7 | 12,8 | 37,6 | 650 | 306 | 95,8 | 98 | 56 | 35 | 36 |
| U17-C1 | strada | 10,9 | 10,3 | 55,4 | 46,9 | 4.688 | 376 | **80,9** | **90** | 46 | 58 | 71 |
| U17-C1 | filmato | 24,4 | 23,3 | 16,8 | 40,2 | 878 | 308 | 88,9 | 98 | 48 | 34 | 42 |
| U17-C1 | garage | 12,2 | 10,9 | 47,8 | 46,2 | 3.689 | 377 | **78,2** | **92** | 45 | 54 | 65 |
| U17-C2 | cucina (2 finestre) | 33,2 | 26,4 | 10,3 | 24,3 | 517 | - | - | - | - | - | - |
| U17-C2 | strada | 14,4 | 11,8 | 44,9 | 47,9 | 3.626 | (profilo non valido) | **72,0** | - | - | - | - |
| U17-C2 | filmato | caricamento di **86 s**, 1 finestra, poi altri **34 s** senza immagine | | | | | | | | | | |
| U17-C2 | garage | 14,1 | 8,6 | 42,6 | 46,5 | 3.579 | - | - | - | - | - | - |
| U17-G0 | cucina | 24,8 | 23,7 | 14,4 | 37,7 | 640 | 308 | 99,4 | 102 | 56 | 34 | 40 |
| U17-G0 | strada | 9,1 | 7,6 | 58,0 | 37,9 | 4.481 | 365 | 95,6 | 111 | 40 | 47 | 59 |
| U17-G0 | filmato | 26,0 | 23,8 | 16,6 | 33,6 | 786 | 344 | 93,5 | 105 | 40 | 39 | 55 |
| U17-G1 | cucina | 22,5 | 21,4 | **18,1** | 39,0 | 685 | 245 | 99,9 | 70 | 36 | 34 | 39 |
| U17-G1 | strada | **8,2** | 7,2 | **65,4** | 38,0 | 4.255 | 368 | 96,5 | 112 | 37 | 46 | 66 |
| U17-G1 | garage | 9,9 | 8,5 | 57,9 | 41,6 | 3.646 | 361 | 94,6 | 109 | 35 | 48 | 71 |
| U17-G2 | strada | 8,9 | 7,7 | 58,3 | 36,5 | 4.540 | 367 | 95,0 | 111 | 38 | 50 | 60 |
| U17-G2 | garage | 9,6 | 8,9 | 52,7 | 40,8 | 3.936 | - | - | - | - | - | - |
| U17-G3 | cucina | 24,7 | 22,3 | 14,5 | 38,0 | 652 | 308 | 99,5 | 101 | 53 | 34 | 40 |
| U17-G3 | strada | 9,4 | 8,4 | 53,7 | 40,0 | 4.134 | 362 | 95,4 | 111 | 39 | 46 | 59 |
| U17-G3 | garage | 9,9 | 8,6 | 47,9 | 40,4 | 3.700 | 356 | 92,8 | 108 | 39 | 42 | 55 |

Audio sull'intera run (dai 60 s): A 97,2 % (peggiori 10 s 93,2), C1 84,2 (76,5), C2 77,3 (66,7), G0 96,0, G1 95,7, G2 95,3,
G3 96,1; U16-A 95,8. Nessuna compilazione attesa da un draw in nessun run (A: 23 compilazioni uam in background).

**Il ripristino in due passaggi funziona sulla console (fatto).** Tutti i run: "`8001 restores in two passes (GTA IV's
depth-words copy), 0 in nine`" (A; U16-A: 0 e 4.001). Effetto misurato: GPU per frame **59,3 -> 39,2 ms in cucina** (fps 16,6 ->
24,6, +48 %), 49,0 -> 38,6 nel filmato (20,0 -> 25,1, +25 %), 56,4 -> 35,8 in strada, 63,7 -> 40,0 in garage. In strada e garage
gli fps non cambiano (9,4 / 11,1 contro 8,9 / 10,7): lì il limite è la CPU (render thread 44-54 ms, core 0-2 saturi), non la
GPU. Luce e cielo nelle catture come U16.

**C1 e C2: causa (dimostrata dai log, il meccanismo del sistema è un'ipotesi).**
- Il "supervisor" spostato è il **mixer XAudio stesso** (`0x82169400`, priorità 0x2D): in A usa 102-111 % di un core (oltre
  100 % = il blocco del profilo dura un po' più di 10 s), cioè è **saturo**: 94 % del tempo sulla CPU, tutto calcolo DSP
  (router `8216D7E0` 28,9 %, `828C6740` 15,7 %, biquad `828C2D28` 14,6 %, `828C5A58` 11,4 %, `82185E38` 8,7 %); le attese di
  rotazione (`db16cyc` in `8216C750`) sono 0,3 %.
- **C1** (preferisce il core 3, maschera 0xF): il mixer riceve **90-92 %** invece di 106-111 in strada e garage; l'audio scende a
  **80,9 % / 78,2 %** (A 98,0 / 95,5): la voce "robotica" è il gioco che non produce l'audio in tempo. Gli fps salgono
  (strada 9,4 -> 10,9, garage 11,1 -> 12,2) perché i core 0-2 restano più liberi: guadagno pagato con l'audio. Il core 3 non è
  libero: la base ha misurato (banjo-nx, 2026-10-27, `threading_switch.cpp`) che **il sistema ne usa circa metà**; il mixer,
  preferendolo, ci resta anche quando il sistema glielo toglie (la migrazione non è immediata).
- **C2** (solo core 3): stessa perdita, più grave (audio 72 % in strada, 77,3 % sulla run). In più un thread a 0x2D che vuole
  un core intero su un core condiviso col sistema **affama i servizi del sistema sul core 3**. Misure: le letture dal SD del
  gioco (`[io] SLOW`) passano da mediana 10 ms a **51 ms** (p90 17 -> 107, massimo 220 -> 727 ms; 3,6 -> 16,9 s in tutto;
  ~16 -> **2,5 MB/s**), solo in C2 (C1 10 ms); il caricamento del filmato dura **86 s** più 34 s senza immagine (A: nessuna
  finestra di caricamento oltre 10 s); il report del nostro profilo, che si scrive sul SD, arriva ~2,9 volte in ritardo (15
  blocchi in 432 s; per questo C2 mostra "CPU 1002 %", un artefatto: i tick dei thread vengono letti dopo l'attesa, il tempo
  del blocco prima). Le regressioni riferite (NPC che spariscono = modelli `componentpeds.img` letti 4,3 s in tutto contro 0,2;
  cielo parzialmente nero; "It's your call" prolungato; caricamenti lenti; UltraHand non interagibile) sono coerenti con
  **file system e overlay del sistema, che girano sul core 3, rallentati dal mixer**: questo meccanismo è un'ipotesi (non
  vediamo le priorità dei servizi), le misure di I/O sono un fatto.
- **Decisione:** C1 e C2 non si usano più (nessun vantaggio senza perdita audio); la chiave resta a 0 (spenta) di default.
  Nessun cambiamento a DSP, qualità o priorità. La strada per liberare i core 0-2 è un mixer più economico (nativi esatti),
  non un altro core.

**G0-G3 e i lampeggiamenti.**
- Costo delle catture (G0 contro A, stesso percorso fino al filmato): cucina uguale (24,8 / 24,6 fps), strada 9,1 contro 9,4 fps
  con render thread 58,0 contro 53,9 ms: **le raffiche costano ~4 ms di render thread a frame** in strada. G0 non è un
  riferimento di prestazioni.
- **G1** (`banjo_renut_fast_draw` off): render thread +7,4 ms in strada (65,4 contro 58,0), fps 8,2 contro 9,1, cucina 22,5
  contro 24,8: il percorso veloce vale ~11 % in strada. Nessun miglioramento visivo (utente). Resta acceso.
- **G2** (barriere lean/merge off) e **G3** (texture skip off): prestazioni uguali a G0 (strada 8,9 / 9,4 fps, GPU 36,5 /
  40,0 ms); nessun miglioramento visivo (utente). Per G3 lo conferma la misura sotto.
- **Alone della lampada: misurato.** Zona della lampada del corridoio-cucina (pixel 545-615 x 40-85), Niko e telecamera fermi:
  sulla console la luminanza salta fra **due valori discreti, 60,8 e 78,8, da una cattura all'altra della stessa raffica**
  (G0 37-69 s; G3 identico, `texture_skip` spento). Sul **PC** (D3D12, stesso salvataggio copiato, stessa stanza, raffiche
  uguali) la stessa zona **non alterna mai**, né a 60+ fps né con `frame_cap = 30` (valori continui 30-37, nessun doppio stato):
  non è il gioco e non è il frame rate, è un percorso che solo la console esegue, diverso da fast draw (utente), barriere e
  texture skip. Due stati che si alternano = qualcosa con due copie per frame in volo (anelli di upload, buffer di costanti a
  frame alterni, una risorsa riusata dal frame precedente). Ombre in strada e vetri: stessa famiglia probabile, non misurata
  (nessuna finestra ferma utile in G0-G3: in G0 a 240-279 s si guida).
- **Prossima prova (isolata, nessuna correzione speculativa):** una run di viste **V** (A + le verifiche del renderer che
  confrontano il modello con i dati del gioco e scrivono nel log la prima differenza: `banjo_deko3d_push_constants_verify`,
  `banjo_renut_fast_draw_verify`, `banjo_renut_chain_reuse_verify`, `banjo_renut_texture_slot_skip_verify`,
  `banjo_renut_store_cards_verify`, `banjo_deko3d_txq_verify`), 60 s fermi nel corridoio con raffiche; poi, se il log non
  nomina nulla, una bisezione per gruppi solo-deko3d: **H1** hand-over (`banjo_deko3d_depth_handover`, `_color_handover`,
  `_scanout_handover` off), **H2** `banjo_deko3d_push_constants` off, **H3** `sasrt_gpu_skip_inert_draws`,
  `sasrt_gpu_precise_depth_resolve`, `sasrt_gpu_depth_restore_whole_clear` off, **H4** `banjo_deko3d_zcull`, `_early_z`,
  `_depth_compression` off. Un gruppo per run, stessa inquadratura.

**Prima missione dopo il filmato iniziale (segnalazione, non riprodotta).** Sul **PC** (stessa build del renderer, D3D12),
cartella utente **vuota** in `scratchpad` (il salvataggio vero non è toccato), nessun input: 560 s; il filmato iniziale
(nave, molo, titoli con Roman alla guida) finisce e a 376 s **Niko è al volante, HUD e radar con il percorso giallo della
missione**: la missione parte. Sulla console non provata. Da questo run, un fatto nuovo: la guardia delle copie native del PC
trova **4 copie diverse dall'originale** su un percorso che i run dal salvataggio non fanno: `82221530` (un byte scritto
`d4` contro `d7` in `0x82BD1A13`: il byte basso di un float, una differenza di arrotondamento, non un NaN), `8290F508`,
`8290F050`, `8290E808` (lane 0 di un vettore: `7fc00001` contro `ffffffff` e il contrario, un payload di NaN), ciascuna tre
passaggi di fila. Sul PC la guardia le spegne; **sulla console le copie sono "in place", senza guardia**: girano. Che
causino il blocco della missione non è dimostrato (`82221530` ha 216 campioni nei profili della console: gira anche nelle
partite caricate). Correzione preparata (stessa regola di U10, §2.16): le quattro fuori da `localized_bulk.txt`
(`out/u18-prep/g_bulk_drop.diff`, verificata con `git apply --check`; serve la rigenerazione delle copie). Procedura per la
console in `run-protocol.md` ("Dalla U18": slot separato, copia di sicurezza).

**Port 142-147 della base (letti nel codice: `git diff 3e6ae026..c84f7c33` e `c84f7c33..bf6ccc1e` del clone, `origin/main`
ancora `bf6ccc1e` dopo un fetch).** Divisi in gruppi che toccano file disgiunti (ognuno `git apply --check` pulito sul
nostro albero U17; solo i documenti della base vanno uniti a mano): `out/u18-prep/g142_code.diff`, `g143_kernel.diff`,
`g144_codegen.diff`, `g145_render.diff`, `g146_runtime.diff`, `g147_tests.diff`, `g_docs.diff`.

| Gruppo | Codice | Dipendenze | GTA IV: cosa tocca (dai nostri log) | Beneficio atteso | Rischio | Prova |
|---|---|---|---|---|---|---|
| 142 | toglie la chiave `kernel_timestamp_timer_on_import` (spenta da 139): timer creato all'avvio come prima | nessuna | nessun effetto (chiave spenta) | igiene | nullo | build |
| 143 | `kernel_deferred_overlapped_delay_ms` 0 con `kernel_overlapped_reset_event`; `xmp_poll_throttle`; spenti: `kernel_u64_args_whole`, `xam_swap_disc_signal`, `kernel_scatter_read_offset_zero`; fuso orario dell'host (`xconfig_host_time_zone`, spento) | nessuna | 2 `XamContentCreate` al caricamento del salvataggio | ~0,2 s al caricamento (100 ms a chiamata overlapped) | basso (la base l'ha provato su più giochi) | tempo dall'avvio alla stanza; salvataggio |
| 144 | `spin_hint_yield` (le `db16cyc` chiamano `rex_spin_hint`), forma NEON NORMSHORT2; molte chiavi di esattezza spente | **rigenerazione** (`post_codegen.py`), guardia densa | 12 `db16cyc` (XAudio `8216C750`, `82169D90`; D3D `829CFE80`, `829DD928`): nel mixer 0,3 % dei campioni | piccolo sul mixer; da misurare nei thread D3D | medio (rigenerazione) | guardia densa sul PC, poi console |
| 145 | `stream_redeem`, `keep_unused_targets`, `dynamic_texture_rows`, `native_16bit` (deko3d), `banjo_async_present`; `fetch_filters` spento | nessuna (plume, `--wrap=nwindowDequeueBuffer`) | target creati: 33 in tutta la run (G0), quindi `keep_unused_targets` quasi nullo; present: coda + dequeue **~0,5 ms a frame** (U17-A strada, profilo libnx) | render thread: async present al massimo ~0,5 ms a frame (misura); stream redeem da misurare (TW2: -7 ms) | async present: in Ryujinx picchi di latenza (la base lo dice da provare sulla console) | A/B sulla console; `[latency]` |
| 146 | `thread_mutant_release_lockfree`, `filesystem_bounce_piece_mb` 4, `filesystem_bounce_guest_only`; `crt_xmemcpy_as_memmove` spento | nessuna | letture grandi (`common.rpf` 17 MB in 228 ms) a pezzi da 1 MB | caricamenti, streaming | basso | tempi `[io]`, caricamento del filmato |
| 147 | test PowerPC (`tools/ppc_tests`, NRO di test) | nessuna | nessun effetto sul gioco | strumento | nullo | - |

`banjo_renut_fetch_filters` (spento in 145) è da provare a parte: è la correzione di TW2DE per i filtri dati dalle istruzioni
di fetch (FINAL FANTASY XIII: "raggi a gradini al posto di sfumati") e i bordi netti di alone e fascio della lampada sono la
stessa famiglia di sintomo (ipotesi).

**TW2DE-NX e PGO (complemento di §2.24).** La base ha preso da `vec_neon_forms.py` solo NORMSHORT2 (144) e già prima
VUPKD3D_COLOR; restano `REX_SLLV31` (vslw), `REX_PERMWI` (vpermwi), `REX_CVT/_SCALED/CVTU_SCALED` (vcsxwfp, vcuxwfp): i 2.222
siti contati in §2.24, nel mixer solo `828C5A58`. PGO: la catena della base (`BANJO_PGO=generate|use`, `tools/pgo_merge.py`,
`build_switch_release.bat`) non è mai stata usata per GTA IV; il piano (dopo il gruppo 144, che cambia il codice generato) è in
`19-optimization-matrix.md`.

**`refs/` (complemento).** LibertyRecomp, `docs/GTA4_COLLISION_VPKUWUS_ALIASING_FIX.md` letto: un `vpkuwus` con destinazione
uguale a una sorgente tradotto lane per lane faceva cadere attraverso le strade. **Già corretto da noi** (`build_vpkuwus` e
`build_vpkuhus` passano per `v_temp`, rr6-nx 2026-11-02). Controllo esteso a tutti i builder vettoriali (scrittura della
destinazione lane per lane mentre si leggono le sorgenti): l'unico caso non sicuro è `vpkd3d128` NORMSHORT4 con vD = vB
(`vector.cpp`, il ciclo scrive `u16[(3-i)+2*shift]` e legge `s32[3-i]`), ma GTA IV non lo usa (6 `vpkd3d128` nel codice
generato: tipi 2, 3 e 5): difetto latente dell'SDK, da correggere quando lo si tocca.

**Prossimo ciclo (U18), proposto, nessuna build Switch senza autorizzazione:** in `19-optimization-matrix.md`.

### 2.26 U18-1: la build Switch autorizzata, il pacchetto B0-B5, V, N (2026-11-16; build sul PC, nessun run su console)

**Contenuto (autorizzato dall'utente: gruppi 142, 143, 145, 146, 147 + le quattro copie tolte; niente 144, LTO, PGO,
mixer; C1/C2 escluse; fast draw acceso).** Base `168baa6` (U17). Le patch di `out/u18-prep` ricontrollate prima: hash
uguali a `README-U18-prep.txt`, ognuna e tutte insieme `git apply --check` pulite; i simboli che usano (`GetFlagByName`,
`RexGmBase/RexGmSize`, store cards, `nwindowCancelBuffer` di libnx) esistono nel nostro albero.

**Rigenerazione (fatto, misurato).** Le copie sono scritte "in place" nei file generati: togliere le quattro dalla lista
richiede `tools/ultimate/regenerate.ps1` completo (codegen 74 s, `post_codegen.py` 1.162 s; manifest
`ultimate-manifests/gen-u18-1.json`, 922 file, 37.067 funzioni, hash combinato `336793fad70bfb28`). Prima, l'albero U17 copiato
in `ultimate/out/generated-u17-backup` (930 file). Confronto completo dei due alberi (`gen_compare.py`):
- tolti solo `hot/n_82221530`, `n_8290E808`, `n_8290F050`, `n_8290F508` (`.h` e `.blocks.json`) e le loro righe in
  `banjo_hot_bulk_*.inc`;
- cinque copie che le chiamavano (`n_8290EF68`, `n_822828B0`, `n_823DB1E8`, `n_825EBA50`, `n_8266F3D0`): ora riscrivono al
  ritorno i registri che la funzione generata può leggere (`ctx.r10 = l_r10` ecc.), atteso;
- **87 `REX_LOOP_BARRIER()` in meno** (17.914 -> 17.827) in 39 etichette: un controllo indipendente
  (`barrier_check.py`, raggiungibilità sulle `goto`) dice che **nessuna è un ciclo**: dall'etichetta si arriva solo a un
  `return`. L'albero U17 le aveva perché il passaggio U16 di `post_codegen.py` girò su testo già elaborato
  (`REX_LEAF_DEAD_R` non letto come ritorno: barriera prudente). Nessuna barriera di un ciclo vero è sparita;
- 12 righe del flusso della modalità flush in 3 funzioni (`828B7ED8`, `828B0080`: un cambio di modalità ora dimostrato
  inutile, `REX_FLUSH_KNOWN_*`; `82229488`: di nuovo esplicito); `renut_pch.h`: lo stesso blocco `F64ANY` spostato e il
  censimento CAS solo con `REX_NX_CENSUS_ON` (mai definito nelle build). Da verificare con la guardia sul PC (sotto).

**Build Switch (fatto).** `tools\ultimate\build_switch.bat everyday`, `THEFT4_SWITCH_JOBS=1`, `THEFT4_BUILD_LABEL=u18-1`,
nessun'altra build in corso: 363 passi, 86 minuti, nessun errore; gli avvisi sono quelli dell'SDK in una ricompilazione
completa (nei file dei port: solo l'espressione originale di `XFile::ReadScatter` riformattata, ramo nuovo spento).
Stamp `168baa62b651-dirty+u18-1`. Pacchetto `out/console-runs/u18-1` (`package_switch.py --allow-dirty`, manifest
`ultimate-manifests/switch-u18-1.json`, cache DKSH U10):

| File | Byte | SHA-256 |
|---|---|---|
| `COPY_TO_SD/switch/theft4-ultimate/theft4-ultimate.nro` | 65.235.141 | `315669d34192752e61a38d8334f7640fc1b5e5546eb87382087804b923ea39d2` |
| `theft4-ultimate.elf` | 82.285.744 | `be4b450863d4a6a13f7a6ad027f80767ad7ccb7f764020cc99d3056c4d67552e` |
| `deko3d_shaders.bin` (U10, invariata) | 51.766.560 | `438346a3…` |
| `u18-1-uncommitted.patch` (il codice contro `168baa6`, 34 file, `git apply --check --reverse` pulito) | 101.894 | `0ecf0cbf…` |

**Gruppo 147 tolto dopo la build (fatto).** La build PC si ferma su `sdk/src/rexglue/commands/test_recompiler.cpp` del 147:
usa `RecompilerConfig::vcmpbfpCr6Exact` e `vupkd3dExact`, che arrivano col **144** (escluso). È una dipendenza 147 -> 144 che
`git apply --check` non vede (file disgiunti). Il 147 (strumenti di test PowerPC, generatore host `rexglue`) non entra
nell'NRO: tolto dall'albero con `git apply -R`; `ninja -n` nella cartella Switch dice "no work to do", quindi **l'NRO
`315669d3…` è identico senza il 147**. Il 147 va con il 144 (U18-2).

Gli hash dei toml, del README e della patch sono in `SHA256SUMS-U18-1.txt`; le istruzioni per la scheda in
`README-U18-1.txt`. U16 (`4424ac8a…`) e U17 (`15da0fff…`, 0 discordanze con `SHA256SUMS-U17.txt`) intatti.

**Varianti (verificate: ogni chiave esiste come cvar nell'ELF; ogni toml differisce dal suo riferimento solo nelle chiavi
elencate; in tutti core 3 = 0, fast draw acceso, 720p fissi; `u18_verify.py`).** Base: il toml di U17-A. Nell'ELF le quattro
copie non ci sono più (nessun simbolo `n_<indirizzo>`).

| Run | Rispetto a | Differenza |
|---|---|---|
| B0 | U17-A | le 11 chiavi nuove come prima dei port (`banjo_renut_stream_redeem`, `_dynamic_texture_rows`, `_keep_unused_targets`, `banjo_deko3d_native_16bit`, `banjo_async_present`, `thread_mutant_release_lockfree`, `filesystem_bounce_guest_only` = false; `filesystem_bounce_piece_mb` = 1; `kernel_deferred_overlapped_delay_ms` = 100; `kernel_overlapped_reset_event`, `xmp_poll_throttle` = false): cambia solo il codice |
| B1 | B0 | 146: pezzi da 4 MB, buffer host diretti, mutant senza mutex |
| B2 | B0 | 145: stream redeem, righe di texture, target tenuti |
| B3 | B2 | 145: texture a 16 bit native |
| B4 | B0 | 145: present asincrono |
| B5 (facoltativa, dopo B0) | B0 | 143: overlapped senza 100 ms, reset dell'evento, XMP throttle |
| V (viste) | B0 | `banjo_deko3d_push_constants_verify`, `banjo_renut_fast_draw_verify`, `_chain_reuse_verify`, `_texture_slot_skip_verify`, `_store_cards_verify`; raffiche 3 ogni ~2 s, 60 |
| N (nuova partita) | B0 | catture ogni 15 s, 60; procedura di `run-protocol.md` "Dalla U18" |

`banjo_deko3d_txq_verify` resta fuori da V: dipinge di magenta e compila varianti a run (cambierebbe l'immagine).

**Verifica sul PC (stesso albero, build PC a 3 job dopo quella Switch; D3D12, chiavi nuove ai default della base).**
- Nuova partita (cartella utente vuota, 560 s, `out/pc-runs/u18-1-newgame`): copie native **0 differenze** (271 raggiunte e
  accese, 241.348 chiamate confrontate; U17 sullo stesso percorso: 4), controlli del codice generato 0, specchio di r1 0,
  inoltri dello stack 0 su 71,9 M; la missione parte come prima (Niko al volante, percorso sul radar, 420 s).
- Salvataggio copiato (240 s, `out/pc-runs/u18-1-save`): copie 0 differenze (267), **flusso della modalità flush: 2,22
  miliardi di controlli dimostrati, 0 hanno dovuto cambiare modo** (le 12 righe cambiate dalla rigenerazione sono giuste),
  codice generato 0, ripristino in due passaggi 26.001 / 0 in nove; la stanza come prima.
- Non dice nulla sui percorsi solo-console (deko3d, file system Switch, present asincrono): quelli li misurano B0-B4.

### 2.27 U18 MAX-A: le run U18-1 lette, la causa dell'alone intermittente, le ottimizzazioni integrate in una build (2026-11-17; PC provato, NRO costruita, nessun run su console)

**Richiesta dell'utente (U18 MAX):** basta run isolate; una build con tutte le ottimizzazioni trasferibili attive
(MAX-A), al massimo 2-3 sessioni su console in tutto (MAX-B solo per una regressione concreta, MAX-C solo se
indispensabile); audio e lampeggiamenti affrontati ora; nessun commit prima della prova su console.

**Le run U18-1 lette (B0-B5, V, N; nessuna nota dell'utente nelle cartelle).** Mediane per scena (`u18_scenes.py`,
lo stesso metodo di U17; il garage manca dove il percorso non lo raggiunge):

| run | cucina fps / GPU ms | strada fps / render thread ms | filmato fps | audio strada | mixer (% di un core) |
|---|---|---|---|---|---|
| U17-A | 24,6 / 39,2 | 9,4 / 53,9 | 25,1 | 98,0 % | 106 |
| U18-B0 | 25,2 / 38,7 | 9,7 / 47,9 | 24,2 | 94,0 % | 107 |
| U18-B1 (146) | 30,7 / 25,2 (2 finestre) | 9,2 / 54,1 | 24,6 | 95,7 % | 110 |
| U18-B2 (145 render) | 24,1 / 40,6 | 8,5 / 55,0 | 24,8 | - | - |
| U18-B3 (+16 bit) | 25,2 / 31,5 | 9,5 / 51,6 | 25,6 | 97,0 % | 98 |
| U18-B4 (async present) | 25,5 / 38,3 | 9,5 / 50,2 | 25,1 | 95,5 % | 106 |
| U18-B5 (143) | 24,0 / 38,3 | 10,0 / 49,4 | 28,0 | 96,4 % | 106 |

Nessun port 142-146 cambia in modo misurabile un fps o l'audio, nessuno peggiora: restano accesi (default della
base). B3 (texture native a 16 bit) contro B2 nelle stesse inquadrature: catture identiche (schermate di caricamento,
porta d'ingresso), nessun colore sbagliato. **Run N (nuova partita): la prima missione parte sulla console**: dopo i
titoli Niko guida il taxi di Roman, radar col percorso giallo, suggerimenti a schermo (catture 23-25, 351-382 s): la
questione aperta da U17 (le quattro copie diverse dall'originale, tolte in U18-1) è chiusa. Strada: tre core saturi (~354 %), thread di gioco a 0x3B ~128 %, mixer
~105 %, render thread di Renut ~59 %, main ~41 %.

**L'alone della lampada: la causa dell'intermittenza.** Run V (le verifiche del renderer accese): nel log 8 righe
(il tetto del log) `banjo_renut_fast_draw_verify: the kept or cached pipeline key differs from the one made`, a 48 s,
subito dopo un ripristino EDRAM della depth, su draw di sola depth/stencil (VS `A97B1F043F720396`, `22F0AB2B742F2053`).
E con la correzione della verifica (V usa la chiave rifatta) la zona dentro il disco della lampada resta **95,4 in ogni
cattura per 30 s a camera ferma** (69-98 s, tre catture a raffica ogni 2 s), dove U17-G0 senza verifica alternava
60,8 / 78,8 dentro la stessa raffica. Nel codice: `MakeKey` mette nei bit 4-31 di `PipelineKey::reserved` il bit gamma
e il bias d'esponente di ogni target (regola di Halo 3, accesa), ma `RenderStateIdLocked` (il numero con cui il
percorso veloce trova la pipeline in `FastPipeline` e decide di tenere quella del draw prima, `key_rs_epoch`) firma i
target solo per formato host e campioni: due draw con lo stesso stato su target dello stesso formato ma bias diverso
(GTA IV: i target HDR hanno bias -1, 925 = 63; 111 delle 245 coppie di shader della lista V esistono con 925 = 0 e 63)
condividono il numero, e il secondo disegna con la pipeline del primo (alfa per 2^-1 o no). Secondo buco: un target
riassociato con un altro `ColorInfo` (stessa EDRAM, stesso formato, bias diverso) veniva aggiornato senza far avanzare
l'epoca delle superfici. Solo deko3d (D3D12 non ha il percorso veloce): torna con "solo console".
- **Correzione** (`render/draw.cpp` `RenderStateIdLocked`: `sig[7]` = gli stessi bit gamma/bias di `MakeKey`;
  `render/surfaces.cpp`: epoca delle superfici avanzata quando `color_info_word` cambia davvero; la verifica ora scrive
  il primo byte diverso e i due `reserved`, fino a 32 righe). Da provare sulla console (run L e P).
- **Il disco in sé è un'altra cosa:** sul PC (D3D12, stessa build, stesso salvataggio, camera girata sulla lampada,
  `pc-route1`) attorno alla lampadina c'è un alone morbido, sulla console un disco bianco uniforme a bordo netto (V lo
  mostra fermo). Candidati sul PC (draw additivi ONE/ONE a 1280x720 nel frame della lampada): PS `23C88DA6F953E36B`
  (11 draw), `AEB7F20FE3E5BEA9`, `55C44C1D9AF1FE91`, `458818340E2283DE`, `FD19EBDD8F6E207A`, `8E604F2EC70ED37A`. Due meccanismi solo console
  mai riprovati dopo la correzione della luce spiegherebbero un disco piatto e circa il doppio della luminanza del PC
  (60,8 / 78,8 contro 30-37): la regola di Halo 3 (`halo3_renut_target_gamma_bias`: scrittura codificata gamma nei
  target di formato gamma e alfa per 2^bias, solo SPIR-V; il PC non ha né l'una né l'altra; U9-C2 la provò a luce
  ancora sbagliata) e la saturazione 7e3 di Dishonored (`dishonored_renut_clamp_7e3`: un plateau HDR tagliato a 31,875
  è un disco uniforme). Nessuna build in più: due run facoltative di 2 minuti nella stessa sessione, **L2** (regola di
  Halo 3 spenta) e **L3** (saturazione spenta), ferme sulla lampada. Escluso: l'alpha-to-coverage reso test (port 127:
  nessun draw del frame della lampada ha `a2c`).
  **Indizio forte (lista delle pipeline di V):** GTA IV non ha target di formato gamma (la costante 924 non compare mai),
  ma gli shader additivi delle luci candidati (`23C88DA6…`, `AEB7F20F…`, `2673E2AF…`, ONE/ONE) esistono in due varianti:
  per il target HDR con bias -1 (`925=63`, nessuna saturazione) e per un target con bias 0 **con la saturazione 7e3**
  (`990=1`, uscita tagliata a 31,875). Una luce forte tagliata a 31,875 è un plateau piatto, cioè un disco uniforme dal
  bordo netto; il PC (nessuna saturazione) disegna la sfumatura. E il buco del bias faceva scambiare al percorso veloce
  proprio queste due pipeline: disco presente / assente, i "due stati" di U17. Con la correzione ogni draw tiene la sua
  variante: L dice se il disco resta (luce disegnata nel target con bias 0) o sparisce; L3 prova la saturazione da sola.

**Il mixer (thread a 0x2D, 100-110 % di un core: il limite dell'audio).** Campioni PC del thread in B0 (5.295):
router `8216D7E0` 28,7 % (62 % di esso nel ciclo del nativo XAudio di U10, già NEON a 16 campioni per passo: niente da
togliere), `828C6740` 15,5 %, `828C2D28` 14,1 %, `828C5A58` 12,1 %, `82185E38` 9,1 % (PCM -> float già vettoriale),
`828C7848` 4,4 %. Dal PowerPC:
- `sub_828C6740` è un **riverbero tipo Freeverb**: 8 comb (1116 1188 1276 1356 1424 1491 1556 1616) e 4 allpass
  (556 440 340 224), quattro campioni per vettore VMX, la ricorsione dello smorzamento come prodotti 4x4
  (`vmsum4fp128`). Nella copia, per comb e per passo: due tabelle di puntatori lette con byte-swap, l'indice letto due
  volte, un `divwu` per l'anello, lo stato riletto dallo stack, una card per store: ~80 istruzioni attorno a ~25 di
  aritmetica.
- `sub_828C5A58` è un **limiter con look-ahead**: per ognuno dei 256 campioni il picco |x| su una finestra di 21
  campioni (20 di storia + il canale) con un ciclo `fabs` / `fsubs` / `fsel` (ogni `fsel` = conversione single->double,
  test intero dei bit, due select: ~300 istruzioni a campione), poi inviluppo, guadagno con divisione.
- `sub_828C2D28` sono due biquad in cascata con stato e coefficienti dallo stack a ogni campione: candidato seguente
  (risparmio stimato 4-5 %, la catena `fmadds` resta).

**Nativi audio (bit-exact).** Scritti da script che leggono la copia stessa (`tools/ultimate/gen_reverb_native.py`,
`gen_limiter_native.py` -> `app/src/cpu/hot/gta4_reverb_native.inc`, `gta4_limiter_native.inc`), nelle liste
`hand.txt` / `localized.txt`, chiavi `gta4_hand_reverb`, `gta4_hand_limiter` (accese):
- riverbero: le istruzioni della copia, con i due nidi di cicli (comb, allpass) che tengono tabelle, indici, lunghezze
  e stati nei registri per gli 8 passi, le stesse operazioni simde e `REX_VMADDFP` nello stesso ordine, ogni load e
  store dei buffer nell'ordine originale, l'anello senza divisione salvo indice + 4 >= 2 x lunghezza, ogni registro
  lasciato come l'originale (`exact=` tutti i 33). Torna alle istruzioni originali se una lunghezza è 0 o se una scrittura
  del blocco tocca qualcosa che il blocco legge una volta sola. **PC, guardia densa (240 s dal salvataggio): 4.026
  chiamate confrontate con le istruzioni originali, 0 differenze** (ogni store e i 33 registri).
- limiter: per canale le 276 parole della finestra raccolte una volta (bit di segno tolti), per campione il picco come
  massimo delle parole (per valori non NaN `fsel(m - |x| >= 0, m, |x|)` nell'ordine originale è esattamente il
  massimo, e per single non negativi l'ordine dei valori è quello dei bit); le istruzioni originali per un canale con
  un NaN, per l'ultimo campione di ogni canale (registri come l'originale) e se l'uscita si sovrappone agli ingressi. La
  guardia non può confrontarlo (due chiamate reali del timebase prima del ciclo). **Verifiche:** contatori dei blocchi
  sul PC (`banjo_hot_count`): ciclo dei campioni 18,3 M volte, cicli della finestra solo 71.624 (= l'ultimo campione di
  ogni canale): il percorso veloce gira; modello esatto delle macro generate (`REX_FSUBS`, `GeZeroAnyMode`) contro il
  massimo su 240.000 finestre casuali e di casi limite (denormali, infiniti, zeri, pareggi): **0 differenze**
  (`scratchpad/limiter_check.py`). La chiave `gta4_hand_limiter_verify` (controllo di ogni campione) non ha preso il
  valore dalla riga di comando del PC: da capire, non usata.

**Le altre fonti.**
- **Base `53bc36a3` (port 148-156, 2026-11-17).** 148 (lavoro CPU di Renut): integrato, ma GTA IV **non registra
  command buffer** (B0: 0 draw "replayed", ogni draw è una copia intera del device da ~12 KB, ~280 MB/s): binds senza
  lock e lock del record non si applicano; la metà "semafori su indirizzo" del port è tolta (il nostro
  `thread_semaphore_address_wait` di U9/U10 resta, è quello che ha raddoppiato la cucina). 149 (hand-over della griglia
  dei campioni), 151 (allocatore dei blocchi piccoli, 32 MB), 152 (coda APC senza lock), 153 (registro delle attese,
  diagnostica), 154 (strumenti), 156 (frame contati allo Swap): integrati. 150 (codegen): CAS e fence nelle orderings del
  PowerPC (63 CAS, 0 `isync` nel codice di GTA IV; 9 sync, 48 lwsync, 14 eieio), 154 load double nell'unità vettoriale,
  **cache per sito delle chiamate indirette (CG-08): 14.161 siti** dopo una correzione di `tools/indirect_calls.py`
  (in GTA IV la riga del `bctrl` finisce con `REX_R1_ASSUMED();` e il generatore scrive `ctx.lr = ...` tra il commento
  e la chiamata: nessun sito era mai stato riconosciuto, né per le promozioni né per la cache). 155: le parti 2-3
  integrate; la parte 1 (`blur_renut_target_bias_rgb`, colore scalato per il bias) **spenta per GTA IV** in
  `gta4_switch_defaults.toml`: il PC disegna GTA IV giusto senza alcuna regola di bias e accenderla vuole un pacchetto
  di shader e una cache DKSH nuovi.
- **144** (`spin_hint_yield`, opzione del generatore): accesa in `renut_config.toml`, 12 siti (`db16cyc` di XAudio
  `8216C750`, `82169D90` e di D3D `829CFE80`, `829DD928`); forme NEON di NORMSHORT2 in 9 siti. **147** (strumenti di
  test PowerPC): integrato, fuori dall'NRO.
- **FINAL FANTASY XIII (`recomp-nx/fnxf_xiii` `c88f574`):** 30 fps a clock standard con build O3 + LTO + PGO "batched"
  (codice generato in file da ~1 MB, build propria). Conversione dei vertici su GPU (`stream_convert_cs`: per GTA IV
  `ConvertVertices` è l'1,7 % del render thread), lock nativi, FSR3: nessuno è il collo di bottiglia misurato qui.
- **LibertyRecomp:** lo studio dei use-after-free (`uaf-handle-investigation`) riguarda il suo eseguibile (gli indirizzi
  di `grcDraw` cadono, nel nostro, nel codice audio) e un crash che le nostre run non mostrano: niente da portare.
- **Copie macchina:** 9 nuove dai profili U18-1 (almeno 50 campioni e 40 istruzioni, nessuna delle escluse prima); la
  guardia del PC ne ha trovate 2 diverse dall'originale in un NaN di una lane (`828DC908`, `828DDFD8`: già escluse in
  U12, i commenti non le avevano fermate): tolte; e `82890AF0` (180 operazioni float vettoriali, la stessa famiglia) non gira
  mai sul PC dal salvataggio (`banjo_hot_count`): non verificabile, tolta (regola di U10). Restano 6 (`828C1028`,
  `828D6118`, `821CAD90`, `821DD718`, `8239D498`, `827312B0`), tutte raggiunte e confrontate dalla guardia.

**Escluso, con il motivo:** C1/C2 (mixer sul core 3: le cause, il core condiviso col sistema e la fame del file system,
non sono risolte); fast draw spento (costa l'11 % in strada; ora corretto); prefetch del mix (nessuna implementazione
diversa); `audio_switch_pump_pairs` / `_frames` (latenza +5,3 ms per frame e parametri aggiornati la metà delle volte:
non è una scelta del DSP ma cambia il tempo, non presa senza l'utente); `BLUR_FPCR_ONE_MODE` (non bit-exact sui
denormali scalari); `thread_event_spin` (rumore in Blur); `BLUR_LOCALIZE_ABI_TRUST` (quasi esatto); **LTO e PGO**: la
LTO è una build intera in una cartella propria (FFXIII, la base) con un picco di memoria al link non misurato su 16 GB
a 1 job (~1,5-2 h in più), e la PGO vuole prima una sessione su console con l'NRO strumentata: entrambe proposte come
passo dopo MAX-A, non dentro.

**Priorità XAudio:** `gta4_xaudio_thread_priority = 45` (0x2D) ora nella NRO (ogni run da U11 la aveva nel toml); il
nuovo mixer costa meno CPU, quindi la stessa priorità ruba meno ai thread di gioco.

**Verifiche sul PC (D3D12, stesso salvataggio di U16-U18).** Guardia densa 240 s (`banjo_hot_guard_calls` 4000,
periodo 64), due run: 274 copie su 296 raggiunte, 1,40 M chiamate confrontate, 0 differenze (guard1); sull'albero
finale meno `82890AF0` 272 su 294, 1,42 M, 0 differenze, riverbero 4.028 confronti, 0 differenze (guard2); controlli del codice generato
(costanti piegate, r1 mirror, stack forward) 0 differenze. Run cvt1: le 2 copie NaN trovate (sopra). Percorso: la
lampada, la porta, il corridoio disegnati come prima (catture `pc-route1`, `pc-route2`); il resto del percorso fino
alla strada non si fa alla cieca con `input_script` (Niko si ferma contro un mobile del corridoio): lo fa la run L.

**Build e pacchetto (fatto, controllato).** Rigenerazione completa quattro volte (le ultime due per togliere le copie
trovate dalla guardia): l'ultima 935 file, 37.067 funzioni, hash combinato `7a3bef565100e1ea`
(`ultimate-manifests/gen-u18-max.json`). Build PC (clang) e Switch (GCC devkitA64, 1 job, 224 passi, 74 min) senza
errori. **NRO `4485221b…` (67.102.917 byte), stamp `168baa62b651-dirty+u18max-a`.** Controllato nell'ELF, non solo nei
sorgenti: i due nativi (`n_rage_reverb::Native`, `n_rage_limiter::Native`), la sezione `rex_site_cache` (116.200 byte,
14.525 parole), 13 chiamate a `rex_spin_hint`, `blur::small_alloc`, `rex::system::waitreg` (port 153), le impostazioni
incorporate (`gta4_xaudio_thread_priority = 45`, `blur_renut_target_bias_rgb = false`). Pacchetto
`out/console-runs/u18max-a` (`tools/ultimate/package_switch.py --allow-dirty`, manifest `switch-u18max-a.json`): NRO,
ELF, map, toml A / L / P / L2 / L3, cache U10 `438346a3…` (la stessa di U18-1), `README-U18MAX-A.txt`, la patch delle
modifiche tracciate e lo zip dei 56 file nuovi (gli script `.py` nuovi sono ignorati da git: vanno aggiunti con `-f`),
`SHA256SUMS-U18MAX-A.txt`, i log delle build e della guardia. Niente committato (richiesta dell'utente: dopo la prova
su console).

### 2.28 U18 MAX-A letto sulla console; il disco della lampada spiegato; MAX-B: porte 157-181, clamp 7e3 spento per GTA IV, barriera delle texture nei vertex shader, nativo del filtro audio (2026-11-18)

**Identità.** NRO SHA-256 `4485221B57BF…`, stamp `168baa62b651-dirty+u18max-a` in tutte e cinque le run (A, L, L2, L3, P);
nel log di ogni run: `gta4_hand_limiter: ON`, `gta4_hand_reverb: ON`, `[window blocks]` e `[code blocks]` (memoria del
gioco e codice dell'NRO su blocchi da 2 MB), 0 compilazioni in A e P, 0 draw in attesa di compilazione in tutte.

**Misure (A e P contro U18-B0 e U17-A; `u18_scenes.py`, `mixcost.py`, `street_agg.py`, `cores.py`).** La run A è più
corta del percorso (~180 s: cucina ~10 s, strada, filmato, niente garage): la cucina si legge da P.

| scena | U17-A | U18-B0 | MAX-A | MAX-P |
|---|---|---|---|---|
| cucina fps / GPU ms | 24,6 / 39,2 | 25,2 / 38,7 | (10 s) | 25,8 / 37,6 |
| strada fps (p10) / thread di rendering ms / draw | 9,4 (7,8) / 53,9 / 4747 | 9,7 (7,9) / 47,9 / 4038 | 9,3 (7,4) / 59,0 / 4616 | 9,3 (7,7) / 62,5 / 4475 |
| filmato fps | 25,1 | 24,2 | - | 24,5 |
| CPU strada (400 % = 4 core) | 350 % | 354 % | 346 % | 349 % |

Strada: nessun guadagno misurabile di fps (differenze dentro lo scarto fra percorsi). I core 0-2 restano saturi (mask
0x7 + render thread sul core 1: 338 % su 300 % disponibili misurati con le scale del profiler); il thread di rendering
dell'host costa ~12,6 µs per draw contro 11,9 di B0 (+6 %), da guardare con il batch dell'host (porta 177).

**Audio (il risultato reale dei due nativi).** Mixer (thread a 0x2D): 4,37 ms per blocco in A e 4,44 in P contro 5,12
di U18-B0 e 4,94 di U17-A (-15 %); CPU del mixer 92-96 % di un core contro 102-106 %; attese nel kernel 14-15 % contro 5-7
%; consegna 100 % dei blocchi (A: 0-6 richieste senza dati ogni 10 s) contro 92,8 % di B0. Per funzione, strada, % di un
core: limiter 12,5 -> 2,5 (nativo), riverbero 15,7 -> 12,7 (nativo: le combs sono limitate dalla memoria), router
`8216D7E0` 30,5 -> 31,6, filtro `828C2D28` 16,0 -> 16,3, `82185E38` 10,0 -> 10,1, `828C7848` 4,9 -> 5,6. Il router è il mix
XAudio già nativo e vettoriale (16 float per iterazione; i campioni cadono su `ldp`/`stp`: limite di memoria, non di
calcolo). Il thread audio RAGE (`828A0A38`, `8289F7A8`) sale da 19,8 a 28,1 % di un core: con la consegna al 100 % lavora a
ogni richiesta. Stima del ciclo precedente: 105 -> 85 %; reale: 102 -> 92-96 %.

**Il disco della lampada: causa.** Le catture di L (clamp acceso) mostrano un disco chiaro dal bordo netto attorno alla
lampada che compare e sparisce fra catture a 120 ms (zona 70-93 di luma nella stessa raffica); L2 (regola halo3 spenta)
uguale (4,7-4,9 % dei pixel cambiano a camera ferma): halo3 non è la causa. L3 (`dishonored_renut_clamp_7e3 = false`):
nessun disco in nessuna cattura, zona della lampada 110,3-110,4 per 16 catture in 16 s, la cucina come sul PC. Il clamp
(porta 106 di dishonored-nx) aggiunge a ogni pixel shader che scrive un target 7e3 con bias 0 `clamp(rgb, 0, 31.875)`,
`clamp(a, 0, 1)`, NaN -> 0 (costante 990): 223 pipeline di GTA IV lo ricevono. Nessuna pipeline esiste sia con sia senza
la costante (`clamp_pairs.py` sulle varianti di L): la classificazione del target è stabile, quindi l'alternanza non
viene da una chiave che cambia ma dal clamp applicato ai valori della luce. Il D3D12 del PC (riferimento di ogni correzione
dell'illuminazione dalla U12) non ha mai avuto il clamp e disegna un alone morbido. Quale valore la luce scrive oltre il
campo (alfa sopra 1 o colore sopra 31,875 in una miscela SRC_ALPHA) resta da vedere su una cattura GPU: non serve per la
correzione, che è per GTA IV soltanto (`gta4_switch_defaults.toml`); gli altri giochi tengono il default del base.

**Bicchieri e luci esterne (ancora aperti, separati dall'alone).** L3, raffiche a camera ferma: il bicchiere alto e quello
piccolo sul tavolo cambiano riflesso/contorno fra catture a 120 ms (89-101 s); in strada (141 s) solo personaggi animati
più due piccole sorgenti (la lampada di una porta, un'insegna al neon) che si accendono e spengono; le ombre del carretto
non cambiano. 124 dei 666 vertex shader di GTA IV campionano texture, 34 nelle run MAX-A, alcuni con cinque prelievi di
profondità attorno a un punto (`vs_44297F746E7C4B27`: il test di visibilità delle luci). Su deko3d una texture resa
leggibile passa per una barriera Fragments che non trattiene il lavoro dei vertici (porta 161 della base, da fnxf_xiii:
il suo flare lampeggiava allo stesso modo). IPOTESI per le luci esterne, da verificare in MAX-B (run L contro V0). Per i
bicchieri nessuna causa trovata: candidati l'ordine/composizione dei trasparenti e il riflesso; la run L di MAX-B li
inquadra 30 s fermi.

**MAX-B: contenuto.** Porte 157-181 della base (commit `956824b9`, `2f5cd495`, `66bb4eb6`, `ult_port_sync.py`, 0
conflitti; `device.cpp` e `katamari_keys.cpp` adattati a mano: la contabilità del frame dopo il present nella forma di
GTA IV, il censimento del batch dell'host nelle attese della fence, il limite dei fotogrammi letto dal menu di gioco),
`dishonored_renut_clamp_7e3 = false` e `banjo_deko3d_vertex_texture_barrier = true` in `gta4_switch_defaults.toml`, il
nativo del filtro `gta4_hand_biquad` (`tools/ultimate/gen_biquad_native.py` -> `app/src/cpu/hot/gta4_biquad_native.inc`):
il ciclo della copia eseguito una seconda volta, davanti, con i 18 slot dello stack 96-176(r1) (coefficienti e stati)
in variabili locali; le stesse istruzioni nello stesso ordine, i campioni letti e scritti in memoria guest come
nell'originale, gli stati dei filtri accesi riscritti sullo stack all'uscita. Rinuncia (le istruzioni originali) se i
campioni toccati (`r11-8 .. r11+1016`) si sovrappongono agli slot (`r1+96 .. r1+180`) o con la chiave spenta.

**MAX-B: verifiche sul PC (2026-11-18).**
- *Prova strutturale.* Tolto il blocco inserito, il nativo è la copia riga per riga (il generatore la compone come
  `righe[:e1] + blocco + righe[e1:]`); il blocco non ha altre uscite (un `goto` fuori dalla regione rinominata non
  compilerebbe); la seconda entrata del ciclo (`loc_F0000002`) usa sempre le istruzioni originali. Il `.inc` compilato è
  l'uscita attuale del generatore sulla copia attuale (rigenerato e confrontato).
- *Test di sovrapposizione* (`scratchpad/biquad_overlap_check.py`): la condizione C++ (aritmetica a 64 bit) contro
  l'intersezione degli intervalli: 0 discordanze su 2.013.000 casi (limiti, casuali, vicini) e 0 su 2.200 distanze
  controllate byte per byte. I limiti sono quelli degli accessi del ciclo (offset -8..+20 per 32 passate da 32 byte).
- *Guardia densa* (`banjo_hot_guard_calls` 4000, periodo 64, 240 s dal salvataggio di U16): **guard B1** 274 copie
  raggiunte, 1.390.395 chiamate confrontate, **0 differenze**; `sub_828C2D28` 4.643 confronti con le istruzioni
  originali, 0 differenze; **guard B2** (binario finale) 273 copie, 1.389.579 confronti, 0 differenze, `sub_828C2D28`
  4.667, riverbero 4.024, 0 differenze; controlli del codice generato (costanti piegate, mirror di r1, 5,9 M stack
  forward) 0 differenze.
- *Il nativo gira davvero* (run di conteggio, `banjo_hot_count`, 180 s): il blocco nativo conta con `s.Hand` (nullo
  nelle forme reale e di guardia) 770.261 esecuzioni (670.093 col solo filtro A, 100.168 col filtro B); il blocco
  d'ingresso del ciclo 770.213 entrate, la seconda entrata 0: **ogni esecuzione del ciclo passa dal nativo**, nessuna
  rinuncia per sovrapposizione. 1.719.234 chiamate in tutto: le altre (filtri spenti) escono prima del ciclo.
- *Chiave spenta* (guard B5off, `gta4_hand_biquad = false` in un `renut.toml` temporaneo accanto all'eseguibile, poi
  rinominato): `GTA IV filter (gta4_hand_biquad): off`, la copia senza nativo 4.225 confronti, 0 differenze (1,34 M in
  tutto, 0). **Dalla riga di comando del PC le chiavi booleane `--chiave=false` e `--no-chiave` non hanno effetto**
  (prova: `--no-gta4_hand_biquad`, `--gta4_hand_biquad=false` e `--log_level=debug` ignorati; `--banjo_hot_count=true`,
  le chiavi intere e le stringhe delle catture prese): difetto della riga di comando dell'SDK (`cvar::Init`, CLI11), già
  visto in MAX-A con `gta4_hand_limiter_verify`; non corretto in questo ciclo. Sulla console le chiavi arrivano dal toml
  (`ApplyTomlTable`), il percorso provato qui.
- *Due correzioni trovate dalle prove:* `kNames` dei nativi a mano non aveva il nome del 27° (la riga "ON" del filtro
  non usciva nel log: spdlog scartava il formato col puntatore nullo); aggiunto `"GTA IV filter"`. Il blocco nativo ora
  chiama `s.Hand(kRageBiquad, filtri)` (il conteggio sopra).
- *Hook adattati e porte:* build PC completa senza errori, 240 s dal salvataggio con catture: la stanza disegnata come
  prima; gli avvisi del log uguali a quelli della guardia di MAX-A (cambiano solo le letture lente, che dipendono dalla
  cache dei file); il ramo "dopo il present" di `device.cpp` ha la forma della base `66bb4eb6`. Il clamp 7e3 e la barriera
  dei vertex shader agiscono solo su deko3d: il PC non li esercita, la prova è la run L / V0 sulla console.

**Il codice generato rielaborato dalla build PC (trovato e corretto in questo ciclo).** `tools/ultimate/build_pc.bat`
configura a ogni chiamata; la configurazione riscrive `rex/version.h` (`REXGLUE_BUILD_TIMESTAMP`), quindi `rexglue.exe`
si ricollega e il passo "Generating recompiled code" (codegen + `tools/post_codegen.py`, stesso comando) riparte. Il
codegen non riscrive nulla ("0 written, 1 module up to date") e `post_codegen` ripassa sull'albero già elaborato: dopo le
porte di MAX-B questo **non è idempotente** (stack_forward 58 load inoltrati invece di 67, barriere dei cicli 14.021
invece di 13.990, flush_hoist 102 invece di 105, `fnm_sign` 1.456 ripristini): 182 file diversi dalla generazione
registrata (`gen-u18max-b.json`). La prima build PC (interrotta per memoria a 203/414) e la seconda l'avevano fatto.
Correzione: l'albero rielaborato salvato in `D:\theft4-backups\generated-u18max-b-pcbuild-rerun`; rigenerazione da
zero (`regenerate.ps1`, manifest `gen-u18max-b-2.json`): 934 file su 935 identici alla generazione delle 09:27 (solo
`codegen.times.json`); poi la build senza riconfigurare (`cmake --build`), con `codegen.stamp` messo da parte
(`codegen.stamp.bak-1430`) perché il passo del generatore riscrivesse i 321 file del codegen da zero e `post_codegen`
girasse su codice nuovo: **931 file identici al manifest**, gli altri 4 sono stamp, dipendenze e tempi. MAX-A non era
stata colpita: il suo backup coincide con `gen-u18-max.json` (935 su 935). La build Switch non ha il passo del generatore
e fissa `version.h` con `SOURCE_DATE_EPOCH`; per il PC la stessa cosa in `build_pc.bat` è il rimedio (prossimo ciclo:
riconfigurare oggi rilancerebbe proprio quel passo).

**Build Switch (non finita).** `tools/ultimate/build_switch.bat everyday`, 1 job, `THEFT4_BUILD_LABEL=u18max-b`: fermata da Claude Code per memoria
scarsa del sistema (non un errore della build) a 166/407, senza errori di compilazione fino lì; gli oggetti restano e la build
riprende incrementale. Nessuna NRO MAX-B ancora: il pacchetto `u18max-b` (run L, A, V0 in quest'ordine) si prepara dopo.

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
| U9 | 2026-11-14 | `c9c511e`, (questo commit) | C1/C2 letti: cessione dei worker efficace sulla console (43-46 % -> ~6 % di un core ciascuno), fps non confrontabili; **regola di Halo 3 smentita** come causa della luce; luminanza riletta e ripristino della depth uguali al PC: la causa è nel lavoro solo deko3d mai spento (GPU round SASRT, 22 passate GLSL nuove). **Thread principale:** 44,6 % delle sue attese nel rilascio dei semafori -> semafori su indirizzo (`thread_semaphore_address_wait`), test host OK. Mixer limitato dal calcolo (80 % CPU). NRO `0012aa01…`, pacchetto `u9` (G0, G1). Nessun run su console. §2.15 |
| U10 | 2026-11-14 | (questo commit) | **baseline tecnica, non giocabile.** 297 copie macchina in place (mixer incluso) dai profili G0/B, nativo del mix XAudio corretto per `vmaddfp` fuso, layout caldo (1.600 funzioni), cache DKSH U10 (9.174 record). Guardia densa sul PC: 4 differenze NaN -> 11 funzioni lasciate generate; insieme finale **0 differenze su 1.389.924 chiamate**. NRO `d24db83b…`. Console U10-A: strada ~10,4 fps (G0 ~10,7), nessuna regressione, nessun guadagno; audio 65,8 %; 12 compilazioni; luce: manca il sole/ambiente. §2.16 |
| U11 | 2026-11-14 | (questo commit) | console: **priorità dei thread XAudio** (0x2D, la richiesta time-critical del gioco) audio 69,1 -> 86,0 % (strada 66 -> 85 %, mixer da 82 a 105 % di un core, -8 % fps), chiave ancora spenta di default; ritmo legato alle consegne scartato (PC); `input_record`; **luce: la scena HDR della console è illuminata dal sole, il buio è nel post-processing**; `pipeline_list.bin` mancante in 5 run su 6: effetto misurato trascurabile. §2.17 |
| U12 | 2026-11-15 | (questo commit) | PC: 3 copie NaN escluse (gruppo 293), la guardia ripete le differenze di prefisso (0 differenze su 4.135.393 chiamate). Console (NRO `d3642faf…`): **prefetch del mix senza effetto misurabile, tolto** (5,32-5,42 ms per blocco, 85,8-87,3 %); fps di M0/M1 non confrontabili (percorsi diversi, M1 senza `pipeline_list.bin`); **post-processing uguale al PC, la scena HDR no** (manca l'ambientale). §2.18 |
| U13 | 2026-11-15 | (questo commit) | Console V-light (NRO `3ae9d456…`): ingressi del passaggio ambientale `97AC30BA…` uguali al PC (costanti, texture, G-buffer). **Causa: il ripristino EDRAM della base lascia lo stencil 0, il gioco ci mette 0xFF**: l'ambientale (EQUAL 1) fallisce, le luci (NOTEQUAL 0xFF) coprono tutta la sagoma; regressione della migrazione rispetto a `6921903`. Sul PC due errori si compensavano (plume D3D12 con ref dello stencil sempre 0). Correzione: stencil ripristinato bit per bit (`gta4_renut_depth_restore_stencil`, accesa) + ref D3D12; PC: senza la correzione la stanza è nera come sulla console, con la correzione uguale al riferimento. Console da provare (`u14`). §2.19 |
| U14 | 2026-11-15 | (questo commit) | Console (NRO `59481f7d…`): **luce ambientale corretta** con il ripristino dello stencil (§2.19); glitch rimasti: alone della lampada (disco bianco a bordo netto, a tratti assente; query di occlusione escluse) e ombre. Prima finestra a 650 draw: 16 fps, **GPU 61,5 ms** (U13 35,5), render thread 13,7 ms; PC: la correzione costa +19 % di GPU. Audio 91,4 % (meno contesa a fps più bassi, non codice). §2.20 |
| U15 | 2026-11-15 | (questo commit) | I port 129-141 della base (`3e6ae026`, `ult_port_sync.py`; 1 conflitto, 3 file a mano; CG-13 spento; `BASE_COMMIT` -> `3e6ae026`). PC: cache XMA 6,5 -> 92 %, guardia 0 differenze. Console (NRO `063dcbb7…`): prestazioni e audio invariati, **thread XMA ~30 -> ~10 % di un core**; profilo GPU per passaggio: il ripristino dello stencil costa ~12,8 ms a ripristino (25,6 ms dei 60,9 in cucina). §2.21 |
| U16 | 2026-11-15 | (con U17, console) | Ripristino dello stencil in due passaggi per la copia del gioco (PC: 1,64 -> 0,83 ms, immagini uguali); rigenerazione con i passaggi di codegen 136/140 (guardia 0 differenze su 3,13 M chiamate); risoluzione dinamica della base pronta (toml D). Build Switch da autorizzare. §2.22 |
| U16, console | 2026-11-16 | (con U17, console) | Console (NRO `4424ac8a…`, A/D/P): fps uguali a U15-A in ogni scena, audio migliore (strada 90,7 -> 95,0 %, filmato 92,0 -> 98,8 %); **il ripristino in due passaggi non è mai partito** (0 su 4.001: `FastDrawLocked` non segna la copia); risoluzione dinamica senza effetto (GPU non proporzionale all'altezza 3D). §2.23 |
| U17 | 2026-11-16 | (vedi U17, console) | Analisi CPU: strada con core 0-2 saturi (~357 % su 300 %), mixer ~105 % di un core, core 3 libero; vetri e fari che si alternano a scena ferma (catture). Nell'albero: copia del gioco sul percorso completo (ripristino veloce su deko3d), `gta4_xaudio_supervisor_core3` (spenta). PC compilato; console da autorizzare. §2.23 |
| U17, build | 2026-11-16 | (vedi U17, console) | Build Switch a 1 job (incrementale: 3 file), NRO `15da0fff…` (`6be295853eed-dirty+u17`, `THEFT4_BUILD_LABEL`), pacchetto `u17` con A, C1, C2 (facoltativa), G0-G3, cache U10. TW2DE-NX esaminato: PGO, LTO, -O3, batched già presenti nella base; trasferibili `vec_neon_forms` e (via la base) i port 142-147 di oggi. Console da eseguire. §2.24 |
| U17, console | 2026-11-16 | (questo commit) | Console (NRO `15da0fff…`, A/C1/C2/G0-G3): **ripristino in due passaggi attivo** (8.001 su 8.001), GPU cucina 59,3 -> 39,2 ms, **cucina 16,6 -> 24,6 fps**, filmato 20,0 -> 25,1; strada e garage invariati (CPU). **C1/C2 respinte**: il mixer sul core 3 riceve meno CPU (audio 80,9 / 72 % in strada); C2 affama anche il file system (letture SD 10 -> 51 ms, filmato caricato in 86 s). G1-G3 non tolgono i lampeggiamenti; l'alone alterna due stati solo sulla console (PC stabile anche a 30 fps). PC, nuova partita: la prima missione parte; 4 copie native diverse dall'originale trovate dalla guardia (correzione preparata). Port 142-147 divisi in gruppi applicabili. §2.25 |
| U18-1, build | 2026-11-16 | (non committato) | Build Switch autorizzata a 1 job (363 passi, 86 min), NRO `315669d3…` (`168baa62b651-dirty+u18-1`): U17 + port 142, 143, 145, 146 + 4 copie native tolte (147 tolto: dipende dal 144, non entra nell'NRO; rigenerazione completa: differenze dall'albero U17 verificate una per una). Pacchetto `u18-1` con B0-B4, B5 facoltativa, V, N; ogni chiave e ogni differenza dei toml verificate. Console da eseguire. §2.26 |
| U18 MAX-A, build | 2026-11-17 | (non committato) | U18-1 letta (port 142-146 neutri; N: **la prima missione parte**; V: causa dell'alone intermittente = bias dei target fuori dal numero dello stato del percorso veloce). NRO `4485221b…` (`168baa62b651-dirty+u18max-a`, 1 job, 74 min): correzione del percorso veloce, nativi bit-exact del riverbero e del limiter (mixer), port 144 e 147-156 (155 colore spento), cache per sito su 14.161 chiamate indirette, 6 copie nuove, XAudio 0x2D incorporata. PC: guardia 0 differenze (1,42 M), riverbero 4.028 confronti 0 differenze, limiter esatto su 240.000 finestre. Pacchetto `u18max-a` (A, L, P, L2, L3). Console da eseguire. §2.27 |
| U18 MAX-A, console | 2026-11-18 | (questo commit) | Console (NRO `4485221b…`, A/L/P/L2/L3): **audio risolto** (mixer 5,12 -> 4,37 ms per blocco, consegna 92,8 -> 100 %), strada invariata (9,3 fps, core 0-2 saturi), **disco della lampada = clamp 7e3** (L3 senza clamp: nessun disco), nessuna chiave diversa del percorso veloce (P). §2.28 |
| U18 MAX-B, PC | 2026-11-18 | (questo commit) | Porte 157-181 della base, clamp 7e3 spento per GTA IV, nativo del filtro `828C2D28` (`gta4_hand_biquad`): guardia 0 differenze (1,39 M chiamate, filtro 4.667), il nativo esegue ogni passata del ciclo (770.213), chiave spenta -> copia 0 differenze; codice generato rielaborato dalla build PC (post_codegen non idempotente) trovato, rigenerato e controllato col manifest. Build Switch fermata per memoria a 166/407. §2.28 |
