# 16. GTA IV on deko3d: development log

Goal: the first playable build of GTA IV (European base, no title update) on the Switch with the Liberty
Recompiled renderer (`gta4_native`, LibertyRecompAndroid `7c97c58`, GPL-3.0) drawing through deko3d instead
of NVK. Intermediate target: at least 15 FPS in the city. Final target: 30 FPS at stock clocks with 720p
output (FSR1 from a lower internal resolution). The PM4 renderer (N2/N3, [06](06-renderer-plan.md)) and the
NVK path stay frozen and buildable; deko3d is a separate build (`REXGLUE_SWITCH_DEKO3D`).

Milestones (the order if time runs out: M0, M1, M2):

| | Where | What |
|---|---|---|
| M0 | PC | LRA's `gta4_native` instead of ours, hooks census and translation on the EU base, the 24 missing shaders, intro → loading → free roam at 640x360 + FSR1, side by side with the emulated renderer |
| M1 | PC | deko3d shader chain for every GTA IV variant: XenosRecomp deko3d mode → DXC → SPIR-V → SPIRV-Cross → GLSL (banjo-nx passes, `--check`) → uam → DKSH |
| M2 | one console build | plume's deko3d back end with `REXGLUE_SWITCH_DEKO3D` and `REXGLUE_SWITCH_NO_NVK`, debug and release NROs, a 5 s test pattern, `nx_gpu = deko3d` |
| M3 | after the run | fixes from the logs up to free roam on the console |
| M4 | console | kabba536's rendering profile, measurements at stock clocks and docked |

Each entry: date, what was done, result, decisions. "da verificare" marks a fix not yet proven.

## 2026-10-20: readings

Read in full before writing code (clones in `<refs>/`):

- **nfsmw-nx**, branch `deko3d-implementation` (`266f943`): `docs/HowToDeko3d.md`, `docs/deko3d-devlog.md`.
- **sa2-nx** (`adf3a4e`): `docs/deko3d-renderer-guide.md`, `docs/renut-renderer.md`, `THIRD_PARTY_NOTICES.md`,
  and the inventory of `app/thirdparty/{plume,uam,reblue-XenosRecomp}`, `patches/`, `tools/deko3d_*.py`.

Rules taken from them (each one is applied where the code is written, and listed again there):

