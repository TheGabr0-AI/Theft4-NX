# 18. Migration to ReNut's renderer (ReNut + plume + deko3d)

Started 2026-11-10. Decision (the user's): the recorder of `16-deko3d-devlog.md` stops being the main path. The new base
is **ReNut's Direct3D-level renderer on plume, with deko3d on the Switch**, as ChanseyIsTheBest's ports carry it
(renut-nx, sa2-nx), retargeted to GTA IV's Direct3D. The old renderer stays in the tree as the fallback until the new
one draws a real scene right.

Sources read for this page (clones under `<refs>/`): renut-nx `48ae3cdd` (1.0.2) and sa2-nx `adf3a4e` (1.0.3.1): their
`docs/` (`porting-start-here`, `renut-renderer-port`, `xdk-versions`, sa2-nx's `renut-renderer`; the headings of
`deko3d-renderer-guide`, `cpu-rounds`, `pgo-lto`, `four-core-mode`, `record-time-draw-path`) and
`app/src/renut_engine/gpu` (`d3d/d3d.h`, `d3d/d3d_native.h`, `d3d/hooks/device.cpp` in full; the rest by outline).
**Not read yet:** `render/draw.cpp` (12,287 lines), `render/textures.cpp`, `render/surfaces.cpp`, `guest/command_buffers.cpp`
and the two `enhancements*.md` logs: the rows below that depend on them say so.

## 1. State found

- Branch `sync-clean` at `5c23f97`, equal to `origin/sync-clean`, working tree clean (two untracked items that were
  there before). `out/console-runs/run65-build/` is complete (`COMMIT` = `136ae66`).
- At the first audit `out/console-runs/run65-logs/` did not exist. **It was brought back later and read on 2026-11-10
  (devlog §AO): the characters are already in shards right after their own draws**, in an otherwise empty G-buffer
  (they are drawn before the scenery), and the resolve of the same frame holds the same shards. Nothing removes them
  afterwards: the old renderer hands the rasterizer wrong inputs for those draws (positions or indices), and the
  scenery shows it too (the ship's hull, frame 3964). **Use for the new renderer:** ReNut reads vertex data, indices
  and the skinning constants from the device at the draw; the same shots (ship at the credits, the two close-ups)
  are the first comparison once R3 draws.

## 2. GTA IV's Direct3D against the ones ReNut knows

`default.xex` (European base) links **D3D9 2.0.6274.0** (static library list, XEX2 header `0x000200FF`; all libraries
are 6274, XAPILIB and XHV QFE 3). ReNut was written for Nuts & Bolts (2.0.7645, title update 2.0.7978) and retargeted
by sa2-nx to 2.0.21076. GTA IV's is the oldest of the three: nothing of Banjo's or SA2's private layout is assumed.

The library is the same code, found in the European base at 0x829C7000-0x829E4600 (`tools/d3d_census.py`: 560
functions, 79 named). No reference image of N&B or SA2 is on this PC, so sa2-nx's shape matcher (`tools/d3d_match.py`)
could not be used; the functions below were identified by reading the European base's own code against the behaviour
ReNut's hooks document.

**Device layout** (`tools/renut-gta4/gta4_d3d.h`; `tests/renut/test_gta4_d3d.py` reads every value again from the
generated listing: 65 instructions in 21 functions):

| Part of the device | GTA IV (2.0.6274) | N&B (2.0.7645) | SA2 (2.0.21076) |
|---|---|---|---|
| Size, alignment | 22,400, 128 | 24,064 | 24,704 |
| Public part: dirty masks, ring pointers (+0x30/34/38), call tables, constants (+0x480), register packets | N&B's | - | N&B's |
| Fence completion, fence tag, flags (10896, 10908, 10940-10942) | **N&B's** | 10896, 10908, 10940 | +128 |
| Vertex declaration, index buffer, targets, depth, streams, strides, textures, shaders, tiling surfaces | **N&B's - 8** (11812, 12428, 12432, 12448, 12452, 12520, 12536, 12684/12688, 12720) | 11820, 12436, ... | +376 |
| "KickOff in progress" word, command buffer being recorded | 13216, 13500 (**- 16**) | 13232, 13516 | 13600, 13920 |
| Present fence, critical sections, swap count, side buffer | 14924, 14928/14956, 16544, 16712 (**- 16**) | 14940, 14944/14972, 16560, 16728 | +404-412 |
| Tail: the -1 words, timer frequency, behaviour flags | 21540/21544, 21568, 22280 (**- 20**; the flags are 1,700 lower: the tail is shorter) | 21560/21564, 21588, 23980 | - |

**Functions ReNut hooks** (sa2-nx's set; the state setters and resources stay the game's code):

| Function | GTA IV EU | SA2 | N&B | Read from |
|---|---|---|---|---|
| Direct3D_CreateDevice | `829D0218` | 8254FC10 | 8264DF28 | allocates 22,400 / 128, device type 2 sets flag 0x80 |
| the hardware path (not called) | `829DF3F0` | 8255CDA0 | - | critical sections, VdGlobalDevice (slot 0x82000768), ring, threads |
| InitializePresentationParameters | `829DEF28` | 8255CB20 | - | VdQueryVideoMode, the automatic buffers |
| InitializeApiState | `829DECC0` | 8255C150 | - | both creation paths call it |
| CDevice::KickOff | `829D8518` | 8254EBB8 | 82210AB0 | 49 callers, returns `m_pRing` |
| KickOff's tail (next segment) | `829D8390` | 8254E830 | 822106C8 | the nearest to SA2's KickOffSegment |
| CDevice::BeginRingBig | `829D8730` | 8254EDF8 | 821F7C18 | ring + count * 4 against the limit |
| ring segment allocator | `829D8138` | 8254DA90 | - | (device, dwords), non-zero on success |
| RingBufferDeviceAllocate | `829D74E8` | 8254D4B8 | 8222D3E0 | (device, flags, size*, alignment) |
| CDevice::AllocateInlineData | `829D76F0` | 8254D9A8 | 8222D240 | from the ring or the recorded buffer's allocator |
| CDevice::BlockOnFence | `829D7D40` | 8254D6E8 | 8228F558 | polls through CBlocker |
| CBlocker::Check | `829CFE80` | 8254F400 | 8264DCD0 | the function the old renderer's PC notes saw polled |
| wait for a resource's fence / InsertFence | `829D7F78` / `829D8618` | - | - | not hooked (they call the hooked ones); 829D7F78 takes the fence from the resource's +8 |
| D3DDevice_Swap | `829D5338` | 82558E98 | 82287590 | calls VdSwap; swap count 16544 |
| DrawVertices / DrawIndexedVertices / BeginVertices | `829CCAC0` / `829CCEA8` / `829CC610` | 82565C60 / 82566078 / 82565758 | ... | the native renderer's hooks |
| BeginTiling / EndTiling / EndTiling's tail | `829CD470` / `829CD908` / `829DDF28` | not hooked (SA2 does not tile) | 822788A0 / 82212328 / 82210138 | 829CD470 stores the tile count at 12740; the old notes had it as Clear |
| Resolve | `829CB7C8` | 825656F8 | 8222D6C8 | EndTiling calls it once per tile |
| SetPredication | `829CD300` | - | - | (device, mask); EndTiling calls it around each tile's resolve |
| D3DDevice_InsertCallback | `829D8810` | 8254EEE8 | 821FB688 | writes the callback packet through `sub_829D7E08` |
| CreatePixelShader / CreateVertexShader | `829D1708` / `829D1818` | 8255DBF8 / 8255E658 | ... | the native renderer's hooks |
| D3DDevice_ClearF | `829D49A0` | 82563978 | 822811A0 | BeginTiling calls it with flags 0x3F; no other Clear in the library calls the clear packers (`829CB258` / `829CB6F0` are the resolve's) |
| D3DDevice_CreateVertexDeclaration | `829D1598` | 8255E198 | 8264EA90 | counts 12-byte elements to Stream 0xFF, allocates 56 + 12 n, fills through `829D14A8` |
| D3DResource_Destroy | `829D8D40` | 8254A200 | 822084F0 | only caller D3DResource_Release `829D92E8`; fence wait by resource type (7, 8, 9, 11, 13, 15, 17), then the free |
| EndVertices | `829CCAB0` | (inline) | - | `m_pRing = device + 13428`: the ring end BeginVertices stored |
| Presentation interval, gamma ramp | +13592, +14988 | +14000, +15408 | - | Swap loads +13592 and passes it to SynchronizeToPresentationInterval (`829D5158` / `829D50D8`, Swap's only callees of that kind); Swap passes `device + 14988` (768 words) to the display |
| **Not located yet** | IsFencePending, SetRingBufferParameters (no caller found in the image: GTA IV may not use them; the renderer works without hooks on them) | | | |

**A trap found and closed:** `tools/switch-codegen/direct_calls.py` had turned the calls of every unhooked function into
`__imp__sub_X`. The ring and fence functions had no hook then: **122 calls bypassed any hook on them** (KickOff alone
80). `tools/renut-gta4/weak_calls.py` turns those calls, and only those, back into `sub_X` (an alias of the same
function when no hook is linked). It rewrote two local generated files; it must run again after every code generation
or `direct_calls.py` run, and the PC target's configure step refuses to go on without it. sa2-nx met the same ("the
first run dropped every draw").

## 3. Comparison: what is there, what ReNut has, what it takes

| Component | Now (gta4_native + recorder) | ReNut (renut-nx / sa2-nx) | To do | Risk | How to verify |
|---|---|---|---|---|---|
| Hooks | ~100 hooks at v8 addresses translated to EU (`out/gta4-native-eu/src`, two copies), Direct3D and game functions mixed | 25 Direct3D functions (sa2-nx); the setters stay the game's | the table of section 2; R1 has 16 | low: same library | `test_gta4_d3d.py`; the R1 line on the PC |
| Device creation | the game's hardware path with an emulated-GPU stand-in | the library's no-hardware path + the title device's set-up without the GPU | written for 6274 (`gta4_renut_device.cpp`) | medium: the worker threads and notification routines of 6274's hardware path are left out | boot to the first Swap on the PC |
| GPU state | commands re-encoded by the hooks (`native_front_end`) | the device's own shadow state read at each draw | none beyond the layout | low | R1 logs a draw's bound state through the layout |
| Ring, fences | emulated ring bookkeeping, PM4 not parsed | a sink ring; fences complete on the render thread | R1: complete at the kick (no render thread) | medium: GTA IV's render thread and its fence waits | fence waits in the R1 line; no stall |
| Vertex / index data | geometry store (runs 60-65: hashes, write signals, trust across frames) | stream cache hashed after each kick, or proven by store cards | take ReNut's; our store's write signals are the same idea as store cards and stay as knowledge | medium | ReNut's `*_verify` keys |
| Vertex declarations | from the device (11812) and the shader | from the declaration object (`render/vertex_format.cpp`) | CreateVertexDeclaration to locate; GTA IV's declaration object is 6274's | medium: **not read yet** | a draw's layout against the recorder's for the same draw |
| Textures | native texture cache, lock tracking by hook | read from guest memory by header and fetch constant, content hashes | texture header as N&B's (to check for 6274) | medium | first textured frame against the emulated GPU |
| Render targets, depth | EDRAM-free native targets, placements ported from LRA, resolve handling per case (runs 44-63) | targets by surface object, resolves at the API level, no EDRAM | GTA IV's deferred path (4 targets, depth read back as colour, depth aliases) is the hard part: `render/surfaces.cpp` **not read yet** | **high** | lockstep frames against the emulated GPU |
| Shaders | 1,370 containers extracted; XenosRecomp (our `GTA4_DEKO3D` mode) -> SPIR-V -> GLSL -> DKSH, package s61 (12,060 variants) | reblue-XenosRecomp keyed by the container's XXH3, shader table + GLSL package, uam variants on a background core, DKSH cache from the pipeline list | run GTA IV's containers through Renut's translator; keep our extraction and the uam failure list (60 vertex shaders) | **high**: GTA IV's shaders have never been through this translator | every container translated and compiled offline before any run |
| Draw / clear / resolve | recorder commands, one thread | queued captures, render thread, plume command lists | take as is | medium | PC first |
| Present | front buffer -> deko3d swapchain, gamma | Swap -> render thread -> present shader (gamma ramp from the device) | the ramp's place in 6274's device to locate | low | brightness against the emulated GPU |
| Sync | none (same thread) | render thread, fence completion, callbacks, display sync | take as is | medium | the R1 fence counters, then ReNut's stats |
| Diagnostics | captures, anatomy, A/B, counters, probes (runs 55-65) | 10 s lines, in-run A/B, verify keys, line profiles | keep `guest_profile.py`, the equivalence tests and the run protocol; the recorder's own probes go with the recorder | - | - |
| Audio | `nx_audio_native_routes` (run 64) | not the renderer's | unchanged; next functions as in run 65's notes | - | - |

**What is kept from the old renderer** (knowledge and tools, not its code path): the Direct3D census and its names;
the device offsets its hooks had found (confirmed here); the shader extraction and the list of containers; the
knowledge of GTA IV's frame (deferred G-buffer, depth resolved and read back as colour, reverse Z, stencil
references of the characters, the EDRAM-size clears); the lock / unlock hooks' meaning (write signals); the game-level
hooks that are not Direct3D (aspect, fonts, draw distance, frame limiter), which do not depend on the renderer.
**What becomes useless** with ReNut: the command re-encoding of `native_front_end`, the recorder and its geometry
store, the EDRAM placement emulation, the package's variant scheme. None of it is deleted yet.

## 4. Chansey's optimisations: what transfers

1 = as is, 2 = with adaptation, 3 = not applicable. Every one of them has its own key and a verify mode in the source
ports; here they come with the renderer, off until measured on the console.

| Optimisation | Class | Note |
|---|---|---|
| Render thread (queued draws, pinned core, never-joined threads) | 1 | part of ReNut; GTA IV already has its own render thread, so the core layout needs its own design |
| Device snapshot deltas, chain reuse, capture pool and batches | 1 | sa2-nx's; they act on the device layout, which is what section 2 retargets (the chunk ranges move with the private offsets) |
| Vertex / index copies deduplicated, proven instead of hashed | 2 | ReNut's stream cache; "store cards" need a codegen pass over GTA IV's generated code (`store_cards.py`): our run 64 write signals are the hook-level version |
| CPU-cached upload memory, flushed before the submit | 1 | plume `HOST_CACHED`; our plume copy is sa2-nx's `adf3a4e` |
| Pipeline and resource reuse, pipeline list, DKSH cache | 2 | the cache is built from a run's pipeline list; GTA IV has many more shaders than SA2 (1,370 containers against 103) |
| Wake batching, notify-when-idle, batch clock | 1 | render thread internals |
| Frame pacing on the presentation interval | 2 | sa2-nx's is for a 60 Hz game; GTA IV's interval and its frame limiter hook decide |
| Core layout, four-core mode | 2 | needs the forwarder NPDM and GTA IV's thread census (game, render, audio mixer at 95 % of a core, streaming) |
| Hand-written natives with equivalence tests | 1 | the method of run 64 (`dk_run64_routes.cpp`) is the same |
| Post-codegen CPU passes (non-volatile locals, inline memcalls, ...) | 2 | tools exist for rexglue v0.10; our SDK is v0.9: each pass needs its difference test here |
| LTO, PGO from console sessions, hot function layout | 2 | needs a profile of a session on the new renderer; pointless before it draws right |
| Shader passes (branchless selects, early Z, constants in uniform buffers) | 2 | translator passes written against N&B's and SA2's shader shapes; GTA IV's must go through the difference test |
| Asynchronous shader compilation | 1 | ReNut's prebuild workers and uam on the background core |
| Late input, display sync, eager kicks | 2 | they sit in the SDK's input path and the present; measured useful only once the frame rate is near the cap |
| Tiled drawing drawn once | 2 | **GTA IV tiles every 3D frame** (section 6): N&B's BeginTiling / EndTiling hooks are the model, their device work at 6274's offsets; sa2-nx does not hook them |
| Banjo's native state setters (`d3d_native_state.cpp`) | 3 | written from N&B's disassembly; sa2-nx does not build them either |

## 5. Architecture and order

    GTA IV Direct3D -> hooks at 2.0.6274's functions (tools/renut-gta4) -> ReNut (device snapshot, queued draws)
    -> render thread -> plume -> Vulkan (PC) / deko3d (Switch)

Selection: `gta4_renderer = "xenos"` (default: the emulated GPU, the reference on the PC) or `"renut"`. On the Switch
the old path stays `nx_gpu = deko3d` until R5.

| Step | What | Done when |
|---|---|---|
| **R1** | Direct3D without a GPU on the PC: creation, ring sink, fences, Swap, at GTA IV's addresses and offsets | the game runs frames on it and the 10 s line shows draws with plausible bound state |
| R2 | ReNut's engine imported (sa2-nx's copy at `adf3a4e`, the commit our plume copy is from), building in the PC target against our SDK (v0.9; sa2-nx's is v0.10 + patch) | it links; the render thread starts. Measured for it: the engine is 32,783 lines; of the 18 SDK headers it includes, ours lacks two (`rex/frame_latency.h`, `rex/input/read_hook.h`: sa2-nx has stand-ins for both in `sa2_compat`); it also needs the full plume (ours is the deko3d back end only), reblue-XenosRecomp as a host tool, uam and its Dear ImGui |
| R3 | Shaders: GTA IV's containers through Renut's translator; first frame on the PC | a real scene equal to the emulated GPU's, frame by frame |
| R4 | GTA IV's specifics: deferred targets, depth read-backs, anything the first frames show | intro and play on the PC |
| R5 | Switch build on plume / deko3d, console run at stock clocks | a console log |
| R6 | Profiles and the optimisations of section 4, one at a time, console-measured | - |

The migration is **not done** until R3's condition holds. A build that links is not a renderer that works.

## 6. Step R1: what is in the tree

- `tools/renut-gta4/gta4_d3d.h`: the layout and the hook table (offsets and addresses only).
- `tools/renut-gta4/gta4_renut_device.cpp`: the 16 hooks. ReNut's design (`d3d/hooks/device.cpp`), written for 6274 from
  the European base's code: no line of game code, and ReNut's file is not copied. Each hook runs the original function
  when `gta4_renderer` is not `renut`.
- `tools/renut-gta4/weak_calls.py`: section 2's trap.
- `tools/pc-eu-base`: option `THEFT4_RENUT` (its own configuration: the native renderer's hooks and the counting
  wrappers take the same addresses), `main.cpp` sets no emulated GPU when ReNut is chosen.
- `tests/renut/test_gta4_d3d.py`.

R1 draws nothing: it is the hook layer the renderer will sit on, proven on the running game before 40,000 lines are
put on top of it.

### R1 on the PC (2026-11-10, `out/pc-renut/renut.log`, local)

Build: `out/build/pc-eu-base` configured with `-DTHEFT4_RENUT=ON -DTHEFT4_GTA4_NATIVE=OFF -DTHEFT4_NATIVE_HOOKS=OFF`,
0 errors. `gta4_renderer = xenos` (default): the game starts as before. `--gta4_renderer=renut`:

| Run | What happened | What it took |
|---|---|---|
| 1 | Both devices created (title at 4006BB00, a no-hardware one at 40071300), the loading screens ran 28 s (4.1 M inline draws, a draw's state read through the layout: 1280x720 target, shaders, declaration), then a write to guest address 0 at the first scene draws | the fault was in `sub_829D77D8`, the hardware submission of a command block, reached through `sub_829D8290` because the title device has no ring set-up: hooked (the 16th hook) |
| 2 | No fault in 180 s; after the first scene frame (indexed draws with stride 24 into a depth-only target, draws into 320x180 and 256x256 targets, then the 1280x720 back buffer) no thread made progress | the library's graphics interrupt was not registered: `sub_829DF308` is now called, and the null GPU's 60 Hz interrupt reaches `sub_829D7318` |
| 3 | The same stall, at the same place, with the interrupt registered | the cause below |
| 4 | **No stall.** 70 s of the 3D scene after the loading screens (4,589 tiled frames, 400-680 frames in 10 s with no pacing, up to 1,650 draws a frame), 0 faults | the hook on EndTiling's tail (`sub_829DDF28`) |

**The stall, found and closed (2026-11-10, evening).** GTA IV draws its 3D scene with **predicated tiling**:
`sub_829CD470` is `D3DDevice_BeginTiling` (the old renderer's notes called it Clear; its one caller is the game's
`sub_8249E060`) and `sub_829CD908` is `D3DDevice_EndTiling` (callers `sub_8249FA60`, `sub_828525C0`). EndTiling's tail
(`sub_829DDF28`) closes the segment the tiles' commands were recorded in, hands it to the GPU as a command block
(`sub_829D7C68`: the device's count of pending blocks, +11000, goes up by one) and puts a callback in the ring
(`sub_829D7E08` with `sub_829DDD30`), which on the console wakes Direct3D's worker thread (`sub_829DDC40`, created by
`sub_829DE0C0`): the worker replays the block for each tile and takes the pending block off. R1's ring is a sink, so
nothing took it off, and the next wait for an idle GPU (`sub_829D8798`: `BlockOnFence`, then a loop until +11000 is 0)
never ended: that loop has no kernel call, which is why the log only showed every other thread waiting for the frame.
The three candidates of the first reading were one mechanism: the ring callback (1) is the worker threads' wake-up (2);
the flip queue (3) was not involved.

*The fix is ReNut's own design, not a stand-in for the GPU:* a Direct3D-level renderer draws the tiled frame once at
full size, so ReNut's EndTiling leaves out "the tiles' ring segment, D3D's WorkerDpc callback that replays it per tile,
and its fence's packets" (sa2-nx `d3d/hooks/draw.cpp`). The hook on `sub_829DDF28` does the tail's device work (the
tiling words cleared, the seven segment lists ended, the batch's fence, the out-of-command-memory result) and submits
no block, so no callback and no worker are needed. No callback is faked and nothing is signalled by hand.
`D3DDevice_InsertCallback` is `sub_829D8810` (its only caller is EndTiling); the swap's callback (`sub_829D4E90` with
`sub_829D4CF8`) belongs to the original Swap, which the hook replaces.

Still true: with no pacing the loading screens swap 30,000-70,000 times a second (the presentation interval is the
renderer's work: sa2-nx's `sa2_renut_pacing.cpp`).
What R1 does prove: Direct3D 2.0.6274's creation, ring, fence and Swap functions are the ones named in section 2, the
device runs without a GPU through boot and the loading screens, and the device's bound state reads true through
`gta4_d3d.h`, and the 3D scene's frames run on it. **It does not draw: it is the hook layer, not the renderer.**

## 6b. Step R2: ReNut's renderer in the PC build (2026-11-11)

**What is in the tree.** `thirdparty/renut/` (sa2-nx `adf3a4e`, first committed unchanged as `df941c7`; every change
since is marked `theft4` and guarded by `THEFT4_RENUT_ENGINE` where sa2-nx's behaviour is kept for comparison):

| Part | Change for GTA IV |
|---|---|
| `renut_engine/gpu/d3d/d3d.h`, `d3d_native.h`, `hooks/device.cpp` | every private device offset GTA IV's (2.0.6274; values in `tools/renut-gta4/gta4_d3d.h`, re-read by the test); snapshot 12,704 bytes |
| `hooks/device.cpp` | the 10 device hooks at GTA IV's addresses; creation as step R1 ran it (interrupt handler, prebuilt commands, ResetAllState, the 41 words); flip count kept equal to the swap count; EndTiling's tail (`829DDF28`) and the block submission (`829D77D8`); a 2 s fence-wait report |
| `hooks/draw.cpp` | DrawVertices, DrawIndexedVertices, BeginVertices, ClearF (`829D49A0`), Resolve (`829CB7C8`), **BeginTiling / EndTiling** (`829CD470` / `829CD908`: the tiled frame drawn once at full size, as Renut does for N&B; sa2-nx does not tile) |
| `hooks/shader.cpp`, `hooks/resource.cpp` | GTA IV creates shaders with `CreateVertexShader` / `CreatePixelShader` (`829D1818` / `829D1708`, container at the N&B offsets), declarations with `829D1598`; D3DResource_Destroy `829D8D40` |
| `reblue-XenosRecomp/.../shader_common.h` | **NORMAL1 and TANGENT1** get input locations 23 and 24: 4 of GTA IV's 1,370 containers read them, and DXC refused their partly located inputs |
| `backend/queued.cpp` | the snapshot deltas' unread private words at GTA IV's place (10880-11776) |
| `guest/guest_records.cpp`, the ring sink, the inline vertex scratch | 1 MB each instead of 64 / 4 / 4 MB: GTA IV fills the console's physical memory, and with 64 MB taken its own allocations failed at boot |
| `plume/plume_vulkan.h` | volk under private names on the PC too (the SDK links its own volk) |
| `overlay/app_integration.cpp`, `render/draw.cpp` | this SDK (v0.9) has no `ImGuiDrawer::HasDialogs` and no store cards (GTA IV's code marks none: `banjo_renut_store_cards` off) |

`tools/renut-gta4/`: `renut_gpu.cmake` (the OBJECT library, the translator as a host tool, the DXC of
`tools/XenosRecomp`), `gta4_renut_compat.cpp` + `compat/` (what the engine asks of SA2's application: enhancement keys,
render height, frame count, read hook, store cards off, and **`gta4_renut_capture_s`**: a PNG of the presented frame
every N seconds). `tools/pc-eu-base`: `THEFT4_RENUT_ENGINE` (ON) and `THEFT4_RENUT_SHADER_DUMP_DIR` (the local folder
of extracted containers); `main.cpp` starts the renderer as sa2-nx's application does. Step R1's hook layer stays
(`-DTHEFT4_RENUT_ENGINE=OFF`).

**Shaders.** reblue-XenosRecomp translates GTA IV's 1,370 extracted containers into the shader cache in the build tree:
**1,370 of 1,370** after the two input locations (before: 1,366).

**PC result (D3D12, AMD RX 560X, `--gta4_renderer=renut`):**

| What | Seen |
|---|---|
| Boot, devices, render thread | both devices created, render thread started, no fault, no allocation failure |
| Pacing | 601 game frames in 10 s (sa2-nx's vertical blank pacing at GTA IV's interval) |
| Loading screens | **drawn right**: the loading artwork at 20 s (`renut-0020s`), the "THE COUSINS BELLIC" title at 30 s |
| 3D scene (from ~40 s) | the game runs it (441,352 draws in 10 s, ~735 a frame; render thread 4.4 ms, GPU 9.1 ms a frame) but **the presented frame is black**; three fence waits of more than 2 s while the scene loads |

**Where R2 stops:** the 3D frame does not reach the screen. The next step is the frame's last part: which resolve or
copy writes the texture GTA IV passes to Swap as its front buffer, and why ReNut's present finds it black (R3/R4: the
deferred targets, the depth read-backs, the resolve of a tiled frame).
### R2, second round (2026-11-11): boot made reliable, the black 3D frame narrowed

| Found | How | Change |
|---|---|---|
| **Half of the PC boots stopped** in the first frames: the render thread (or, earlier, the UI thread's overlay present) waited for ever for a GPU slot, with the GPU idle and the device not removed; in the other half the game ran | a stack sampler of the process (dbghelp `StackWalk64` against the link map: `Video::SetGuestDevice` waiting for the video lock held in `WaitSlotLocked`), plume's fence report (completed 8, waiting for 9), the GPU engine counters (0 % during the stop) | the D3D12 swapchain's vertical sync: **3 of 6 boots with it, 6 of 6 without**. `renut_vsync` off by default in this build (frames are paced by `sa2_renut_vblank_pacing`; the console presents through deko3d) |
| plume's D3D12 `waitForCommandFence` waited on an auto-reset event, one signal per wait assumed | reading | it waits for the fence's last signalled value |
| GTA IV registers its graphics interrupt; with no graphics system the SDK ignored it | the boot log | the SDK's null graphics system beside the renderer raises the 60 Hz blank and has no presenter (ReNut presents). The 3D scene then runs at 44 game frames a second instead of 60: two pacings at once, to look at |
| The last resolve of every 3D frame (into the front buffer) passes the rect (0,0)-(-1,-1) | `banjo_renut_diag_resolve_trace` | the Resolve hook clips the rect to the source surface, as the console's resolve does |
| plume's Vulkan back end crashes on this PC's AMD driver (a read of address 0 inside the driver, after the swapchain) | the fault logger | not chased: D3D12 is the PC back end; the console uses deko3d |

Test tools added: `gta4_renut_capture_s` (the presented frame as PNG), the fault logger
(`renut-faults.txt`), a fence-wait report in `BlockOnFence` (2 s) and in plume's D3D12 wait (3 s), and a report of
whether `banjo_renut_diag_show_texture` found its texture.

**The 3D frame is still black, and it is black before the tone mapping.** With `banjo_renut_diag_show_texture`: the
scene's resolved colour (`40079D00`), its 640x360 HDR downsample (`DA1A63E0`) and its resolved depth (`DA1A6320`) are
all exactly 0 (mean 0, maximum 0); the 1x1 average-luminance target (`40226B50`) is saturated. The frame's draws are
all there (`banjo_renut_diag_draw_log_s`: 971 depth-only draws, 588 into the 3-target G-buffer, 478 into the final
1280x720 target, 325 pipelines built). So the lit scene does not reach its targets. Next: read back the G-buffer
targets themselves (not their resolves) right after the G-buffer pass, to tell rasterization (viewport, depth test,
tiled rendering's extent) from the targets' handling (the 2x multisampled G-buffer, the resolves of the three tiles
into one texture).

### R3, first round (2026-11-11, PC only): the black 3D frame found and closed

**Cause (verified):** sa2-nx's draw path reuses the previous draw's render targets and texture bindings when the device
bytes they come from did not change (`sa2_renut_chain_reuse`), and it named those bytes **by number, in SA2's device
layout**: 12816-12835 for the four render targets and the depth surface, 12920-13023 for the 26 texture words. In GTA
IV's device (2.0.6274) the targets are at 12432-12451 and the textures at 12536-12639; 12816-13023 are other words.
So a change of render targets went unseen: after the G-buffer, every pass (the lighting, the down-samples, the
luminance chain, the tone mapping) drew into the target bound before it, and the targets the game resolved and showed
stayed empty. The fix: both ranges by the layout's constants (`render/draw.cpp`); `tests/renut/test_gta4_d3d.py`
(`CHAIN`) now fails on any `ChainChanged` range that names a private device byte by number (it flags the four of
sa2-nx's original file).

**How it was found** (each step a hypothesis tested on the running game, PC, D3D12):

| Hypothesis | Test | Result |
|---|---|---|
| draws clipped (viewport, scissor, tiling) | `[drawlog]` with the host viewport, scissor, guest and host size, tiles | rejected: G-buffer 1280x720 at 2x, 3 tiles, viewport (0.5,0.5) 1280x720 z 0-1; shadow map 1280x1280 z 1-0 (reverse Z) |
| resolves before the draws | resolve and clear lines with the draws since the last change of targets | rejected: 541 G-buffer draws, then EndTiling's resolves |
| vertex constants missing | the vertex shader's constants at the draw | rejected: world matrices with GTA IV's coordinates (c3 = -346, -1370, 17.3, 1) |
| vertex streams wrong | `banjo_renut_diag_stream_dump_vs` | rejected: positions, normals and UVs plausible, byte swap right |
| the G-buffer not written | byte-for-byte readback of the targets before and after each resolve | rejected: RT0 516,209 texels lit, resolved depth 598,438, RT1 normals |
| **the passes after the G-buffer write elsewhere** | `[drawlog]` with the render target the device has bound | **confirmed:** draws with 320x180, 80x45 and 1x1 surfaces bound reached the 1280x720 host target; the final colour surface (`40076EB0`) read back empty after the 724 draws before its resolve |

**Result (PC, D3D12):** the 3D scene reaches the screen. Captures at 50-90 s: the night sky with clouds and lights, the
city with the bridge (`out/pc-renut/cap31`, `cap32`, local). A run with `sa2_renut_chain_reuse_verify` (each reused
target, viewport, key and pipeline compared with one made anew) logged no difference.

**Not right yet:** the image is far darker than the console's (run 65's ship shot at dusk: blue-grey sky and sea; here
the sky is near black and the sea black). The 1x1 average-luminance target was saturated before this fix; exposure
and tone mapping are the next thing to check, then run 65's comparison shots (characters, ship hull).

Also changed in this round:

- **Resolve rect (round 2's clip undone):** round 2 clipped the rect to the guest surface. While tiling the surface is
  one tile (1280x256) and the rects are in screen coordinates, so the second and third tiles' resolves became empty
  ((0,256)-(1280,256)). Now only a negative right or bottom edge (the (0,0)-(-1,-1) of the frame's last resolve) means
  "to the edge"; the renderer clamps to the host target, which covers every tile.
- Tools: `[drawlog]` lines with the viewport, the bound surface and the vertex constants;
  `banjo_renut_diag_resolve_trace` lines with the draws since the last change of targets and a byte-for-byte readback
  of each resolve's source and destination (resolve, then copy to a buffer: no shader, no descriptor);
  `gta4_renut_diag_surfaces` (each surface's header words and target key); `THEFT4_D3D12_DEBUG=1` (environment: the
  D3D12 debug layer in the release build, its messages in `logs/plume_stderr.log`; `1g` adds GPU-based validation).
  With the debug layer on, a run through the 3D scene reported only clears without an optimised clear value.
- **Open, not understood:** ReNut's own `ReadRenderTargetLocked` (compute readback through a bindless slot) gave
  different counts for the same target read twice in a row (48,305 and 13,183 lit texels). It is only used by SA2's
  photo capture; the byte-for-byte readback above does not use it.
- GTA IV rewrites the header of `40076EB0` (the HDR colour surface, EDRAM base 0x2D0) between colour format 3
  (2_10_10_10_FLOAT, drawing) and 0xC (2_10_10_10_FLOAT_AS_16_16_16_16, resolving), several times a frame. The format
  is part of ReNut's target key, so the two are different host targets and the format-3 one, with no other user, is
  retired at each switch. Not yet shown to matter (the lit passes may draw through another surface); to check with the
  exposure.

### R3, second round (2026-11-11, PC only): the lit scene's colour format

**Cause (verified):** ReNut's table of colour target formats (`render/format.cpp`) had no entry for the Xenos formats
0xA (2_10_10_10_AS_10_10_10_10) and 0xC (2_10_10_10_FLOAT_AS_16_16_16_16), so both fell to its default, 8-bit unorm.
GTA IV draws its lit scene into `40076EB0` through 0xC (the draw log's bound surface: colour info `000C02D0` for the
180 draws of the lighting) and switches the same surface's header to 3 (2_10_10_10_FLOAT) several times a frame. So the
HDR scene was clamped to [0, 1] at 8 bits, and each switch to 3 gave a second host target (16-bit float), which was
retired at the switch back. On the console both are the same EDRAM bits; only a resolve reads 0xC wider. Now 0xA maps
as 2 and 0xC as 3: one host target, 16-bit float.

**Result (PC, D3D12, `out/pc-renut/cap33`, local):** the opening credits are readable over the 3D scene, the city's
lights, the pier's numbered posts and the clouds show; mean brightness of the 60-100 s captures 28-38 (was 12).
**Still wrong:** everything below the horizon (the sea) is black, and the buildings show only their lit windows and
lamps: the surfaces the deferred lighting should light are dark. Next.

### R3, third round (2026-11-11, PC only): the lighting's depth and stencil restored from colour

**What the frame does (verified in the draw log, frame 13805):** after the G-buffer (2x, tiled) is resolved and its
EDRAM cleared, GTA IV draws one screen-sized triangle (VS `B9320A..`, PS `B3D105..`) that writes the resolved depth
words back **as colour** into surface `40226810` (8_8_8_8, EDRAM base 0, pitch 1280, 1x), then binds the depth surface
`400007C0` (D24FS8, the same base, pitch and sample count) for the lighting: a full-screen sun/ambient pass with
stencil EQUAL, then light volumes marked in the stencil (colour mask 0) and drawn with stencil NOT EQUAL and depth
LESS. On the console the two surfaces are one memory. ReNut keeps colour and depth in separate host targets, so
`400007C0` held another target's depth (it shares its key with the 1280x1280 shadow map) and the lighting's stencil
tests lit nothing: the dump of the frame (`gta4_renut_diag_dump_dir`, `tools/renut-gta4/raw2png.py`) showed a right
G-buffer (albedo with the ship, Niko and the city; clean normals) and a lit target with only the sky and the far
windows.

**Fix:** a host pass restores a depth target from the 32-bit colour target over the same EDRAM view when that colour
was written after the depth (`render/draw.cpp` `MaybeRestoreDepthLocked`, `shaders/host/depth_restore_ps.hlsl`): the
colour texel is the EDRAM word (R | G << 8 | B << 16 | A << 24, the inverse of ReNut's depth-resolve words), depth from
its upper 24 bits (20e4 float for D24FS8, unorm for D24S8), stencil from its low byte, one pass for the depth and one a
stencil bit (deko3d has no stencil export from a pixel shader). Every target write now has a clock
(`NoteSurfaceWrite`) to order colour and depth writes. And the stencil had to reach the words in the first place:
ReNut's depth resolve wrote it only with Vulkan bindings; plume's D3D12 back end now makes a stencil-plane view
(`X32_TYPELESS_G8X24_UINT`, plane 1, mapped to every component) and the resolve uses it on D3D12 too.

**Result (PC, D3D12, `out/pc-renut/cap36`, local):** the lighting works. The ship's deck and superstructure, the
lamp, and **the characters whole and lit** (Niko and the captain at 70-100 s: faces, hands and clothes in one piece;
run 65's console shots had them in shards). The interior at 100 s has no visible fault. D3D12 debug layer: no errors.
**Still wrong:** in the exterior shots, blocks and stripes of the sky's colour over the geometry (letter-like shapes
at 70 s, a chequer at 80 s); the sea is still black; the image is darker than run 65's. Next.

### R3, fourth round (2026-11-11, PC only): the EDRAM depth tiles' swapped halves

The restore of the third round left blocks and stripes of the sky over the geometry, about 40 pixels wide, at depth
edges. GTA IV's shader that writes the depth words as colour (PS `B3D105989610E354`, read through
reblue-XenosRecomp's HLSL dump) fetches them at x + 40 in the left half of each 80-pixel column and x - 40 in the right
half: in the Xenos EDRAM a depth tile holds its two 40-sample halves the other way round from a colour tile. It also
writes 0x80 (no depth) or 0xFF in the stencil byte. The host restore now reads the colour at the swapped column
(`depth_restore_ps.hlsl` `SwapHalfTile`, in guest pixels, so any render scale).

**Result (PC, D3D12, `out/pc-renut/cap39`, local): the opening scenes are drawn right.** 60 s: the ship in the harbour
at night, its deck, masts, rigging and lamps, the lit skyline; 70 s: the hold seen from above, the characters; 80 s:
Niko and the captain on deck with the credits; 90 s: the pier with its numbered wall. No stripes, no holes, the
characters whole (run 65's console shots had them in shards and the hull with pieces missing).
Open: the overall brightness against a reference of the same frames (the emulated GPU on the PC, or the console);
run 65's ship shot is brighter, but that renderer is not a reference.

**Repeated boots (PC, D3D12, build of `00683a5`):** three boots in a row, 240 s each through the loading screens and
the opening cutscene: 3 of 3 alive at the end, 0 faults, the one error line each is the missing title update
(`update:\update.img`), the depth restore logged in each. Captures every 40 s (`out/pc-renut/boot1-3`, local): the
same scenes drawn right in all three (deck, hold, pier, the docks with cranes at 160 s, the car on the wet quay at
200 s); one capture at 40 s fell on a loading screen.

**Brightness against the reference (PC):** the same build with `gta4_renderer = xenos` (the emulated GPU) captured from
the composed screen at the same times (`out/pc-renut/xenos-shots`, local; fullscreen 1920x1080, partly covered by a
terminal window): the docks with the cranes at 160 s and the deck's "NO SMOKING" wall at 80 s are the same dark night
blue as ReNut's frames (mean 16-25 against 17-27, frames not identical). The dark image is the game's; run 65's
brighter ship shot was the old renderer's. Not frame-exact: a lockstep comparison is still to be made.

## 6c. Step R5: the Switch build (2026-11-11)

**How it is put together** (as sa2-nx's Switch build carries the same renderer, `app/cmake/sa2_renut_gpu.cmake`):

| Part | Switch build |
|---|---|
| Configuration | `tools/switch-game`, its own build folder `out/build/switch-game-renut`: `-DREXGLUE_SWITCH_NO_NVK=ON -DTHEFT4_SWITCH_RENUT=ON` (`build_variants.sh renut`, NRO `theft4-renut.nro`, NACP "Theft4 ReNut"). It refuses `REXGLUE_SWITCH_DEKO3D` and `THEFT4_SWITCH_LRA`: the old renderers' hooks take the same Direct3D functions. The PC and the old Switch variants are unchanged |
| GPU back end | thirdparty/renut's plume with `PLUME_DEKO3D` (banjo-nx's deko3d back end), `PLUME_SWITCH_NO_NVK` (no Vulkan driver linked; plume's Vulkan code compiles and cannot start), this repository's `thirdparty/uam` (localised as `uam_local.o`, as the old build does) |
| Renderer selection | `SA2_SWITCH_NO_NVK` on `banjo_renut_select.cpp` and `backend.cpp`: ReNut on deko3d whatever `gta4_renderer` says (sa2-nx's rule: this NRO has no other renderer) |
| Application side | `main.cpp` (`THEFT4_NX_RENUT`): the SDK's `NxWindowedAppContext` pumped by the main loop, `ConfigureRuntime`, the null graphics system for the 60 Hz vertical blank only (as on the PC), `StartRenderer` before `Runtime::Setup` (the PC's order), `ShutdownRenderer` at exit, the working folder set to `sdmc:/switch/theft4` (ReNut's "next to the executable" files: the SDK has no executable path on Horizon) |
| UI pieces | the SDK builds no `rexui` for a Switch build without Vulkan: `theft4-renut-ui` compiles the graphics-free part (window context, NWindow window, immediate and ImGui drawers, presenter base) and ReNut's own Dear ImGui 1.92.5 (one copy in the NRO) |
| Host shaders | `renut_host_shader` with `BANJO_DEKO3D_HOST_SHADERS`: DXC, SPIRV-Cross, `tools/deko3d/deko3d_glsl.py`, uam for the PC (`out/deko3d_tools`) into `<stem>.deko3d.h` (`tools/deko3d/deko3d_host_shader.py`, sa2-nx's) |
| Game shaders | made on the PC beforehand, `tools/renut-gta4/renut_switch_shaders.ps1` into `out/renut-switch` (game data, local): see below |
| Shader table | `renut_switch_shader_cache.py`: the PC build's table of the 1,370 shaders with the DXIL and SPIR-V arrays empty (deko3d reads neither: `shader_library.cpp`), 496 KB of source instead of 5.9 MB |

**Game shaders for deko3d (PC, 2026-11-11).** The PC build's translator (Renut's XenosRecomp) run again over the
1,370 containers with `BANJO_DEKO3D_SPIRV_DIR`: its shader cache came out byte-identical to the PC build's (same
translator, same inputs), and **1,370 of 1,370** SPIR-V for deko3d (`-DBANJO_DEKO3D`, D3D clip space), 0 failed. Then
SPIRV-Cross, `deko3d_glsl.py` and uam (the default variant of each, as a compile check): **1,370 of 1,370 compiled**
(vertex shaders: median 3,584 bytes of DKSH, at most 39,680; pixel shaders: median 2,048, at most 18,176). banjo-nx's
GLSL passes (`deko3d_glsl_opt.py --check`, all 27 of sa2-nx's default set): 0 shaders kept as they were for a uam
failure. Package: 1,370 shaders, 12 MB of GLSL in 1,242 KB (`deko3d_glsl.bin`, embedded by `deko3d_glsl_switch.cpp`).
A package of the same shaders without the passes is kept beside it (`out/renut-switch/plain/deko3d_glsl.bin`, 1,225
KB): put next to the NRO as `deko3d_glsl.bin` it takes precedence over the built-in one (`banjo_renut_deko3d.cpp`),
which tells a fault of the passes from one of the renderer without a new build.

**Compile errors met and fixed (Switch build, first round):**

| Error | Cause | Fix |
|---|---|---|
| every host shader: `dxc-linux` not runnable (code 126) | MSYS2's CMake on the Windows host leaves `CMAKE_HOST_WIN32` off, so `gpu.cmake` took the Linux DXC | the Windows DXC also for an MSYS / Cygwin host (`gpu.cmake`, marked theft4) |
| `threading_switch.h` not found | sa2-nx's SDK header; this SDK has none | the four helpers the renderer calls (thread priority, pin to a core of the process's mask, current core, unpin = no, as sa2-nx) on libnx's system calls (`compat/me/me_platform_compat.h`, `gta4_renut_compat.cpp`) |
| `std::byteswap` not a member of `std` | the renderer's target had no C++ standard on the Switch (the PC gets C++23 from the SDK's target settings) | `cxx_std_23` on `theft4_renut_gpu` |

After them: the renderer (44 objects), plume with the deko3d back end, uam and the UI pieces compile with 0 errors and
0 warnings; `main.cpp` too.

**The full build and the link (2026-11-11, second round).** The first full build stopped for lack of RAM on the PC at
generated unit 22 of 106 (not a code error). Resumed from the objects already made, one compile at a time
(`THEFT4_BUILD_JOBS=1 tools/switch-game/build_variants.sh out/console-runs/run66-build renut`; the variable lowers
the parallelism for one call, the default stays `-j3` for every variant): the other 81 units compiled with at least
9.9 GB free, one GCC at about 1.2 GB. The link then failed on three symbols sa2-nx's SDK has and this one does not:

| Undefined | Asked by | Fix (`tools/renut-gta4/gta4_renut_compat.cpp`, Switch only) |
|---|---|---|
| `vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion` | plume's Vulkan interface (`plume_vulkan.cpp`), compiled even without the driver | "no driver" entry points (null, `VK_ERROR_INCOMPATIBLE_DRIVER`), the ones `PLUME_SWITCH_NO_NVK` expects from the parent: plume's Vulkan interface stays invalid, deko3d is the back end |
| `RexSetGuestThreadCoreLayout` | `device.cpp`, `sa2_renut_core_layout` at the first Swap | moves no thread (returns 0, the log line says "(0 threads set)"): this SDK places the guest threads itself (`nx_guest_core_mask`); SA2's layout, made for SA2's threads, is not ported |

Result: `out/console-runs/run66-build/theft4-renut.nro` (49,043,653 bytes, SHA-256 `c2dc1581…abbce`, NACP "Theft4
ReNut"), `switch-game-renut.elf` (static PIE, 0 undefined symbols, the GLSL package `g_deko3dGlslPackage` 1,272,760
bytes inside), commit `fead1cd` in the start-up line. Not run on the console yet: run 66 is its first run.

## 6d. Run 66 read, the crash fixed, run 67 (2026-11-11)

**Run 66** (`theft4-renut.nro` `fead1cd`, logs `out/console-runs/66-logs`): two starts, both stopped the same way
14.5 s after the start, before any frame. The start-up ran right up to the device: ReNut chose deko3d ("this build
links no Vulkan driver"), the deko3d device, the uam compile thread, the 3-image 1280x720 swap chain, the first image
heap and shader code blocks, `Runtime::Setup`, the XEX, the guest main thread, `common.rpf` / `xbox360.rpf` /
`audio.rpf` opened, the GLSL package read (1,370 shaders). Then, on the guest main thread, an unhandled write fault
(`esr 0x92000045/46`, translation fault) in `memset`, called from `Sa2CreateDevice` (`renut_engine/gpu/d3d/hooks/device.cpp:1354`,
lr `+0x9AAB0` through `addr2line` on `run66-build/switch-game-renut.elf`), size 1 MB (`x2`), destination
`far = 0x2C08AA0000` = the arena's virtual base (`x26 = 0x2B49000000`) + **`0xBFAA0000`** (`x21`): the ring sink,
a block `records::AllocPhysicalBlock` takes from the 64 KiB physical window. Same values in both starts (`far` minus
`x26` = `0xBFAA0000`). Innocuous lines: "nx_gpu = vulkan, but this build has no Vulkan backend" (this NRO ignores
`nx_gpu`: ReNut always), "runtime-cache NOT EMPTY" (not deleted: the run is still valid for a crash at the device),
sys-clk RUNNING with the clocks read equal to the clocks wanted (the "default profile" case of `run-protocol.md`),
"process MiB used 3185 / heap free 0" (libnx's heap takes the process's memory at the start; deko3d reports 2,933 MB
free in that heap).

**Cause.** `GuestPtr` (`renut_engine/gpu/guest/guest_memory.h`) made host pointers as `virtual_membase + VA` on every
platform but Windows / macOS arm64: right where the guest space is one linear 4 GB view (sa2-nx's SDK on the Switch,
Linux). This SDK's Switch arena is not (`rex/memory/guest_table.h`, memory design A; `core/memory_switch.cpp`
`ReserveGuestArena`): 0x90000000 of virtual space, then 0x20001000 of physical memory, and a 16 MB slice table
maps 0x00-0x8F to the virtual half (0x90-0x9F as the 0x80 mirror), 0x7F, 0xA0-0xBF, 0xC0-0xDF and 0xE0-0xFF (+0x1000)
to the physical half. The plain sum put VA 0xBFAA0000 at base + 0xBFAA0000, past the arena's end (game.log: `0x2b49000000-0x2bf91fffff`, base + 0xB01FFFFF).
The record heap did not fault because it goes through `Memory::TranslateVirtual`, which on NX is the table.

**Fix** (`ebc999f`, marked theft4 in the file): on NX `GuestPtr` returns `rex::memory::GuestToHost(va)`, the
translation of the generated code and of `TranslateVirtual` there (one table load and an add: the cheap translation
of banjo-nx's proposal 11 is kept). `GuestPhysicalPtr` was already right (`physical_membase` is the arena's physical
half), and `HostToGuestVirtual` returns the 0xA0000000 window, so a block's VA and pointer round-trip. Windows and macOS
keep `TranslateVirtual`; the PC path does not change. In `run67-build/switch-game-renut.elf` the memset of
`Sa2CreateDevice` now takes its destination from `rex_guest_table` (`adrp`+`add` = 0x3B71F80, `ldr [table, va >> 24]`,
`add`): 0xBFAA0000 -> physical half + 0x1FAA0000, inside the arena.

**Run 67 build** (`THEFT4_BUILD_JOBS=1 build_variants.sh out/console-runs/run67-build renut`; 16 steps, the generated
code unchanged): `theft4-renut.nro` 49,039,557 bytes, SHA-256 `8d39473b…fdfd69`, NACP "Theft4 ReNut", commit
`ebc999f` in the start-up line; ELF static PIE, 0 undefined symbols; the GLSL package (`BDKG` v1, 1,370 shaders,
1,272,760 bytes) found byte for byte in the NRO. `tests/renut/test_gta4_d3d.py`: OK (XDK part skipped without
`THEFT4_GAME_ROOT`). PC build after the change (`tools/pc-eu-base/build.bat`): 27 steps, 0 errors, the exe linked
again (same size, 53,797,376 bytes: the Windows branch of `GuestPtr` is unchanged); no PC game run repeated for a
change the Windows preprocessor does not reach. **To verify on the console:** the
device created and the first frames; what comes after the device is untested on the Switch (run 66 never got there).

## 6e. Run 67 read, the second stop fixed, run 68 (2026-11-11)

**Run 67** (`ebc999f`, logs `out/console-runs/67-logs`, one start): **further than run 66**. Everything of run 66's
start-up, then past `Direct3D_CreateDevice`: guest devices type 1 at `0x4006BB00` and type 2 at `0x40071300`, the
render thread (priority 0x2C, core 1), the swap chain at 1280x720, the first Swap (`sa2_renut_core_layout ... (0 threads
set)`, as expected with this SDK), the HDR colour surface `40076EB0` (format 10) and the depth surface `400007C0`
(format 33) created, the display frame limiter thread, the first two shaders compiled by uam (stage 0 and 4). Then
`ERROR TERMINATION: std::terminate (uncaught St12system_error: Function not implemented)` at about 15.7 s, `abort`
from `OnTerminate` (`tools/switch-game/main.cpp:1168`, `addr2line` on `run67-build/switch-game-renut.elf`) and
`_exit(1)`. No fault (fault ring 0): not a memory access; run 66's address check (`far` minus the arena base) has
nothing to read.

**Cause.** The thread that logged the first Swap's core-layout line is the one that runs the termination (same
`t...` id): the guest main thread, inside `D3DDevice_Swap_hook` (`renut_engine/gpu/d3d/hooks/device.cpp:916`), which calls
`sa2::render::OnPresented` -> `std::call_once(g_capture_once, StartCaptures)` (`tools/renut-gta4/gta4_renut_compat.cpp`).
With `gta4_renut_capture_s = 15` (run 67's `game.toml`) StartCaptures made the capture thread and called
`std::thread::detach()`. In the ELF, `pthread_detach` tests the weak `__syscall_thread_detach`, which this libnx does
not define (the linker turned the call into a `nop`): it **always returns ENOSYS (88)**, and libstdc++'s `detach()`
throws `std::system_error(ENOSYS)`, uncaught in the hook. `__syscall_thread_create`, `__syscall_thread_join` and the TLS
calls are defined (`pthread_create`, `pthread_join`, `pthread_key_*` work). In the whole run 67 ELF `std::thread::detach`
has one caller, StartCaptures; `pthread_detach` has one more, plume's `Deko3DDevice::initialize()` (the uam thread,
`plume_deko3d.cpp:883`), which ignores the result: there it does nothing, and the thread lives for the session (not
changed; `09-rules.md` §3 lists it). The PC never failed because Windows has detach. The rule broken is `09-rules.md` §3
(no detach on NX), written for libnx after nfsmw-nx; the capture thread (step R3's test tool) was written without it.

**Fix** (`e1d4c18`, `5b15e0a`): the capture thread waits on a condition variable with a stop flag (no `sleep_until`)
and `theft4::renut::StopCaptures()` (`gta4_renut_select.h`) joins it, called before `renut::gpu::ShutdownRenderer()` on
the Switch (`ExitGame`, `tools/switch-game/main.cpp`) and on the PC (`OnShutdown`, `tools/pc-eu-base/main.cpp`). The
`std::thread` lives on the heap and is deleted only after its join: an exit that skips StopCaptures (a crash, the PC's
`std::_Exit` at the window close) never destroys a joinable thread. After the join one line: `gta4_renut_capture_s:
capture thread joined in N ms`. Nothing else changes: the captures, their cvars and the renderer are as before.

**Checked, not causes:** the `(service trap)` lines (936; runs 61 and 65 had 1,050 in play); the heap going from
2,933 to 1,940 MB free (mostly the 768 MiB guest pool reserved after the deko3d device); the missing
`xbox360\audio\config\*` files (the game probes optional names; same in earlier runs); the forwarder's address space
(every host address of the run under 2^39: arena `0x30FDA00000`, module `0x70E6...`, pool `0x6749400000`; the guest
translation works on 64-bit host addresses) and its 4 cores (ReNut pins its threads to cores 1 and 2 after checking the
process's core mask, `RexSwitchPinCurrentThreadToCore`; the pins succeeded: "now on core 1", "core 2").

**Verified on the PC:** Switch build (`THEFT4_BUILD_JOBS=1`, 5 steps): `run68-build/theft4-renut.nro` 49,039,557 bytes,
SHA-256 `108ca380…42969`, commit `5b15e0a` in the start-up line, ELF 0 undefined symbols, `std::thread::detach` with no
caller left (`pthread_detach` only from plume), `StopCaptures` called by `ExitGame`, run 66's table translation still in
`Sa2CreateDevice`, the GLSL package (1,370 shaders) byte for byte in the NRO. `tests/renut/test_gta4_d3d.py` OK (XDK
part skipped). PC build OK; **PC game run** (D3D12, `--gta4_renderer=renut --gta4_renut_capture_s=10`, 55 s, then a
window close): 5-6 PNGs written by the new thread, exit code 0 in about 0.5 s. The join is not exercised on the PC
(`ReXApp::OnClosing` ends in `std::_Exit(0)` before `OnShutdown`, as before). **To verify on the console:** frames after
the first Swap, the capture PNGs, the `capture thread joined` line at the exit with +.

## 6f. Run 68: the first start that reaches gameplay; the deferred frame black; run 69 (2026-11-11)

**Run 68** (`5b15e0a`, logs `out/console-runs/68-logs`, 13 captures): **the first ReNut start on the Switch that gets
through**: logos, the intro's artwork, the opening cutscene with its audio (a little crackly), the skip, gameplay with
the minimap and a camera that answers the right stick; 332 s, 0 faults, exit with + (`capture thread joined in 3 ms`:
run 67's fix verified). On the screen only the **forward** parts of GTA IV's frame: the sky and the moon, the far LOD
buildings' windows, the bridge's lights, the water's reflections, the HUD. Everything the deferred lighting draws is
black: the ground and the near buildings (their silhouettes hide the sky: depth is there), the ship in the cutscene,
Niko and the cars. The black part is not a fixed half of the screen: its edge is the horizon and moves with the camera
(y = 300, 455, 595 in three captures), so not a viewport, scissor or tile rect. The card was not cleaned between runs
67 and 68 (only `game.toml` copied, the user's note); the log shows it changed nothing that matters: the DKSH cache
held the 2 variants run 67 compiled before its stop (`DKSH cache deko3d_shaders.bin: 2 variants`) and `runtime-cache`
was empty (`runtime-cache 0`): a cold start in practice. 7 fps in gameplay (render thread
200 ms a frame, GPU 59 ms; uam compiled 220 variants at 390 ms each in the first minutes): a separate matter.

**What runs as on the PC (same lines in both logs):** the surfaces (the 2x G-buffer `40226670` / `40226700` /
`402267D0` RGBA8 with depth `402265E0` D32F_S8, the 1x words target `40226810`, depth `400007C0`, HDR `40076EB0`
RGBA16F), the depth restore (`400007C0` restored from `40226810`), no `banjo_deko3d_depth24` target (every depth target
D32F_S8, as on the PC), no device fault, no pipeline or shader failure (220 uam compiles, 0 failed). So the restore's
depth reached the frame (the geometry hides the sky) and the **lighting's result** was black.

**The PC reference** (D3D12, `banjo_renut_diag_resolve_trace`, a traced frame at 120 s in the cutscene): GTA IV
resolves the 2x G-buffer in **three tiles** (rows 0-256, 256-512, 512-720), each tile one depth resolve and four
colour resolves of a **region** of a multisampled target. On deko3d two steps of that path are the back end's own:

1. **Region resolves of a multisampled target** (`render/draw.cpp`, the resolve's `multisampled` branch ->
   `plume_deko3d_commands.cpp` `resolveTextureRegion`): a whole-image resolve went to `dkCmdBufResolveImage`, a region
   to `dkCmdBufBlitImage` of the multisampled image ("it averages the samples it covers"). deko3d's source
   (`source/dk_image.cpp`, read 2026-11-11) says otherwise: the blit reads a multisampled image as its grid of samples
   (2x: 2W x H, source coordinates times 2), sets no resolve mode and filters one point of it; only
   `dkCmdBufResolveImage` offsets to the sample block's centre and steps by it, and it takes whole images only. The
   run 68 log counts about 5 such blits a frame. **Fix:** every multisampled colour resolve is drawn
   (`shaders/host/resolve_ms_ps.hlsl`: each pixel the mean of its samples, what D3D12's `ResolveSubresourceRegion` and
   Vulkan's resolve give for unorm and float formats), through `DrawResolveCopy` (same formats and conditions as the
   single-sampled drawn resolve; anything else keeps the old path). cvar `gta4_renut_draw_ms_resolves`: -1 (default)
   deko3d only, 1 every back end (a PC test of the same path), 0 never. One log line at the 1st, 1,000th and
   100,000th drawn multisampled resolve.
2. **Zcull after the restore** (`render/draw.cpp` `MaybeRestoreDepthLocked`): the restore writes the depth from the
   pixel shader (`gl_FragDepth`). Zcull is built from the rasterised depth; deko3d's source handles no shader depth
   write and leaves Zcull to the application (`DkInvalidateFlags_Zcull`; it drops it only when another depth target
   is bound). So it keeps describing the depth before the restore (by this reasoning, not measured), and the light
   volumes test LESS against reverse Z right after: a stale "far" Zcull would reject them. **Fix:** `plume::Deko3DInvalidateZcull` (new, `plume_deko3d.h`)
   after the restore's passes, as plume already does after a copy or an upload into a depth target. Zcull's stencil
   side plays no part: deko3d programs depth-only Zcull (`zetaType = 2`) and fixed stencil criteria.

Which of the two blacked the lighting (or both) the console must show: no PC back end has either. **Verified on the
PC (D3D12):** the build; `--gta4_renut_draw_ms_resolves=1` (every multisampled resolve drawn, the G-buffer tiles
included: `40226670 x2 rect (0,0)-(1280,256)` in the log, 100,000 drawn in 110 s): the opening cutscene lit and whole
as with the native resolve (the deck with the NO SMOKING wall and the credits at 80 s, the cabin with Niko and Roman
lit at 100 s); `tests/renut/test_gta4_d3d.py` OK. Not on the PC: Zcull (D3D12 has none), deko3d's own compile of
`resolve_ms_ps` (uam, at the Switch build).

**Open, not addressed here:** game pixel shaders that write `oDepth` into a target Zcull covers would need the same
drop (the renderer does not know which do); the frame rate (7 fps, render thread bound); the crackly audio (the
audio worker starved at that frame rate: first look at it once the frame is right).

**Run 69** (see `run-index.md`): one start, the capture every 15 s, and `banjo_renut_diag_resolve_trace` (one traced
frame every 30 s: every resolve and target change with the bytes' summary) so that a frame still dark says at which
step it goes black.

## 6g. The old deko3d renderer against ReNut: what to recover (2026-11-11, after run 68)

Read from the devlog (16, runs 35-65), the roadmap (17), sections 3-4 above and the run 68 log; each "ReNut" cell
checked in the tree or the log named. Transfer: **done** (ReNut has it), **direct** (a piece that moves as it is),
**adapt** (the idea, rewritten for ReNut's path), **no** (not needed or not applicable). Priority: P0 the frame right,
P1 stutter and audio, P2 frame rate, P3 only if a run shows the need.

| Feature | Old renderer (where) | ReNut now | Transfer | Priority |
|---|---|---|---|---|
| Deferred lighting: scene depth and stencil for the lights | depth hand-off (run 44: black ground gone in the dark state), packed depth alias (runs 38-40) | colour-to-depth restore with the EDRAM half-tile swap (R3; PC right); on the Switch black in run 68, run 69 fixes the multisampled resolves and Zcull (§6f) | no (ReNut's own, closer to the console) | P0: run 69 |
| Characters and cars whole | never solved (runs 48-65: shards when drawn) | whole on the PC (R3); on the Switch behind the deferred frame | no | P0: run 69 |
| Light-volume and skinned vertex shaders | XenosRecomp state machines: 810 ms draws, cut-scene stall and freeze, 60 skinned shaders uam could not compile; `hlsl_structure.py` structured 183 (runs 46-47) | reblue-XenosRecomp writes structured code: `vs_0872615BD9E7930D` 24,576 B of DKSH (745 KB before), no `switch`/`while (true)`; 1,370 of 1,370 compiled, 0 failed | no (solved by the translator) | done |
| Shader variants ready before play | package s61: 12,060 DKSH variants built offline, no compile on the console | GLSL package, variants compiled by uam on the console: run 68 compiled 295 at 153-552 ms each (about 160 s), the render thread waiting (10 s windows stretched to 16-41 s): **the stutter** | adapt: a DKSH cache built offline from a run's pipeline list (sa2-nx's way), or the console's own `deko3d_shaders.bin` kept between runs once the frame is right | **P1** |
| Vertex outputs pruned | run 55-56 (`nx_dk_vs_prune`) | `vsout` pass, constant 905 per pipeline (deko3d.log variants) | done | - |
| Depth-only draws without pixel shader | run 55 (`nx_dk_depth_only`) | `banjo_gpu_depth_only` (run 68: 23,350 depth-only draws counted) | done | - |
| Geometry kept across frames | geometry store: hashes, write signals from lock/unlock, trusted 8 frames (runs 60-65; store off = -5 % draws/s) | stream cache hashed after each kick; store cards off (GTA IV's generated code has no card pass) | adapt: the lock/unlock write signals as the card source, or `store_cards.py` over the generated code | P2 |
| Submission path | packet ring without allocations (run 46: 1-2 -> 3-5 fps), 64-byte constant diff (run 45) | device snapshot deltas, queued captures, render thread (sa2-nx) | no (other architecture) | - |
| Copies at submit | copies by rewritten page (run 59: +10-13 % draws/s) | snapshot deltas copy only changed chunks (run 68: 123-6,595 bytes a draw) | no | - |
| Exposure (luminance read by the CPU) | texture-lock read-back, EDRAM placements of the exposure chain (runs 54-57) | right on the PC (brightness matched, R3); its 4x targets go through the drawn multisampled resolve from run 69 | check in run 69's captures | P1 |
| Depth clamp for z-fail light volumes | run 43, LRA's policy: changed nothing visible | `depthClipEnabled = true` always (`render/pipeline.cpp:304`); lighting right on the PC without it | adapt only if a run shows cut volumes | P3 |
| Depth bias as LRA | run 55 | the Xenos bias through plume's dynamic depth bias | check against a decal scene | P3 |
| Sync primitives (futex waits, multi-wait, lock-free release) | runs 56 (+11 % draws/s) | in the SDK, on in the ReNut NRO (start-up lines C2/C3/C5) | done | - |
| Audio: XMA kick asynchronous, native mixer routing, deadline priority | runs 49, 64 | in the SDK and the game layer, on in the ReNut NRO (`xma_async_kick`, `native routing calls`, `deadline priority`) | done | - |
| Audio starvation | 53-81 % of real time in play (runs 59-65), CPU bound | run 68: 68 % while loading, 100 % in the cut-scene, 87 % with 7 dry outputs in play (while uam compiled) | follows the stutter fix; then the mixer's own cost (NEON mixer: 0 voice loops counted in run 68, to check) | P1 |
| Game-level hooks: aspect, presentation options (motion blur, film grain), font trace | `theft4_add_lra_hooks` in the deko3d and LRA builds | **not linked** in the ReNut variant (`tools/switch-game/CMakeLists.txt` 373-412) | direct (non-Direct3D files), after checking they need nothing of `gta4_native_hooks.cpp` | P2 |
| Clocks, exit mode D, save fix, stack watch, crash records | runs 21-50 (main.cpp, SDK) | in the ReNut NRO (run 68: exit mode D, clocks as wanted) | done | - |
| Diagnostics | anatomy frames, in-run A/B, GPU probes, stage counters | captures, `[trace]`, 10 s lines, verify keys, in-run A/B (`nx_dk_ab` gone) | adapt the A/B when a speed choice needs it | P3 |

**Speed, the two at standard clocks (single samples, different frames):** the old renderer in play 5.3-5.8 fps,
20.5-22.9 k draws/s, 3,600-4,050 draws a frame, GPU busy 120 ms + idle 50-70 ms a frame (runs 59-62); ReNut run 68 in
play 3-10 fps with the compile stalls, 23.5-27.6 k draws/s in its best windows, 2,700-3,100 draws a frame, GPU 52-65
ms a frame **on a frame whose deferred lighting is black** (not comparable until it is right).

**Order from here:** (1) run 69: the deferred frame; if still dark, its `[trace]` lines name the step. (2) The stutter:
variants ready before play (offline DKSH cache from the run 69 pipeline list; meanwhile `deko3d_shaders.bin` kept between
runs). (3) Audio once the compiles are off the play path; the NEON mixer's state. (4) The game-level hooks into the ReNut
variant. (5) Frame rate, one measured change a run: GPU time a frame first (55-65 ms), then the render thread.

## 7. Licences

- reNut (masterspike52) has no licence file. **Settled on 2026-11-10:** this project's owner confirms
  ChanseyIsTheBest's authorisation to reuse the code of his ports here and masterspike52's consent to the reuse of
  reNut's code. From that day ReNut's files are in the repository: `thirdparty/renut/` (sa2-nx `adf3a4e`, first
  committed unchanged: 475 files compared byte for byte with the clone), with its provenance in
  `thirdparty/renut/README.md` and the notices in `THIRD_PARTY_NOTICES.md`.
- banjo-nx's and sa2-nx's own parts are GPL-3.0, as this repository. plume (MIT), reblue-XenosRecomp (MIT) and uam
  (zlib / MIT / MPL-2.0 patches) come with their licences and notices, as `THIRD_PARTY_NOTICES.md` already does for
  plume.
- Game data (the generated code, shaders, packages, captures) stays local.
