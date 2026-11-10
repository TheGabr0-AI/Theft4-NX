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
| **Not located yet** | Clear / ClearF (829CD470 is not it), IsFencePending, SynchronizeToPresentationInterval (candidate `829D5158`), SetRingBufferParameters, CreateVertexDeclaration, D3DResource_Destroy (candidate `829D8D40`, called only by D3DResource_Release `829D92E8`), the gamma ramp's place in the device | | | needed from R2 on |

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

## 7. Licences

- reNut (masterspike52) has no licence file. Its author permits ChanseyIsTheBest's ports to reuse it; ChanseyIsTheBest
  has told this project's owner that renut-nx's code may be reused here. **To settle before R2 puts ReNut's files in
  this repository:** that permission in writing, and whether it covers masterspike52's part. R1 contains no file of
  ReNut's.
- banjo-nx's and sa2-nx's own parts are GPL-3.0, as this repository. plume (MIT), reblue-XenosRecomp (MIT) and uam
  (zlib / MIT / MPL-2.0 patches) come with their licences and notices, as `THIRD_PARTY_NOTICES.md` already does for
  plume.
- Game data (the generated code, shaders, packages, captures) stays local.