| Rule | Source |
|---|---|
| `dkCmdBufBindVtxBuffers` reserves 5 words per buffer and writes 6: bind **one vertex buffer per call** | nfsmw run 2 (GPU method error `[b197:7c4]`) |
| Leave **256 bytes** unused at the end of every command memory piece given to `dkCmdBufAddMemory` | nfsmw `kHolguraComandos` |
| `std::thread::detach()` throws on the Switch (no `pthread_detach` in libnx): join every thread | nfsmw run 1 crash |
| Small resolves the game reads back on the CPU (exposure, 64x64) must be copied back to guest memory | nfsmw run 2 (overexposed 3D) |
| Defer depth clears to the next bind so ZCULL starts the pass valid | nfsmw run 4 |
| `DkBarrier_Full` drains every engine; `DkBarrier_Fragments` only pixel work: Full only around copy-engine transfers | nfsmw runs 3-5 |
| `DkCompareOp` and `DkStencilOp` are the Xenos value + 1; `DkImageSwizzle` is the Vulkan swizzle − 1 | nfsmw P2a |
| Never a negative viewport height (unsigned overflow in deko3d 0.5.0); flip y with `DkSwizzle_NegativeY` or not at all (deko3d clip-space y points up, as Direct3D's: no `-fvk-invert-y`) | HowToDeko3d 6.2 |
| Tile-height bug for level-0 heights of 6-8 GOB rows: `image_tile_size_fix` before every `dkImageLayoutInitialize` | HowToDeko3d 6.3 |
| Clears respect the scissor: full scissor before a clear | HowToDeko3d 6.9 |
| Release libdeko3d aborts with a 2359-xxxx result and no message: first run with `libdeko3dd` and a debug callback that logs, flush the log before every creation, check `dkQueueIsInErrorState` before acquire/submit/present/flush | HowToDeko3d 6.1 |
| uam is not reentrant: one compile thread, 8 MB stack, priority 0x3B (0x3C fails with 0xE001), sleeps when idle; a self-test compile at start | HowToDeko3d 3.3 |
| DKSH cache key = GLSL hash + uam version + patch set + converter revision | HowToDeko3d 3.3 |
| Code memory: shaders aligned to `DK_SHADER_CODE_ALIGNMENT`, last `DK_SHADER_CODE_UNUSABLE_SIZE` bytes unused; `DkInvalidateFlags_Shader` after loading code mid-frame | HowToDeko3d 3.3, 6.11 |
| uam loses a dynamic component index into a UBO `vec4`; integer division by a variable becomes float (warning = bug) | HowToDeko3d 6.12-13 |
| uam patches 6 (deterministic scheduling) and 7 (no dual issue: warp-sized flicker) are required | HowToDeko3d 3.2 |
| GPU timestamps ×1.627 | HowToDeko3d 6.15 |
| Each thread sets its own core mask (setting another running thread's mask can hang) | sa2 guide §7 |
| A depth target smaller than its colour target is a GPU exception on Maxwell | sa2 traps |
| The deko3d swapchain stays 1280x720 unless resized; a second `viOpenDefaultDisplay` fails | sa2 traps |
| Direct calls bypass new hooks until `direct_calls.py` knows them | sa2 §3 (we hit it in [15](15-kabba-port.md) too) |
| The pipeline list and DKSH cache must be `fsync`ed or HOME loses them | sa2 renut-renderer |

### Licences of what may be taken (sa2-nx)

| Part | Licence | Takeable |
|---|---|---|
| `app/thirdparty/plume` incl. `plume_deko3d*.{h,cpp}` (banjo-nx's back end), `patches/plume-banjo-nx-c319b29.patch` | MIT | yes, with credits |
| `app/thirdparty/uam` | zlib (uam), MIT (Mesa files), MPL-2.0 (SwitchWakerHD's patches) | yes, with credits |
| `app/thirdparty/reblue-XenosRecomp` | MIT | yes, with credits |
| `tools/deko3d_*.py`, `tools/early_alpha.py`, `tools/renut_early_alpha.py` | GPL-3.0 (banjo-nx) | yes, with credits |
| `patches/rexglue-sdk-v0.10.0.patch` (`REXGLUE_SWITCH_NO_NVK`, ...) | BSD-3-Clause | yes, with credits |
| `app/src/renut_engine` (Renut) | **no licence file** | **no** |

### Finding: LRA's `gta4_native` is not on plume

The premise was "the LRA renderer on plume, with plume's deko3d back end". Checked at `7c97c58`:
`glue/rexglue-sdk-main/src/graphics/gta4_native` (158 files, 43,553 lines of `.cpp`) includes no plume
header. It calls **Vulkan directly** through ReXGlue's `rex::ui::vulkan` (device, instance, presenter,
submission tracker): 93 distinct Vulkan functions, 587 call sites, among them 35 `vkCmdPipelineBarrier`,
15 `vkCmdBeginRendering` (dynamic rendering), 12 pipeline binds, 12 descriptor set binds, 14 push
constants, 3 `vkCmdDispatch` (compute), 5 query pool resets, `vkCmdResolveImage`, `vkCmdBlitImage`.
plume is in LRA's tree (`thirdparty/plume`) for other code, not for this renderer. Its shaders use the
bindless heaps and `vk::RawBufferLoad` push-constant addresses (`xenos_shader_common.h`, `GTA4_RECOMP`).

Consequence for M2: plume's deko3d back end cannot be plugged under `gta4_native` as it is. Two ways:

1. a deko3d back end behind a thin boundary inside `gta4_native` ("record this draw with this state",
   HowToDeko3d 5.1), with plume's deko3d code as the reference for the deko3d parts it already solves
   (memory, images and their tile fix, swapchain, uam, descriptors);
2. move `gta4_native` onto plume's `RenderInterface` first, then use plume's deko3d back end as it is.

Decision: deferred to M2 (M0 and M1 do not depend on it); see the M2 entry.

### Finding: LRA's shader overrides contain translated game shaders

LRA `7c97c58` has 335 files in `LibertyRecompLib/shader_overrides`. Our repository already tracks 20 of
them (hand-written, imported in `bd7eb4c`). The new ones `forward_fog/*` (284), `sky/*`, `postfx/*` and
`depth_of_field/composite_<hash>.hlsl` are XenosRecomp output of the game's shaders (register code such as
`r6.xyz = r3.zzz * gShadowMatrix(2).zyx + ...`) with edits: game-derived. They stay in a local folder
outside the repository; the build takes the override root as a cache variable.

## 2026-10-20: M0, LRA's `gta4_native` in our tree

What was done:

- `glue/rexglue-sdk-main/src/graphics/gta4_native` and `include/rex/graphics/gta4_native` replaced by
  LRA `7c97c58`'s (158 + 22 files; ours had 125 + 20). `src/graphics/CMakeLists.txt`: the plugin's sources
  as LRA lists them (`core/font_atlas.cpp`, `temporal/{fsr_vulkan,shader_archive,vulkan_scene,
  vulkan_frame_generation}.cpp`; `modern_shader_options.cpp` dropped, as in LRA), and the override root
  as a cache variable. LRA's temporal shader archive (`tools/vulkan/VulkanShaderArchive.cmake`) is not
  built: it only matters with FidelityFX or DLSS, both off here.
- `src/graphics/gta4_metal/temporal/{history,projection,title_boundary}.h` (header-only C++ maths the
  hooks include) copied from LRA.
- The native hooks in `gta4-recomp/src` replaced by LRA's: `gta4_native_hooks.cpp`,
  `gta4_aspect_hooks.{cpp,h}`, `gta4_aspect_policy.h`, `gta4_draw_distance_policy.h`,
  `gta4_fire_escape_trace.inc`, `gta4_font_selection_trace.cpp`, `gta4_frontend_hooks.h`,
  `gta4_frontend_menu_policy.h`, `gta4_gpu_pass_context.h`, `gta4_help_trace_hooks.inc`,
  `gta4_presentation_options.{cpp,h}`, `input/context_touch_{controls,layout}.h`; new:
  `gta4_temporal_hooks.inc`, `input/context_touch_{context,radar,activity_types}.h`. Checked for game
  code: no disassembly-style blocks in any of them.
- **Not taken:** `gta4_draw_distance_guest.inc` (and LRA's other `*_guest.inc`): retail PPC lifted by
  `derive_draw_distance_hooks.py`, i.e. translated game code. Local change on top of LRA's
  `gta4_native_hooks.cpp`:

  ESISTENTE (LRA)
  ```cpp
  #include "gta4_draw_distance_guest.inc"
  ```
  NUOVO
  ```cpp
  #if __has_include("gta4_draw_distance_guest.inc")
  #include "gta4_draw_distance_guest.inc"
  #endif
  ```
  Without it `sub_821D6260` / `sub_821D8CD8` are the game's own (stock 300-unit remap boundary): the
  draw-distance lift is off, which M4 wants anyway until the European hooks exist.
- `tools/gta4_shader_override_compiler.py`: LRA's (exported entry point `shaderMain` for GLSL overrides).
- `tools/pc-eu-base/CMakeLists.txt`: `THEFT4_GTA4_NATIVE_OVERRIDE_ROOT` (local copy of LRA's overrides);
  `native_hooks.cpp`: `pc_shader_dump_dir` writes each new shader container (`<hash>.<vs|ps>.bin`, game
  data, outside the repository) for XenosRecomp.
- Tools: `glslangValidator` built from the SDK's glslang (`out/build/glslang`), needed by LRA's GLSL
  override `motion_blur/gta_composite_mb_e2.glsl`.

### Census and translation on the EU base (LRA's hooks)

`tools/gta4_native_eu_census.py` (`gta4_temporal_hooks.inc` added to the native group) →
`out/gta4-native-census-lra/census.tsv`; `tools/gta4_native_eu_translate.py --census …` →
`out/gta4-native-eu/src`.

| | Hooks | found | not found |
|---|---|---|---|
| Direct3D library (all = v8 − 0x714A0) | 37 | 37 | 0 |
| game code (drawing, LOD, tracing) | 98 | 94 | 4 |
| **total** | **135** (ours: 129) | **131** | **4** |
| calls to other functions | 19 | 19 | 0 |

LRA adds six hooks over ours, all found: `822C7A08`→`822F14B0`, `822C7C00`→`822F16A8`,
`822C89D0`→`822F2478`, `822CC388`→`822F5DE8` (timecycle far clip), `822CC940`→`822F63A0`,
`828C4578`→`82855890`.

The four not found, and what missing them does (each hook then never runs; the game's code does):

| v8 | State | Hook | Effect without it |
|---|---|---|---|
| `8247BED8` | da trovare | TV script draw (`gta4-tv-script-draw`) | TV trace lines and the diagnostic TV movie sequence number only; the TV itself is drawn by the game. No visible effect |
| `828DFF00` | ambiguous | offscreen shader-select boundary log | a log line only |
| `821F32F8` | ambiguous | help box background colour trace | a log line only |
| `82223CF8` | ambiguous | help box draw: aspect role `kHelp` and the touch help scope | the help box is drawn with the default aspect role (no widescreen correction of its position); touch is unused here |

Globals not found in the native group: `824F6464`/`824F6470` (return-address checks of two traces: the
trace takes its other path), `82BAFA84` (a value in a bulb trace line), `828BF4B0` (a label). None
changes drawing.

### What LRA's renderer needed from LRA's SDK (ported by hand: no common ancestor)

Only what the plugin and its hooks call. ESISTENTE → NUOVO, by file:

- `include/rex/ui/vulkan/device.h`: ours → LRA's (command counters, performance and pipeline statistics
  flags, DLSS flags, `maxColorAttachments`, `shaderInt16`/`shaderFloat16`/storage-image features,
  `VK_AMD_buffer_marker`, `WaitForFences`, `queue_family_native_offscreen`, `QueueFamily::queue_flags`)
  plus our `variableMultisampleRate` put back. **Removed:** the Switch `static_assert(sizeof(Properties)
  == 568)` (the size changes with LRA's fields; to check in the NVK build, da verificare).
- `src/ui/vulkan/vulkan_device.cpp`: added the `vulkan_command_counters` cvar and
  `command_counters.inc` (LRA's), `AMD_buffer_marker`, `queue_flags`, `maxColorAttachments`, the four
  storage/int16 features and `shaderFloat16` under `with_native_shader_support`, the AMD entry point;
  `queue_family_native_offscreen_` = the graphics family (LRA separates a second queue only on its
  platforms). Our stencil-copy and NX memory counters unchanged.
- `functions/device_1_0.inc`, `device_1_2.inc`: LRA's (`vkCmdFillBuffer`, `vkWaitSemaphores`,
  `vkGetSemaphoreCounterValue`). New headers from LRA: `image_access.h`, `generated_frame.h`,
  `native_queue_policy.h`.
- `include/rex/ui/vulkan/instance.h`: DLSS getters, always false ("DLSS is not part of this build").
- `include/rex/ui/vulkan/presenter.h`: `VulkanGuestOutputRefreshContext::image_access()` (empty ticket),
  `MarkImageAccessSubmitted`, `SetGeneratedFrame` (refuses); `VulkanPresenter::CanPresentGeneratedFrames`
  (false). `include/rex/ui/presenter.h`: `GuestOutputProvenance::frame_rate_limit` and
  `producer_backpressure`, `Presenter::CancelFramePacingWaits` (no-op).
- `include/rex/filesystem.h`, `src/core/filesystem.cpp`: `GetResourcesFolder` (LRA's).
- `src/core/CMakeLists.txt`: `gta4_native/core/options.cpp` and `modern_shader_options.cpp` in rexcore,
  as LRA has them. `src/graphics/CMakeLists.txt`: `temporal/dlss_{bootstrap,upscaler,frame_generation}.cpp`
  in the plugin without the NVIDIA SDK (they answer "unavailable").
- LRA code changed to build on Windows (LRA builds for Android and macOS only):
  `native_spirv_legacy_mul.h` `small` → `small_either` (a Windows macro); `gta4_metal/temporal/projection.h`
  `#undef near`/`far`; `graphics_system.cpp` `FrameTimelineNow` with `steady_clock` instead of
  `clock_gettime(CLOCK_MONOTONIC)`.
- `gta4-recomp/src/theft4_lra_app_stubs.cpp` (new, ours): touch input (Android) as no-ops, the frontend
  sliders drawn by the game's own routine (LRA's lift is translated game code), no temporal boundary.
  `gta4_app.{cpp,h}`: LRA's (only its cvar definitions are used, by the translation tool).

### Shaders: the 24 European shaders

`pc_shader_dump_dir` wrote the 1,274 containers the European base creates in a 480 s run (local,
game data). XenosRecomp `fb32631` + LRA's `cmake/dependency-patches/xenosrecomp.patch` (+ our deko3d
mode, M1) over them: 1,274 shaders, 0 DXC failures. `tools/gta4_shader_cache_merge.py` adds to the
v8 cache the hashes it lacks: **24** (19 pixel shaders, all with the alpha test and a late variant;
5 vertex shaders) → 1,380 entries. Run with the merged cache: **0** `shader cache miss` lines
(24 before).

### Result on the PC (Intel iGPU, 480 s from boot, no input)

Captures of the presenter's guest output every 15 s (`pc_capture_every_s`, new; the window grabs of
[15](15-kabba-port.md) were black), side by side with `tools/pc_side_by_side.py` → `out/pc-runs/m0-side`.
All three runs go through the intro cutscene (credits over the harbour, Niko on the ship, Roman), the
loading and **into the game: Niko in Roman's car at the docks with HUD, radar and the headlight help
text** (the first mission; free roam proper needs input, none was given). The picture matches the
emulated renderer's; LRA's is a little lighter and bluer in the night scenes (its tone/fog overrides).

The 640x360: `gta4_output_height_cap = 720` (the output is the drawable's 1920x1080 otherwise, since
`video_mode_width = 1280` is the default value and LRA's hook only honours a non-default one) with
`gta4_native_upscaler = fsr1`, `gta4_fsr1_quality = performance` → `render=640x360 display=1280x720`.

| Swaps per second (`native-hooks.csv`) | emulated 1280x720 | LRA 1280x720 | LRA 640x360 + FSR1 (v8 cache) | LRA 640x360 + FSR1 (merged cache) |
|---|---|---|---|---|
| 60-120 s | 12.6 | 14.4 | 16.9 | 22.8 |
| 120-180 s | 12.7 | 14.6 | 21.8 | 23.6 |
| 240-300 s (5,000+ draws) | 11.4 | 11.2 | 11.4 | 14.2 |
| 420-480 s (in the car, ~6,000 draws) | 12.2 | 10.9 | 10.7 | 12.6 |
| mean 60 s - end | **12.5** | **11.2** | **12.7** | **15.1** |

Runs: `out/pc-runs/m0-xenos-720`, `m0-lra-720`, `m0-lra-fsr360`, `m0-lra-fsr360-eu`. The two FSR runs
differ by 2.4 FPS with the same settings (run-to-run spread on this PC; the cache merge does not
change the frame work). On this PC the frame is CPU-bound (iGPU, the game's own threads); the GPU
resolution matters little once the game draws 4,000+ times a frame.
