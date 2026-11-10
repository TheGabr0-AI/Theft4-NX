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

## 2026-10-21: M1, the deko3d shader chain (PC)

### The chain

```
EU containers (pc_shader_dump_dir, 1,274)        game data: <private>/eu-private
  -> XenosRecomp fb32631 + LRA's patch + our deko3d mode (patches/)
       Vulkan SPIR-V (the cache, M0) and, with XENOS_RECOMP_DEKO3D_SPIRV_DIR,
       DXC -DGTA4_DEKO3D, no -fvk-invert-y -> <vs|ps>_<HASH>[_late].spv
  -> SPIRV-Cross (GLSL 460, no Vulkan semantics, combined samplers take the texture's binding)
  -> text checks (no buffer references / int64 / nonuniform, UBO < 16, textures < 32,
     no dynamic component index into a uniform vec4)
  -> banjo-nx's passes (tools/deko3d/deko3d_glsl_opt.py), one shader at a time, 60 s limit;
     a shader whose optimised text uam refuses keeps its input (banjo-nx's --check)
  -> uam (thirdparty/uam, PC build) for every variant
  -> DKSH cache gta4_deko3d_shaders.bin ("GDK1", key = FNV-1a 64 of the exact text compiled)
```

Driver: `tools/deko3d/gta4_deko3d_shaders.py`; PC tools built by clang-cl into `out/deko3d_tools`
(`spirv-cross.exe` from SPIRV-Cross `vulkan-sdk-1.4.304.0`, `uam.exe` from `thirdparty/uam` with the
Windows changes of `patches/uam-sa2-nx-adf3a4e-windows.patch`).

**The deko3d mode of XenosRecomp** (`GTA4_DEKO3D`, `patches/xenosrecomp-gta4-deko3d.patch`):

- `shader_common.h`: no `PushConstants` and no `uint64_t`; the vertex constants are
  `g_DkVertexConstants[256]` (uniform buffer binding 0), the pixel constants `g_DkPixelConstants[224]`
  (binding 1), the shared words `g_DkShared[64]` (binding 2) read at the Vulkan build's byte offsets
  (`g_Booleans` 528, half-pixel offset 552, clip plane 560, alpha threshold 580, alpha-to-mask 736/740,
  fragment coordinate scale 744, texture LOD bias 752 + 4 × slot), always with constant offsets;
  `swapFloats` with `1u` instead of `1ull`; the texture helpers (`tfetch2D/2DArray/3D/Cube`, the `Lod`
  ones, `getWeights*`, the dimension queries) take the texture and sampler objects.
- `shader_recompiler.cpp`: the Vulkan constants block under `#ifndef GTA4_DEKO3D` (byte-identical
  without the define), then `printDeko3dConstants`: float constants from the arrays, every fetch slot's
  `*_DescriptorIndex` macros naming `g_DkTexture<dim>_<slot>` / `g_DkSampler_<slot>`; after the body, one
  texture declaration per (slot, dimension) the shader fetches, at binding = slot (set 0) with its
  sampler (set 1), and a `#error` when a slot is fetched with two dimensions (no fixed binding).
- `dxc_compiler.cpp`: `-DGTA4_DEKO3D` and no `-fvk-invert-y` (deko3d's clip-space y points up).
- `main.cpp`: `XENOS_RECOMP_DEKO3D_SPIRV_DIR` writes the deko3d SPIR-V (and the late alpha-test one)
  next to the Vulkan cache, and the HLSL; DXC failures listed in `failed.txt`.

Variants: GTA IV's specialization constant has two features (the cache's masks): bit 0, R11G11B10
normals, and the alpha test (0x702: bit 1 on, the function in bits 8-10). A shader with mask 0 has one
variant; one with the alpha test nine (off, and the eight functions); twice that when bit 0 is in the mask.

### Result (2026-10-21, all 1,274 European shaders)

| Stage | Count |
|---|---|
| shaders (1,274 containers + 638 late alpha-test sources) | **1,912** SPIR-V, **0** DXC failures |
| SPIR-V → GLSL (SPIRV-Cross) and the text checks | 1,912 of 1,912 |
| banjo-nx's passes (first run) | 1,360 optimised, 552 kept as they were (uam refused the optimised text too, the two forms below), 0 timeouts |
| **variants through uam** | **12,060 of 12,120 compiled (99.5 %), 60 failed** |
| distinct DKSH in the cache | 6,284 (99 MB, `gta4_deko3d_shaders.bin`); the SD package `gta4_deko3d_glsl.bin` 112 MB |

The first full run gave 11,488 of 12,120; two GTA IV forms, now fixed in the converter
(`gta4_fixups`), caused 572 of the 632 failures:

- **vertex input locations**: XenosRecomp gives each semantic a fixed location (TEXCOORD0-3 at 13-16,
  COLOR0 17, BLENDWEIGHT 18, BLENDINDICES 19) and uam takes 0-15 ("invalid explicit location N specified
  for `in_var_COLOR0`", 542 variants): the inputs are renumbered in order and the map is the second
  line of the GLSL (`// gta4-dk vertex-inputs 0>0 13>1 18>2 19>3`), which the renderer will follow;
- **pixel constants c224-c255** (90): the Vulkan build reads them by address; the deko3d pixel block is
  declared with 256 float4 in every pixel shader (the renderer binds 4,096 bytes).

**The 60 failures, cause and shaders:** all are vertex shaders (60 of 636) whose control flow
XenosRecomp writes as its general `for (;;)` state machine (183 vertex shaders have it, 123 of them
compile); 53 of the 60 are skinned (BLENDINDICES). uam crashes on 50 of them (segmentation fault, the
same with devkitPro's own `uam.exe` 1.1.0 and with a 16 MB stack) and refuses 10 with "Error
compiling program: -4". 20 are 19,800-29,300-line shaders, 40 are copies of one 2,741-line skinned
shader. The cause inside uam is not isolated (da verificare): a minimal dynamic-UBO-index shader
compiles. List: `<private>/eu-private/deko3d-out/report.txt`. Ways out to try: XenosRecomp's
`XENOS_RECOMP_MERGE_PREDICATES`, splitting the state machine, or a uam fix found with a debug build.
Without them those vertex shaders draw nothing on deko3d (their draws are rejected and counted).

Time: the passes and checks run one shader at a time (a Python process and up to three uam runs each)
and every variant is a uam process: about 6.5 hours for the first full run on this 4-core PC with 3
jobs; `--reuse` keeps every result whose input text did not change.

### What plume calls LRA's renderer would need that deko3d lacks

LRA's renderer calls Vulkan, not plume (above), so the question is asked of its Vulkan use (93
functions) against deko3d 0.5.0 and how banjo-nx's plume back end (`thirdparty/plume-deko3d/plume_deko3d*`)
covers each:

| LRA uses | deko3d | banjo-nx's back end |
|---|---|---|
| bindless descriptor heaps (runtime arrays, descriptor indexing) | image and sampler descriptor tables, handles (`dkMakeTextureHandle`) bound per slot (32 per stage) | `descriptorIndexing = true`: its GLSL converter turns heap accesses into `sampler2D(dkh(t, s))` handles, or fixed bindings when the words are plain shared-constant reads |
| buffer device addresses (`vkGetBufferDeviceAddress`, `RawBufferLoad` push constants) | no buffer references in uam's GLSL | one storage buffer (binding 15) over a 4 GB GPU window, offsets from the low 32 bits; push constants a UBO (binding 5) updated with `dkCmdBufPushConstants`. Our deko3d mode removes both instead |
| pipelines, pipeline cache, dynamic rendering | none: shaders and state structs, `dkCmdBufBindRenderTargets` | pipelines are state bundles; framebuffers bind targets |
| compute (`vkCmdDispatch`: FSR, SMAA's helpers) | `DkStage_Compute`, `dkCmdBufDispatchCompute` | supported |
| geometry shaders | `DkStage_Geometry` | `geometryShader = true` |
| MSAA 2/4/8 (LRA also lists 16 and 32 samples) | `DkMsMode_1x..8x` only | 2/4/8; 16 and 32 not available |
| `vkCmdResolveImage`, `vkCmdBlitImage` | `dkCmdBufResolveImage`, `dkCmdBufBlitImage` (2D engine: no depth, no compressed, no RGB32 formats) | the 2D engine for colour; `resolveRegion = false` (whole images only) |
| occlusion and timestamp queries; `VK_KHR_performance_query` | `dkCmdBufReportCounter` (samples passed, timestamps ×1.627); no performance counters | query pools as report buffers; no performance query |
| `vkCmdWriteBufferMarkerAMD` | `dkCmdBufReportValue` | not exposed |
| timeline semaphores (LRA's second queue) | one queue, fences | one queue: semaphores are no-ops |
| formats: BC1-3, ASTC, D24S8, D32S8, R11G11B10, 10-10-10-2, 16/32-bit | all present (`DkImageFormat_*`) | 124 plume formats mapped; a format without a deko3d one is refused and logged |

## 2026-10-21: M2, the console build (what it is, and what it is not)

**Decision, with the reason.** LRA's renderer calls Vulkan directly (first entry): plume's deko3d back
end cannot be put under it, and making it draw through deko3d means a deko3d recorder for its
`Record*` functions (primitives, clears, resolves, present: 587 Vulkan call sites in 43,000 lines),
which cannot be tried anywhere but the console (no deko3d on the PC, no emulator here). Writing that
blind for the single console run would almost certainly fail on its first frame and say little.
So this build is nfsmw-nx's P0/P1 step (their `dk_prueba` and "the game with an empty renderer"):
everything deko3d needs is exercised on the console, with the game running behind it on the null
GPU. **The game's frames (logo, loading, the intro) are not drawn in this build**; the draws the null
GPU skips are counted as rejected. What brings LRA's renderer onto deko3d is the open decision below.

What the NRO does with `nx_gpu = "deko3d"` (`tools/switch-game/nx_deko3d.{h,cpp}`):

1. plume's deko3d back end (`thirdparty/plume-deko3d`) makes the device (`DkDeviceFlags_OriginUpperLeft |
   DepthZeroToOne`), the queue, a 3-image RGBA8 swapchain on the default NWindow (the console text is
   closed first) and a D32 depth target. uam runs on plume's compile thread, core 2, priority 0x3B, with
   uam's default flags (1471, as the PC tool compiled).
2. **5 s test pattern**, our own GLSL (`tools/switch-game/deko3d/test_pattern.{vert,frag}`): the corners
   **red top left, green top right, blue bottom left, white bottom right** on dark grey; in the middle a
   **yellow** square at depth 0.25 drawn first and a **magenta** one at depth 0.75 drawn after over its
   upper right part: **the overlap must stay yellow** (depth test LESS). Positions from `gl_VertexID`,
   no vertex buffers; the viewport has a positive height (deko3d's clip-space y points up).
3. **DKSH check**: the two test shaders compiled again on the console by plume's uam and compared byte
   for byte with the PC uam's DKSH (`deko3d/test_shaders.h`, `gen_test_shaders.py`): log line
   `DKSH console == PC for the test pattern: yes|NO`.
4. **The game's shaders in the background**, when `sdmc:/switch/theft4/deko3d/` holds
   `gta4_deko3d_glsl.bin` (`tools/deko3d/gta4_deko3d_package.py`) and `gta4_deko3d_shaders.bin` (M1's
   cache), both made on the PC from the player's copy: every variant compiled by the console's uam for
   up to `nx_dk_compile_s` (150 s), each compared with the PC's DKSH (identical / different / not in the
   PC cache / failed) with uam's time per variant (mean, p50, p90, p99, max).
5. After the pattern, a dark blue screen ten times a second, and every 10 s a `deko3d:` line: frames,
   errors, command memory, image/buffer/code memory, uam compiles and total ms, draws, Full and Fragments
   barriers, **rejected draws** (the game's draws the null GPU skipped).

Build switches (`tools/switch-game/CMakeLists.txt`, sa2-nx's names): `REXGLUE_SWITCH_NO_NVK` (no Mesa, no
Vulkan backend: forces `THEFT4_SWITCH_GAME_VULKAN` off), `REXGLUE_SWITCH_DEKO3D` (plume's back end, uam
localised as in sa2-nx, `nx_deko3d.cpp`), `REXGLUE_SWITCH_DEKO3D_DEBUG` (`libdeko3dd` and plume's
guard bytes). `nx_gpu`'s default stays `vulkan`. The NVK build is unchanged and still configures as
before (`out/build/switch-game`).

ESISTENTE → NUOVO outside the new files:

- `tools/switch-game/main.cpp`: the five header-only `nx_*` includes were under `#if THEFT4_NX_VULKAN`
  but used everywhere, so the build without Vulkan no longer compiled: they are outside the guard now
  (only `vulkan/graphics_system.h` and the UI headers stay inside). New: `nx_gpu = deko3d` (with the null
  GPU), cvar `nx_dk_compile_s`, `nx_deko3d::Stop()` (join) in `ExitGame`.
- `tools/switch-game/CMakeLists.txt`: the three options above; `REXGLUE_SWITCH_NO_NVK` sets
  `THEFT4_SWITCH_GAME_VULKAN` off before its `option()`.

### Open decision: how LRA's renderer reaches deko3d

| Way | What | For | Against |
|---|---|---|---|
| A. a recorder boundary inside `gta4_native` (nfsmw-nx's way, HowToDeko3d 5.1) | move the Vulkan calls of `RecordPrimitive/IndexedPrimitive/PrimitiveUp/Clear/Resolve/Present/NativeFrame` and the resources behind an interface ("record this draw with this state"), first with no behaviour change on the PC, then a deko3d implementation with the DKSH of M1 | the PC keeps checking every frontend change; deko3d gets state structs, not fake pipelines | the boundary goes through a 38,000-line file; the post-effects (SMAA, FSR1 compute, sun shafts, split post-fx) and the LRA overrides need their own deko3d shaders |
| B. `gta4_native` onto plume, then plume's deko3d back end | rewrite its 587 Vulkan call sites against plume's `RenderInterface` | banjo-nx's back end is console-proven (sa2-nx 1.0.3 ships it) | a full rewrite of the renderer's backend half; plume's descriptor and pipeline model differ from LRA's (bindless heaps, dynamic rendering, BDA) |

Recommendation: A, in two console runs: first the draws of the logo, loading screens and Bink videos
(2D quads, the Bink shaders, clears, the present), counting every draw it does not handle as
rejected; then the 3D scene. Both start from this run's answers: whether uam's DKSH match the PC's on
the console, how long uam takes per variant, and whether the device, swapchain and memory hold for
four minutes beside the game.

### The build (2026-10-21)

`out/build/switch-game-dk` (`-DREXGLUE_SWITCH_NO_NVK=ON -DREXGLUE_SWITCH_DEKO3D=ON`, and
`-DREXGLUE_SWITCH_DEKO3D_DEBUG=ON` for the debug NRO): `theft4-dk.nro` 45.9 MB and `theft4-dk-debug.nro`
45.9 MB (the NVK NRO is about 58 MB). No Mesa driver in the ELF (`vkGetInstanceProcAddr` absent from the
map). The frozen NVK build (`out/build/switch-game`) still compiles and links after the SDK changes.
The game's shader package is read from the card a table and a text at a time (112 + 99 MB files: not
loaded whole, the game keeps its memory).

### Run 35: what to copy, what to read

Copy to `sdmc:/switch/theft4/`: `theft4-dk-debug.nro` (first) or `theft4-dk.nro`, and `game.toml` from
`out/console-runs/run35-build/`; to `sdmc:/switch/theft4/deko3d/`: `gta4_deko3d_glsl.bin` and
`gta4_deko3d_shaders.bin` from `<private>/eu-private/deko3d-out/` (game data from the player's copy;
without them the build still runs, with no background compiles). One boot of 4 minutes, standard clocks.

On screen: 5 s of the test pattern (corners red / green / blue / white clockwise from the top left on
dark grey, a yellow square partly covered by a magenta one **whose overlap stays yellow**), then dark
blue. The game runs behind it on the null GPU (sound should play); its frames are not drawn.

In `game.log`, the `deko3d:` lines:

1. `starting`, `device, queue, swapchain 1280x720 (3 images), depth target` (or the failure);
2. `DKSH test pattern vertex/pixel shader: console == PC, identical` and `DKSH console == PC for the test
   pattern: yes|NO`;
3. `PC DKSH cache: 6284 variants`, `game shader package: 1912 shaders, 12120 variants` (when copied);
4. `test pattern done after N frames`;
5. every 10 s: frames, **errors** (in the debug NRO, libdeko3dd's messages come through `plume:` lines),
   **command memory**, image/buffer/code memory, **uam compiles and ms**, draws, barriers, **rejected
   draws** (the game's, skipped by the null GPU);
6. `background uam so far` / `done`: variants compiled, **identical to the PC**, different, not in the
   PC cache, failed, and **uam's ms per variant** (mean, p50, p90, p99, max).

## 2026-10-21: run 35 read (A), and M3 started (B to F)

The user's run 35 (`theft4-dk-debug.nro` and `theft4-dk.nro`, commit `a199a68`, `nx_gpu = deko3d`), logs in
`out/console-runs/run35-logs` (`game-debug.log` and `game-crash-debug.log` from the debug NRO, `game.log` and
`game-crash.log` from the release one). On the console: the test pattern's rectangles all as expected, sound
right, nothing else drawn (expected: the null GPU), the game reaches the game and reads the pad; holding + to
quit showed an error (with NVK the exit was clean).

User's decision for M3: a deko3d back end written inside `gta4_native`, the recording of draws separated from the
front end (state, shader choice, preparation of textures and vertices); the Vulkan part stays as it is; no plume
for drawing. Stages B1-B5 behind the cvar `nx_dk_stage`.

### A. Run 35 from the `deko3d:` lines

| Question | Answer (debug NRO; the release one the same within a frame) |
|---|---|
| DKSH console == PC | **yes**: the test pattern's vertex shader (1,280 bytes, uam 11 ms) and pixel shader (512 bytes, 7 ms) identical; in the background **19 of 19** game variants compiled by the console's uam identical to the PC's DKSH (0 different, 0 not in the PC cache, 0 failed) |
| libdeko3dd errors | **0** during the run (`errors 0` in every line); one at the exit (below) |
| command memory, images | plume's queue 1,024 KiB of command memory; images 3,840 KiB (the swapchain); buffers 0; code 1 KiB |
| uam per variant | **5.1 to 30 s** per GTA IV variant on the console (mean 8.4 s, p50 8.2 s, p90 10.2 s, max 30.0 s), on core 2 at 0x3B beside the running game, against 7-11 ms for the test pattern's small shaders: only 19 of 12,120 variants in the 150 s budget. Run-time compiles of the game's shaders are not an option: **the console must load the PC's DKSH** (B3 does) |
| variants equal to the PC | 19 of 19 |
| draws skipped | 3.59 million in 208 s (release), the null GPU's: about 580 per swap |
| frames | 303 frames of test pattern in 5 s (60 Hz); during the background compiles the pattern loop presented one frame per compile (the loop waited for uam), then ten per second |

**The error at the exit**, decoded with `addr2line` on `switch-game-debug.elf`:

- `exit(1)` from `defaultDebugFunc` (`dk_device.cpp:30`, libdeko3dd): the debug library shows an error applet
  (`errorApplicationCreate`/`Show`, **the error the user saw**) and calls `exit(1)`, after `deko3d says
  (dkDeviceDestroy, result 9): unfreed memory blocks`. What was being taken down: `nx_deko3d::Stop()` from
  `ExitGame`, then plume's `Deko3DDevice::~Deko3DDevice`, then `dkDeviceDestroy` with a block still alive: plume
  makes `texSizeBlock` ("texture sizes") at device creation and its destructor never frees it.
- 0.3 s later, the access violation (`far 0x60`) on the **Main XThread** in
  `std::_Hashtable<std::string, ...>::_M_locate_tr` (an `unordered_map<string, ...>` lookup; the key in the
  registers is `"video_mode_width"`): `exit(1)` ran the static destructors, the cvar table among them, while the
  game's main thread was still running and looked up a cvar. A consequence of the first, not a second bug.
- The release library does not check at `dkDeviceDestroy`: its exit is clean (`exit(0)`, `game-crash.log`).

### D. The exit (one attempt)

- `thirdparty/plume-deko3d/plume_deko3d.cpp` (`patches/plume-deko3d-sa2-nx-adf3a4e-teardown.patch`):
  ESISTENTE `~Deko3DDevice` destroys the retired blocks, the image chunks, the code blocks, `pushBlock` and
  `zeroBlock`, then the queue and the device. NUOVO: also `texSizeBlock`; a counter of live blocks
  (`makeBlock`/`destroyBlock`) logged before `dkDeviceDestroy` if not zero.
- `tools/switch-game/nx_deko3d.cpp`: ESISTENTE the render thread returned and its locals were destroyed in
  reverse order. NUOVO: after the last fence, everything made on the device is reset explicitly (fence, list,
  pipeline, layout, shaders, framebuffers, depth, swapchain, queue), then the device (whose destructor stops and
  waits for plume's compile thread, 2 s at most), with two log lines.
- The new recorder (B) does the same in its own teardown: the recorder (GPU idle), the uam thread (joined), the
  device last, with the live blocks logged (`deko3d: teardown:` lines).

Da verificare on the console (the debug NRO's exit with + held). If it still fails: HOME.

### C. The emulator: not done

sa2-nx's `docs/deko3d-renderer-guide.md` section 5 and `docs/renut-renderer.md` name **banjo-nx's Ryubing build**
(a Ryujinx fork) and say where it lives: a local source folder of banjo-nx's author, with RenderDoc used on it.
Neither project gives a URL, a release or a commit for it, and banjo-nx's own repository is not among our
references. Following the rule (no builds from anywhere else), stopped here: no emulator, and the three
"emulator start" SDK changes were not ported (they would be untestable). To pick this up, banjo-nx's project has
to publish the build or say where it comes from.

### E. The 60 vertex shaders uam fails on

- They are **60 different vertex shaders, variant 0 each**, but only **12 distinct GLSL texts** (the same
  shader under several hashes): 39 of them share one 2,577-line text with 22 `case`s in XenosRecomp's
  `for (;;) { switch (pc) ... }` state machine, the others are 17,000-29,000 lines with 32-38 cases.
- **For 40 of the 60, M1's cache already has a DKSH for the identical text** (another hash with the same text
  compiled in the same run). So the crash is not a property of the text alone: uam crashed on the same input in
  one process and not in another. `gta4_deko3d_runtime.py` looks DKSH up by the text, so the console package has
  DKSH for 40 of them: **20 variants without DKSH** in all.
- The rewrites, each tried on the 60 with the PC uam (`out/deko3d_tools`, 16 MB stack):

  | Rewrite | Compile | Failures |
  |---|---|---|
  | as it is | 0 of 60 | 48 crash (0xC0000005), 10 "Error compiling program: -4", 2 over 300 s |
  | outer `for (;;)` bounded to 65,536 iterations | **2 of 60** | 47 crash, 10 error -4, 1 timeout |
  | `do { } while (true)` | 0 of 60 | 49 crash, 10 error -4, 1 timeout |
  | `while (true)` | 0 of 60 | 46 crash, 10 error -4, 4 timeouts |
  | every `for (;;)` bounded | 2 of 60 | 46 crash, 10 error -4, 2 timeouts |

  The loop's form is not the cause: uam crashes (an access violation) or gives -4 inside, whatever the loop
  looks like. The 2 saved are one text (26,632 lines, two hashes), both among the 20 without DKSH: their DKSH
  (bounded loop: the same shader unless it runs 65,536 blocks) go into the package with
  `gta4_deko3d_runtime.py --extra-dksh`: **18 variants without DKSH** now. Today's uam crashes on all 60 texts as
  they are, also on those whose DKSH M1's cache holds: those 40 DKSH come from an earlier run of the chain (its
  `--reuse`), and whether that uam was the same build is not known: **da verificare** (their draws on the console;
  the cache can be rebuilt without `--reuse`). Next: the crash under a debugger on the PC tool (16 MB stack
  already), or splitting the state machine's cases into functions.
- Draws per frame with these shaders: not measured on the PC in this session (da verificare at the next PC run).
  The console counts every draw it skips for a shader without DKSH (reason 14 in the report) and logs the first.
- Until a fix: the console skips those draws with a log line.

### B. The deko3d back end inside `gta4_native`

**Where the boundary is.** `gta4_native`'s Vulkan class keeps its front end and its recorder in one
38,000-line file, and its render worker creates Vulkan objects while it assembles a frame, so the boundary
could not go through it without changing the Vulkan path (which stays as it is). The interface is therefore at the
title commands, with the game's semantics shared: LRA already keeps those in `gta4_native/core/` ("game
semantics only: no API handles": `core::DecodeFixedState` of the guest device, the shared constant layout, the
vertex usage locations), and the new front end is built on it.

- `gta4_native/native_recorder.h`: the thin interface, no graphics API in it: `RecorderDraw` (fixed state from
  `core::DecodeFixedState`, the constants that changed since the previous draw of the device as runs of
  registers, the shaders by hash, the vertex declaration, the streams and the index buffer as guest addresses, the
  fetch constant and object of each sampled slot, the render targets and depth target as `SurfaceDescriptor`),
  `RecorderClear`, `RecorderResolve`, `RecorderPresent`, `RecorderResourceEvent`, and `INativeRecorder` (Draw,
  Clear, Resolve, Present, ResourceEvent, Report). Vulkan is the other implementation in principle; in practice
  it keeps its own path (moving it behind this interface is later work).
- `gta4_native/native_front_end.{h,cpp}`: the title commands read as `Gta4NativeGraphicsSystem`'s producer reads
  them (`ValidateAndCopyCommand`, `CaptureSurfaceDescriptor`, the device layout of `core/draw_state.h`): at each
  draw the device is decoded and the constants compared with the previous draw's; DrawPrimitiveUp vertices are
  copied (the title rewrites them right after the call); vertex and index buffers and textures are read by the
  recorder, which the title waits for (the completed-frame word).
- `gta4_native/deko3d/` (Switch only): `dk_core` (device, queue, work slots, upload ring, image heap, code,
  descriptors, swapchain: nfsmw-nx's `Nucleo`), `dk_compiler` (uam thread: 8 MB stack, priority 0x3B, core 2,
  banjo flags 1471 as the PC), `dk_shader_package` (the SD package below), `dk_xenos.h` (tiled addresses,
  NEON untiling, packed mips, byte swaps: nfsmw-nx's), `dk_png.h` (stored-deflate PNG), `dk_recorder` (the
  recorder), `dk_graphics_system` (the `IGraphicsSystem` the title talks to: ABI 33, the front end on the
  title's threads, one recorder thread, the completed-frame word after each recorded Present, at most two frames
  queued, the null GPU kept inside for the Vd* services).
- `tools/deko3d/gta4_deko3d_runtime.py`: the console's shader package `gta4_deko3d_runtime.bin` (format in the
  script): per shader (by hash, early or late) its used texture mask and specialization mask from
  `shader_cache.cpp`, its vertex inputs with their XenosRecomp and uam locations and numeric type (from the
  `// gta4-dk vertex-inputs` map and the declarations), the texture type of each binding, its colour outputs, and
  the PC's DKSH of each variant. Made from M1's output: 1,912 shaders, 12,120 variants, 6,285 DKSH, 99.6 MB; 18
  variants without DKSH (E).

The rules paid for by nfsmw-nx, where they are in the code: no pipeline state (`DkRecorder::Emitted`, each block
emitted only when it changes); one vertex buffer per `dkCmdBufBindVtxBuffers`; 256 bytes of slack per command
piece (`dk_core.cpp`); textures as handles; barriers only around copy-engine transfers (`BeforeTransfer`,
`Before3D`); descriptors written with `dkCmdBufPushData` into the pre-list only; the flip as a viewport swizzle
(identity: the game's viewports are D3D's, positive height).

Stages (`nx_dk_stage`; the draws read it at every draw; 0 is run 35's build):

| Stage | What | Target | State |
|---|---|---|---|
| B1 (1) | the output (the guest front buffer, a full-screen pass with `nx_dk_gamma`), PNG captures every `nx_dk_capture_s` s in `sdmc:/switch/theft4/captures`, clears, render targets as images (D3D formats A8R8G8B8, A16B16G16R16F, R32F, G16R16F, D24FS8 as ZF32_X24S8), resolves | the clears and resolves of the logo frames on screen; draws counted, not drawn | written, compiled; da verificare |
| B2 (2) | resolves as copies (copy engine; a blit between formats; the resolve's clears), textures (fingerprint, NEON untiling, upload in the pre-list, checks every 1 to 32 frames, 256 MB then the unused ones go), samplers, the 2D draws (no depth test): vertex input from the declaration and the shader's map, big-endian conversion (32-bit words, 16-bit components, DEC3N, D3DCOLOR), rectangle lists as quads, indices, constants by differences (`dkCmdBufPushConstants`), the shared block | logo, loading screens and Bink videos right on the Switch | written, compiled; da verificare |
| B3 (3) | the 3D draws with the PC's DKSH; a variant without DKSH compiled by the console's uam in the background, its draws skipped with a log line until it is ready | the city drawn | written (the same draw path); da verificare |
| B4 (4) | depth bias (units as the Vulkan path's D32F, da verificare); ZCULL, deferred depth clears, shadows, reflections, texture read-back to guest memory: not yet | | partly |
| B5 (5) | post-processing, FSR1, the final pass: not yet | | not started |

What B does not do yet (counted or logged when the game asks for it): 1D, 2D-array and 3D textures (empty
textures of the type), cubemaps of resolved faces, texture read-back (`TextureLock` answers without copying),
occlusion queries, MSAA, the EDRAM aliasing of surfaces that share placement (LRA's `FindPlacementOwner`), the
temporal path and the reflections registry, LRA's shader overrides, the environmental data (fog and the "modern
effects" constants stay zero).

ESISTENTE → NUOVO outside the new files:

- `tools/switch-game/main.cpp`: ESISTENTE `nx_gpu = deko3d` = the null GPU plus `nx_deko3d::Start` (plume's test
  pattern). NUOVO: cvars `nx_dk_stage` (0-5, default 2), `nx_dk_gamma` (1.0), `nx_dk_capture_s` (0); with
  `nx_dk_stage >= 1` the null GPU is wrapped in `Deko3dGraphicsSystem` and the test pattern is not started;
  `ExitGame` also stops the recorder thread (`g_dk_graphics->Stop()`); the console text is closed for both.
- `tools/switch-game/CMakeLists.txt`: ESISTENTE `REXGLUE_SWITCH_DEKO3D` built plume and `nx_deko3d.cpp`.
  NUOVO: the library `theft4-gta4-deko3d` (the front end and `gta4_native/deko3d`, C++23, linked with plume's
  target for uam and libdeko3d), and **LRA's native hooks at the European addresses** (the PC build's
  `THEFT4_GTA4_NATIVE` list from `out/gta4-native-eu/src`): without them the title never sends a command. They
  reach guest memory with `base + address + REX_PHYS_HOST_OFFSET(address)` (aliased views); the Switch has none,
  so `tools/switch-game/nx_lra_hooks_compat.h`, forced into those sources only, defines the offset so the sum is
  `rex::memory::GuestToHost(address)` (LRA's files unchanged). Checked in the ELF: the 104 hooked functions
  resolve to `gta4_native_hooks.cpp` (e.g. `sub_829D0CC8`), the generated ones stay as `__imp__`.
- `native_recorder.h`: a vertex buffer whose object holds a fetch constant (type 1) and an address below
  0x20000000 is read through the physical view; LRA reads every one through the virtual view (on the PC they are in
  the physical windows, where both agree).

### F. Run 36

The build (`out/build/switch-game-dk`, commit in `out/console-runs/run36-build/COMMIT`): `theft4-dk-debug.nro`
(libdeko3dd) and `theft4-dk.nro`, the ELFs, and `game-stage2.toml`, `game-stage3.toml`, `game-stage5.toml`.

To copy to the card:

- `sdmc:/switch/theft4/theft4-dk-debug.nro` and one of the tomls as `sdmc:/switch/theft4/game.toml`;
- `sdmc:/switch/theft4/deko3d/gta4_deko3d_runtime.bin` (new, 99.6 MB) and `gta4_deko3d_glsl.bin` (as in run 35),
  from `<private>/eu-private/deko3d-out/` (game data from the player's copy).

The order: **stage 2 first** (3 minutes: the logo, the loading screens, the intro video), then stage 3 (4 minutes,
into the game), stage 5 only if 3 draws. To copy back: `game.log`, `game-crash*.log`, `game-exit*.log` and
`sdmc:/switch/theft4/captures/dk-f*.png` (one every 10 s). If a boot shows nothing or the log stops, the same toml
with `nx_dk_stage = 1` says whether the output and the resolves work without any draw.

What to read in `game.log`: `deko3d:` lines: the device, the core, the uam self-test, `shader package ...: 1912
shaders, 6285 DKSH`, `recorder ready`, the first render targets and resolved textures (`render target`, `resolved
texture`), `draws skipped: <why> (reason N)` (once per reason), and every 10 s the front end's counters and the
recorder's two report lines (frames per second, draws per frame, drawn and skipped by reason, clears, resolves,
presents with and without a source, textures, shaders from the PC and from the console's uam, command memory,
upload, barriers, image and code memory, deko3d blocks, **libdeko3dd errors**); at the exit, the `teardown:` lines.

## 2026-10-26: run 36 read, the shader package fixed, run 37 prepared (G)

Labels: VERIFICATO (seen in a file, a command or a test run on this PC, cited), DA VERIFICARE (not yet proven, or
only on the console), STIMA (engineering estimate). Nothing in this section was run on a Switch: **no hardware
result exists for run 37.** Base `17f3a18` plus uncommitted changes (the NROs are stamped `17f3a18-dirty`; no
commit, no push).

### G1. Run 36, from the logs (`out/console-runs/run36-logs`, `run36-build`)

- VERIFICATO: the NRO that ran is the debug one: `theft4-dk-debug.nro` and `switch-game-debug.elf` have the same
  GNU build-id `ffc677b8…`; the release pair has `5a77494c…`. The log shows `code block 0x2891000` (the debug
  text size) and "debug deko3d library". That the file on the card was this NRO is read from those two lines (the
  card cannot be hashed from here).
- VERIFICATO: stage 2 and stage 3 die identically, 12.7 s after the start, on thread `main (ui)`, after `null GPU:
  no rendering; ring consumed` (game.log:77) and before `GPU system initialized`, with no `deko3d:` line. Data
  abort, read, translation fault (esr `0x92000007`); `far - sp` = 0x1e68 in both runs.
- VERIFICATO (`addr2line` on the debug ELF, offset 0: the ELF is a PIE at 0 and the log says "ELF address = runtime
  address - base"): pc-base `0x285a11c` is libgcc's unwinder, `aarch64_demangle_return_addr` inlined in
  `uw_update_context`; lr-base `0x285a0b8` is `uw_update_context`; `x21` - base is `std::bad_alloc::~bad_alloc()`.
  The fault is in the C++ exception unwinder on the main thread: not in the translated code, the LRA hooks, uam or
  deko3d drawing. (Earlier readings that put it elsewhere were from another ELF; the release ELF decodes the same
  address as `_Unwind_Find_FDE`.)
- VERIFICATO: `game.prev.log` (the same build, `nx_dk_stage = 2`, no `runtime.bin` on the card) passes the same
  point: `no shader package`, `the recorder did not start`, `deko3d: graphics system`, `GPU system initialized`,
  and the game starts. Only the presence of the package changes the outcome.
- VERIFICATO: the code between the last line and the fault is `Deko3dGraphicsSystem::SetupGuestGpu` after the
  inner null GPU: `ShaderPackage::Open`, then the front end and the recorder thread; `Open` had no `try`/`catch`
  and the only `catch` on the path was in `OnTerminate`.

### G2. The package format: what was wrong

- VERIFICATO (`tools/deko3d/gta4_deko3d_runtime.py` at `17f3a18`, the run 36 file and the reader): the writer
  packed each table record with `'<QBBBBIIIIIIII'` = **44 bytes**, but its docstring heading ("shader count x 48
  bytes"), its own `extra_pos = table_pos + 48 * count` (so every stored offset), the reader's stride
  (`dk_shader_package.cpp` 55, 63, 77) and the blob table position all assume **48**. In the file
  (`gta4_deko3d_runtime.v1-run36.bin`, 99,569,920 bytes, 1,912 shaders, 6,285 blobs): the records end at
  88 + 44 x 1912 = 84,216, the stored offsets of record 0 start at 88 + 48 x 1912 = 91,864 and form one
  contiguous chain from there; the physical extra block starts 4 x 1912 = 7,648 bytes before the first offset that
  points at it; the blob table sits at 261,120, where the 48-based arithmetic puts it. Read at stride 48 the table
  is garbage (stage not in {0, 1} in 1,204 of 1,912 records, not sorted).
- Format decision: the 48-byte record is the intent (four independent places agree on it, and 8-byte records keep
  each u64 hash aligned); the docstring's field list ends at 44 bytes because the last four were never named. The
  writer now names them: a reserved u32 and a padding u32, both zero, checked by the reader. The 44-byte reading
  (change four strides) was rejected: it would leave every stored offset wrong.
- VERIFICATO (host build of the **original** reader, `git show 17f3a18:…/dk_shader_package.cpp`, on the run 36 file,
  with `operator new` refusing any single block above 3,000 MiB like the 3,189 MiB process does): `Open` throws
  `std::bad_alloc`; the largest request is 4,294,875,502 bytes (~4 GiB). The same arithmetic redone in Python gave
  4,294,875,463; the 39-byte difference was not chased. This is a simulation of the reader on the PC: the exception
  was never observed on the console (see G7).
- Not comparable: the card's run 36 copy has been replaced, so its hash can no longer be compared with the PC file
  (SHA-256 of the PC file as it was, kept as `gta4_deko3d_runtime.v1-run36.bin` in the private folder:
  `4e9cb185118a19032aebee659c168a3cc94c931ec9ce5f67e6a5ff54d4e6872d`, 99,569,920 bytes). A size of 99,573,760 bytes
  for the PC copy, as remembered in the task, was not found: a search of `C:\dev` by size finds only 99,569,920.
  Whether the card's old copy equalled the PC's is **DA VERIFICARE and now unverifiable**.

### G3. What changed (all uncommitted)

| File | Change |
|---|---|
| `tools/deko3d/gta4_deko3d_runtime_check.py` (new) | the version 2 layout (header 32 bytes with record size 48 and the whole file size; 48-byte records with reserved + padding; offsets absolute) and the validator the reader mirrors; CLI prints counts and SHA-256 |
| `tools/deko3d/gta4_deko3d_runtime.py` | version 2; writes `….bin.new`, validates it, only then replaces the old file (a rejected one stays as `.rejected`); prints SHA-256 |
| `…/gta4_native/deko3d/dk_shader_package.cpp/.h` | `Open` checks every count, size and offset against the file size (and limits) before it allocates or indexes; refuses a file whole with the first reason logged and leaves the package empty; no exception leaves `Open`; the optional GLSL package is checked the same way (a bad one is dropped whole, logged); lazy reads (`ReadBlob`, `VariantText`) cannot throw; one more log line for the GLSL package |
| `…/dk_graphics_system.cpp` | `SetupGuestGpu` catches what the package or the set-up throws (log, "no picture"); `deko3d: set-up: shader package loaded|NOT loaded` is flushed |
| `tools/switch-game/main.cpp` | the start-up line says `deko3d (debug library libdeko3dd)` / `deko3d (release library)` (it said "null GPU only" in run 36) |
| `tools/switch-game/CMakeLists.txt`, `build_variants.sh` (new) | `THEFT4_SWITCH_APP_NAME` (the NACP name; default as before), a warning for `DEKO3D_DEBUG` without `DEKO3D`, and the script that builds the named variants |
| `tests/deko3d/` (new) | host tests, below |
| `run-index.md`, `run-protocol.md` | rows 35 (outcome), 36, 37; NRO names |

`.gitignore` ignores `*.py` and `*.sh`: the new `gta4_deko3d_runtime_check.py`, `test_runtime_package.py` and
`build_variants.sh` need `git add -f` (as the other tools here were added).

### G4. The new package and the tests

- VERIFICATO: regenerated from the same inputs (`deko3d-out`, the EU `shader_cache.cpp`, `dksh_rewritten`):
  `gta4_deko3d_runtime.bin` version 2, **99,569,920 bytes**, 1,912 shaders, 12,120 variants (18 without DKSH),
  6,285 DKSH, tag `uam 1.1.0 - Built on Oct 21 2026 00:52:23 patches 1-8 gta4-deko3d-1`, **SHA-256
  `513924533c37d3e6b41b994ecb03a0bcf4d9892a3a03a12fc6eed35df1381839`**. Same size as the old file (the old writer
  padded to the 48-based position). Compared with the old one: all 1,912 records equal after rebasing the three
  offsets, the 169,212-byte extra block byte-identical, the blob table and all DKSH data (from 261,120 to the end)
  byte-identical. A second reviewer re-derived the same result with their own script.
- VERIFICATO: `gta4_deko3d_glsl.bin` (111,566,486 bytes, SHA-256
  `DEF68A19D3838B7843469BE2B4DFE10795F7740A1B97C2366AF8751CF3EC1DDA`) is unchanged,
  GDKG version 1, 1,912 texts, passes the new GLSL checks (largest text 1,245,038 bytes).
- VERIFICATO: `python tests/deko3d/test_runtime_package.py --real <deko3d-out> --old-v1 <v1 file>`
  (`THEFT4_VS_ROOT` set; `THEFT4_TEST_SANITIZE=1` adds ASan + UBSan) builds 42 small runtime packages (5 good, 37 broken in
  one way each: truncated, padded, wrong version/record size/magic, counts over the limits, offsets outside the
  file or below the extra block, unsorted or duplicate hashes, bad stage/late/inputs/texture types, reserved
  fields, DKSH index/offset/size), a missing file and 9 GLSL cases, compiles **the console's `dk_shader_package.cpp`
  unchanged** with the host clang, and requires the Python validator and the C++ reader to give the same verdict;
  the real package opens (1,912 shaders, 6,285 DKSH, the first 64 DKSH read back), and the run 36 file is refused
  by version by both. All passed, with the sanitizers. A package that is refused is empty afterwards.
- Limits of the tests (DA VERIFICARE on the console): they run on the host (64-bit Windows, `long` is 32 bits
  there; on the Switch it is 64); the reader's allocation cap and 2 GiB file cap have no fixture; the overlap of
  offsets between shaders is not checked (harmless to the reader, it can inflate memory and would throw `bad_alloc`,
  now caught).

### G5. The NRO variants (`out/console-runs/run37-build`)

Built with `tools/switch-game/build_variants.sh` (`-j3`, one build at a time, no other ninja/cc1plus running).
`COMMIT` = `17f3a18-dirty`. Build-ids equal those of the ELFs kept beside them (VERIFICATO with `readelf -n`).

| NRO (menu name = NACP) | Bytes | SHA-256 | What | ELF (build-id) |
|---|---|---|---|---|
| `theft4-dk-debug.nro` ("Theft4 DK (debug)") | 46,852,293 | `d0e46fb04ed43788f30fdc45143c5b7fecce3848a5d8203282d1808d20c3ff6f` | deko3d renderer, `libdeko3dd` (checks every call), `PLUME_DEKO3D_GUARDS` | `switch-game-dk-debug.elf` (`a83a609e…`) |
| `theft4-dk.nro` ("Theft4 DK") | 46,819,525 | `fd9d895713ef34b6b6aae6574e6fc43c3a3348c79f326ebc2f6799c5ae1b3d51` | the same, `libdeko3d` | `switch-game-dk.elf` (`7bde59f4…`) |
| `theft4.nro` ("Theft4") | 64,637,125 | `b73446d97bef7cabf31dfc1304a4fecc330ce05a22a973ce92f6c54d5989f744` | the Vulkan/NVK build (Mesa NVK linked, no deko3d) | `switch-game-nvk.elf` (`55f8fcd9…`) |
| `previous-nvk-switch-game-2026-10-19.nro` ("Theft4 switch-game") | 64,633,029 | `2439f74914cd62355f9f22907c8c8c604b61c3e8cb3b2761484605671e39f57b` | the NVK NRO that was in `out/build/switch-game` before, kept as it was | – |

The two deko3d NROs share one build dir and one set of objects; they differ only through the deko3d library and the
guard define (checked in the strings of the ELFs; the debug text segment is 12 KB larger). `theft4.nro` was
rebuilt from the same tree (only `main.cpp` and the link changed for it); the NVK path compiles and links with the
changes. **`theft4-debug.nro` does not exist**: the NVK configuration has no debug variant (the only debug switch
in the build is `REXGLUE_SWITCH_DEKO3D_DEBUG`, a deko3d library choice); a Debug-type NVK build would recompile
every generated source and was not made. Nothing in the repository selects the Sphaira forwarder's target NRO
(no forwarder file, no path): its configuration was not touched; select the NRO by its menu name, or point the
forwarder at the file by hand. Note: `out/build/switch-game/switch-game.nro` (19 Oct) is still in that build dir; it
is the old code with the old menu name "Theft4 switch-game", do not copy it by mistake.

### G6. The stages against the code (`dk_recorder.cpp::Draw`)

VERIFICATO in the code: stage 1 rejects every draw (counted as "nx_dk_stage 1: draws off"); stage 2 draws those
without a depth test and rejects the ones with (`depth_enable` and a depth target) as "before stage 3"; stage ≥ 3
draws both; stage ≥ 4 adds only the depth bias (`stage >= 4 && depth_bias_enable`); **there is no code for stage 5**
(no FSR1, no post-processing), none for ZCULL (the queue is created with ZCULL off), shadows or reflections. So
"stage 4 = ZCULL, shadows, reflections" and "stage 5 = post-processing, FSR1" are plans, not code. Stage 0 starts
plume's test pattern, not the game's renderer, and is not a test of it.

### G7. Hypotheses

| | State |
|---|---|
| The table stride 44/48 makes the old reader ask for ~4 GiB | **confirmed** on the host with the original reader (G2) |
| That `bad_alloc` is what killed run 36 on the console | **open**: strongly fits (`x21` = the `bad_alloc` destructor, the log position, the A/B without the file) but the exception was not observed and the card's file was not examined |
| Why an uncaught exception ends in the libgcc unwinder on the main thread instead of `std::terminate` | **open**; the fault address is 8 bytes above a page-aligned value, 0x1e68 above `sp` in both runs; no stack range in the crash log (`CrashFinalHandler` is installed after `Setup`) |
| uam, the LRA hooks, deko3d drawing as the cause of the run 36 crash | **refuted for this crash** (pc in libgcc, before the recorder thread exists) |
| `nx_dk_stage = 0` restores run 35's behaviour | **DA VERIFICARE**: not run; the branch is preserved in `main.cpp`, but the hooks and cvars of 17f3a18 are linked in at every stage |
| The recorder, `dk_core` and the swapchain work on the console | **DA VERIFICARE**: they have never run (run 35 used plume's equivalent) |

### G8. Run 37: what to copy and what to read

On the card (nothing else changes; keep the old logs under other names first: `game.prev.log` is overwritten at the
start, `game-crash.log` is appended):

- `sdmc:/switch/theft4/theft4-dk-debug.nro` from `run37-build/` (menu: "Theft4 DK (debug)");
- `sdmc:/switch/theft4/game.toml` = `run37-build/game-stage2.toml` (`nx_gpu = "deko3d"`, `nx_dk_stage = 2`,
  `nx_dk_capture_s = 10`);
- `sdmc:/switch/theft4/deko3d/gta4_deko3d_runtime.bin` = `<private>/eu-private/deko3d-out/gta4_deko3d_runtime.bin`
  (**99,569,920 bytes, SHA-256 `5139…1839`**; hash it on the card or the PC after the copy and compare), and
  `gta4_deko3d_glsl.bin` from the same folder (as in run 35).

One boot of 4 minutes, standard clocks (the protocol's three checks). Expected, in order, in `game.log`:
`null GPU: no rendering` → `deko3d: shader package …: 1912 shaders, 6285 DKSH` → `deko3d: GLSL package …: 1912
texts` → `deko3d: set-up: shader package loaded` → `deko3d: graphics system …` → `GPU system initialized` →
`Runtime initialized successfully` → the recorder's `creating the device`, `core ready`, `uam ready`,
`swapchain`, `recorder ready` → every 10 s the front end line and the two recorder lines (draws "drawn" =
recorded into a submitted command list; GPU execution is only evidenced by `libdeko3dd errors 0`, the completed
works and the PNG). Nothing is known about the last step: a refused package prints one `shader package …:` line
with the reason, then `the recorder did not start`.

Pass for this run (only what the code already logs): the package line with 1,912 / 6,285, `recorder ready`, no
`Unhandled Switch access violation`, `libdeko3dd errors 0`, and at least one `dk-f*.png` in
`sdmc:/switch/theft4/captures/` that is not black. Then stage 3 (`game-stage3.toml`) and `drawn` > 0. If stage 2
shows nothing or the log stops, `game-stage1.toml` says whether the output and the resolves work alone. Copy back:
`game.log`, `game.prev.log`, `game-crash.log`, `game-exit.log`, the captures. A single boot is a go/no-go, not a
rate (the export trace and debug log are on: do not read CPU numbers from it).

### G9. Next

If run 37 draws: counters for the front end (ns per `Submit`), fence and Present waits and the thread's core (the
audit of 2026-10-26: none exist), release NRO and library with tracing off, 3 repetitions per scene. If it crashes
again at the same place, G7's second row is refuted and the next build adds a `catch (...)`/`std::terminate` print
before anything else. Candidates after that, from the audit and not started: use the guest dirty words in the front
end, fuse the vertex byte-swap with the copy into the uncached ring, batch the front end → recorder hand-off.

## 2026-10-27: run 37 read, the queue's scratch memory (H)

Run 37 was the first hardware test of the deko3d recorder (stages 2 and 3, `theft4-dk-debug.nro`, `17f3a18-dirty`,
package v2). Both boots started, ran and exited without a crash.

### H1. What was examined, and what was not

Logs in `out/console-runs/run37-logs/debug_stage2/` and `debug_stage3/` (the folder names use `_`, not `-`): per
folder `game.log` (stage 2 2,499,993 bytes, stage 3 1,900,854), `game-exit.log`, `game-crash.log`. **There is no PNG,
no trace and no `.toml` in either folder, and none anywhere else on the PC** (searched Desktop, Downloads, Documents,
`out/console-runs`, the private tree): the log lists 41 captures for stage 2 and 18 for stage 3
(`sdmc:/switch/theft4/captures/dk-f*.png`), which were not copied back. **No image of run 37 has been seen by me**:
everything below about the picture is the user's description plus the logs and the code. The `game.toml` actually
used is not in the folders either; the log's own lines say `nx_gpu=deko3d nx_dk_capture_s=10` and the stage
(`stage 2/3 (nx_dk_stage)` in `recorder ready`) for each run.

### H2. VERIFICATO from the logs

- Stage 2: `libdeko3dd errors 0 warnings 0` to the end (uptime 442 s, `_exit(0)`, no faults), front end `rejected 0`,
  drawn draws only the non-depth-tested ones: `skipped by reason: 2=4684004` (reason 2 = *draws with a depth test
  before nx_dk_stage 3*) and `10=4` (vertex shader not in the package). **So "mostly lights, the rest black" in stage
  2 is what stage 2 is built to show**: every draw with a depth test, i.e. the whole G-buffer fill and the forward
  geometry, is skipped by `Draw()`. It is not evidence of a texture or shader fault. (Lights, post-processing and 2D
  are the draws without a depth test.)
- Stage 3: the same 0 errors / 0 warnings from libdeko3dd and `rejected 0`, but reason 2 is gone and nearly every draw
  is `drawn` (4 draws for reason 10; 52..114 by frame 4223 for reason 13, a shader variant still compiling).
  **Drawn means recorded into a command list that was submitted; it is not evidence that a pixel appeared.**
- Stage 3 works 3771 to 3804 (frame ~3770, ~85 s after the start): `deko3d: work N: the GPU has not finished slot K
  after 2 s` and `after 4 s` (55 warnings, log lines 10127-11055; none in stage 2); frames/s fall from 16.8 to 0.1-0.2
  with 500-3800 draws per frame, about 5 s per work whether the work has 522 or 2000 draws. **The stall ends by itself**:
  line 11342 runs 15.8 frames/s at 93 draws per frame and line 12325 27.7 frames/s at 160. From line 12518 the game runs
  at 1.0 frames/s with ~2700 draws per frame and **no** fence warning (each work under 2 s): a second, different slowness
  (CPU-side recording cost: `record` 187-204 us per drawn draw, 1899 textures created, not measured further). So the
  stall is episodic and content-dependent, not a permanent hang; libdeko3dd reports nothing in any of it. At the start of
  the episode the loaded shaders go 64 → 135 → 177 → 235 and the shader code from 462 KB to
  2400 KB (+1.9 MB in 71 shaders) and the textures from 52 to 312. The first warning comes ~18 log lines after
  `resolved texture 402182A0: 4096x4096` was created; stage 2 creates the same texture and does not stall.
- Stage 2 also slows at the end (5 frames/s at 2773-3511 draws per frame, 99 % of them skipped) with no GPU wait
  warning: that is the CPU-side cost of capturing every draw (a 480-register diff and a fixed-state decode per draw)
  and not the GPU. Performance is for later; recorded here only so it is not mixed up with the stall.
- Stage 3's `game-exit.log` stops after step 2 (+1160 ms) and `game-crash.log` is empty; stage 2's is complete (9 steps).
  Not explained; the user reports a clean exit.

### H3. The audit (six read-only auditors, one per dimension, then skeptics)

Dimensions: shader interface and constants, targets/clears/resolves, geometry and fixed state, textures and
samplers, synchronisation and the stall, shader selection and filtering; each against the Vulkan path of the same
game logic (`graphics_system.cpp` and the policy headers). Ruled out with evidence (details in the agents' reports, not
repeated): UBO bindings/sizes and every shared-block offset against the generated GLSL and `core/draw_state.h`
(1,912 shaders), constant endianness and order, front-end constant diffing, half-pixel convention, alpha-test
specialisation values, texture slot/type agreement (0 mismatches over 1,912 shaders), vertex element formats and
endian fixes, index/base-vertex maths, compare/stencil/blend tables, untiling and mip addressing, byte order of DXT
blocks, sampler tables, the fetch-constant layout, and the HDR resolve (`RGBA16F` → format 29 is a same-format copy,
the clamp to [0,1] that I suspected exists only for the three loading-screen resolves).

### H4. Ranking of what is left

| # | Candidate | What the evidence is | Confidence | Explains |
|---|---|---|---|---|
| 1 | **Queue scratch memory 2048 B, programs need up to 92,672 B** | offline, reproduced by me (below), by three auditors and six skeptic votes (none refuted the mismatch; all say the link to the stall is unproven) | high that the mismatch exists; medium that it corrupts stage 3 geometry; **low-medium** that it is the stall (the stall recovers by itself after 34 works, which a scratch overrun does not obviously do) | onset of the stall coincides with the first big vertex shaders; corrupt vertex output of skinned/state-machine shaders in stage 3 |
| 2 | `kDepthSurfaceHandoff` dropped (the persistent depth/stencil of the forward phase is never filled: stays 1.0/0) | code: front end drops it, title sends it every deferred phase, Vulkan path consumes it; not refuted by two skeptics | high that it is dropped; medium on the visible effect | stage 3 forward geometry, lights, sky, glass |
| 3 | `kRegisterVirtualResource` dropped (packed-depth alias texture not built) | code; one skeptic refuted the *mechanism* (the base fallback), the dropped command stands | medium | deferred lights read garbage depth: lights in the wrong place, stage 2 and 3 |
| 4 | Resolve colour exponent (`flags >> 26`) and per-target colour exponent not applied | PC log of the Vulkan path shows `exponent=1` on the HDR scene resolves; two skeptics refuted that it explains the symptoms (a power-of-two level is not black or glitched; render-target and resolve exponents may cancel) | low-medium | brightness at most |
| 5 | Resolve-attached clears incomplete (other aspect not cleared, whole surface instead of the rectangle) | code | medium | stale deferred depth, flicker |
| 6 | No depth clamp for z-fail light volumes / clip-disable draws | code | medium | local lights and shadows, stage 3 |
| 7 | Depth bias off below stage 4, slope term 16x | code | low-medium | z-fighting of decals |
| 8 | Missing vertex inputs bound as fixed attributes; LOD bias not delivered; R32F view swizzle; `texture.frame` set before a failed upload; pre-list copy without a leading barrier; rect-list corner choice | code | low | minor, each |

Final skeptic tally (30 agents, 2 per hypothesis): scratch mismatch not refuted (6 votes; "the link to the stall is not
demonstrated", counts of shaders differ from agent to agent, mine are the 27 programs of the package above); depth
hand-off not refuted (4 votes); packed-depth alias dropped command not refuted (3 of 4; the 4th refutes only the
mechanism through the base-address fallback, because no base lookup was seen in the log); depth clamp not refuted but
its visible effect unproven; bound-depth-smaller-than-colour refuted twice as a cause; base-address fallback unsound
refuted as a cause (latent only); "stall = fill-rate bound" refuted twice (the stall is not proportional to the draw
count; a few pathological draws or passes arriving with the new content fit better). The last point is the open
question for the stall.

### H5. The first defect proven offline: per-warp scratch memory (row 1)

deko3d gives every queue `perWarpScratchMemorySize` bytes of local memory per warp; the default is
`4 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT` = 2048 (`deko3d.h:289`). `Device::Create` (`dk_core.cpp`) never set it. A DKSH
declares what its program needs in `DkshProgramHeader::per_warp_scratch_sz` (byte 20 of the program header; uam's
`dksh.h`, `compiler_iface.cpp` sets it to `tlsSpace * 32`).

**VERIFICATO (by me, on the game's package):** `tests/deko3d/test_dksh_scratch.py --real <deko3d-out>` reads the
`per_warp_scratch_sz` of all 6,285 DKSH of `gta4_deko3d_runtime.bin` (SHA-256 `5139…1839`) with the C++ parser and,
independently, with Python: **27 programs are above 2048 bytes (all vertex programs), the largest needs 92,672 B, none
exceeds 96 KiB**, every value is a multiple of 512, the pixel programs all declare 0. The sister project already
hit the same limit (`thirdparty/plume-deko3d/plume_deko3d.cpp:834`, 64 KB with a comment that its biggest vertex
shaders need 32 KB). The timeline fits: the 27 programs are the skinned and state-machine vertex shaders, which stage
2 never draws (it skips every draw with a depth test) and stage 3 first draws at the moment the stall starts.

**DA VERIFICARE (hardware):** that a program running past the queue's scratch window really is what stalled the GPU
and corrupted the geometry. libdeko3dd cannot see a hardware fault, so `errors 0` neither supports nor refutes it;
nothing in the repo shows whether deko3d (debug or release) checks the DKSH scratch against the queue at bind time.
**STIMA:** if this is the cause, stage 3 stops printing `the GPU has not finished slot` and the skinned meshes stop
being spiky; if it is only part of it, the stall may remain (the episode of run 37 lasted 34 works and ended by itself,
so a run that merely shortens it proves little: compare the number of warnings, 55 in run 37) (the next suspects are in the table, plus the state-machine
loops, which the Vulkan path bounds with `native_spirv_loop_watchdog.h` and the deko3d GLSL does not).

### H6. The change (uncommitted, tree `17f3a18-dirty`)

One fix, three small supports:

- `deko3d/dk_dksh.h` (new): `kQueueScratchBytes = 96 KiB` (above the largest, a multiple of 0x200) and `ParseDksh`,
  header only, no Switch headers (so the PC can compile it).
- `dk_core.cpp`: `qm.perWarpScratchMemorySize = kQueueScratchBytes` in `Device::Create` (the log line now says `96 KiB of
  scratch per warp`); `Core::LoadShader` parses the program header, **refuses a shader whose scratch exceeds the queue's**
  (an error line naming it, counted in `scratch_refused`, the draw is skipped as "a shader without DKSH") and records the
  largest scratch loaded (`scratch_max`).
- Diagnostics, no behaviour change: the recorder's second report line ends with `scratch per warp <max> of 98304 B (N
  refused)`; the front end line ends with `commands dropped: depth hand-offs, virtual resources, other`; a new line
  `deko3d: known gaps seen (...)` counts depth-clamp draws, z-fail light volumes, depth bound without a test, target
  binds of unequal size, vertex inputs without an element, draws with a LOD bias, textures found by base address (and
  of those the wrong type), resolves with a colour exponent, resolve clears of the other aspect, and the milliseconds
  spent waiting for slot fences. A count says which candidate of H4 to test next; it is not a visible defect.

Memory: 96 KiB per warp instead of 2 KiB; deko3d sizes the queue's scratch from it (the exact total is deko3d's, not
measured: the log's `deko3d blocks` line does not include it). The process had 1.5 GiB of headroom in run 37 (`heap
arena ... 1559 MiB headroom`), so an estimated few tens of MiB are expected to fit; run 38 reads the headroom line.

### H7. Offline tests

`python tests/deko3d/test_dksh_scratch.py --real C:\dev\Theft4\eu-private\deko3d-out` (`THEFT4_VS_ROOT=D:\Microsoft Visual
Studio`): 14 synthetic parser cases (zero, 2048, 92,672, exactly the queue's, over it, two programs, no programs, bad magic,
truncated, empty, field at the last word of the control section, one byte past it, huge offset, control past the file)
all as expected; on the real package C++ and Python agree: 6,285 blobs parsed, 0 failed, 27 over 2048, 0 over the queue,
maximum 92,672. The recorder itself cannot be run on the PC (deko3d); the build is the only check of the rest.

### H8. Build

`tools/switch-game/build_variants.sh out/console-runs/run38-build dk-debug` (`-j3`, no other `ninja`/`cc1plus` running
before it): 14 steps, **exit 0, no compiler warning** (the only CMake warning is the known `rex_version` one for a dirty
tree). `COMMIT` = `17f3a18-dirty`.

| File (`out/console-runs/run38-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` ("Theft4 DK (debug)") | 46,856,389 | `7d19288416b2cec965fd4e68e87c59ab6879260c1975fd5cba96b5f12272c942` |
| `switch-game-dk-debug.elf` (for addr2line) | 313,945,184 | not hashed |
| `game-stage2.toml`, `game-stage3.toml` | copies of `run37-build/`'s, unchanged | |

The new log strings are in the ELF (`of scratch per warp`, `known gaps seen`, `not loaded`). Only the debug deko3d
variant was built; `theft4-dk.nro` and `theft4.nro` (NVK) in `run37-build/` do **not** have this change, and
`native_front_end.cpp` (shared with the NVK configuration) was touched, so an NVK NRO built from this tree would differ
from `run37-build/theft4.nro` by the three new counters only. VERIFICATO: the build; DA VERIFICARE: everything on the
console.

### H9. Run 38: the next single hardware test

- NRO: `out/console-runs/run38-build/theft4-dk-debug.nro` ("Theft4 DK (debug)"), stage 3 first
  (`out/console-runs/run37-build/game-stage3.toml` as `game.toml`, unchanged). Copy `game.log`, `game.prev.log`,
  `game-crash.log`, `game-exit.log` **and the whole `captures/` folder** (the captures are what I could not see in run 37).
- Same package files (hash `5139…1839`), cache and clocks as in the protocol.
- Read, in order: (1) the queue line `96 KiB of scratch per warp` and `scratch per warp N of 98304 B (0 refused)`;
  (2) whether `the GPU has not finished slot` is gone (55 in run 37) and `waiting for slot fences` stays small;
  (3) the `known gaps seen` line, whose non-zero counters choose the next fix among H4's rows 2-7; (4) the `commands
  dropped` counters (row 2 and 3 are confirmed as live if the hand-off count is > 0 per frame); (5) the captures.
- If the stall remains: the next build needs a cvar that skips the draws of the 27 programs over 2048 B (not written:
  it would be a second change); if the stall disappears with them skipped, the programs are the cause and the queue
  size alone was not enough (loop runaway in the state-machine shaders is then the candidate); if it stays, per-work
  GPU timestamps around the G-buffer, lights, resolves and Present are the next instrument (the core has a query block
  and no timestamps yet).
- Stage 2 is unchanged by this fix (it never draws those programs): do not use it to judge the fix.


## 2026-10-27: run 38 read, the packed depth alias (I)

Run 38 = `run38-build/theft4-dk-debug.nro` (queue with 96 KiB of scratch per warp), stage 3, `17f3a18-dirty`, same package v2.
The logs and **the captures** are in `out/console-runs/38-logs/` (the folder is called `38-logs`, not `run38-logs`):
`game.log` 1,682,541 bytes, `game-exit.log`, `game-crash.log` (0 bytes), `captures/` with 18 PNGs of 1280x720. Original
files untouched.

### I1. What was examined, and what was not

- Opened and looked at (11 of the 18 PNGs): `dk-f000282`, `f003094`, `f003646`, `f003817`, `f003819`, `f004139`,
  `f004150`, `f004171`, `f004191`, `f004201`, `f004211`. Not opened: `f000881`, `f001480`, `f002080`, `f002680`,
  `f003480`, `f004160`, `f004181` (the first four are in the logo/video part, before the game starts).
- `game.log` read in the places that matter (the `deko3d:` report lines in full, the stall windows, the resolve and
  target creation lines, the CPU and audio lines of the interval reports). Not read line by line (1.7 MB).
- Run 37's folders (logs only, no images) for the comparison. Nothing was run on the console by me.

### I2. VERIFICATO from the captures

- Start: frame 282 is the Rockstar logo, correct; frame 3094 is black with `STARTING A NEW GAME...` bottom right,
  correct; frame 3819 is black with `LOADING...`, correct. So the 2D path, textures, the output pass and the loading
  screens work.
- The game: from frame ~3817 the 3D scene is on screen and **recognisable**: a dockside/warehouse street, a taxi seen
  from behind with its red tail lights, buildings with correct window textures (a wooden warehouse facade lettered
  `STATE DELIVERY`, a `BLDG 269` sign), the HUD radar with the yellow route, the money counter, the mission text
  (`ROMAN'S TAXI`, `Drive to Roman's place.`). Textures are therefore found, untiled, byte-swapped and bound; the vertex
  data of the world is mostly right (the shapes are right).
- What is wrong, in every game frame I opened:
  1. **The lighting.** Large areas are black (the asphalt in front of the car, the car body, the ground at the left),
     other surfaces are blown out towards white with a blue/cyan cast, and the lit areas have the outline of **wedges
     and cones**: textured concrete in a wedge on a black ground, translucent cones in the dark frames (`f004201`,
     upper right), a large white wedge at the right of `f004191`. It reads as light volumes whose coverage is wrong, not as
     missing geometry.
  2. **The brightness changes from frame to frame**: `f004139`, `f004171` and `f004201` are almost black, `f004150`,
     `f004191` and `f004211` are strongly over-exposed, 10 frames apart, same place.
  3. Subtitle letters missing, different ones in different frames (`Drive to Ro  a  s p ace.` in `f004191` and `f004211`,
     `Drive to Roman s place.` in `f004201`). A font/texture refresh problem in a 2D draw; not investigated.
- The black of run 37 stage 2 ("only lights visible") is explained in H2 (draws with a depth test are skipped by design);
  the stage 3 picture of run 37 was described by the user as "more scene, badly glitched" and run 38's is the same by
  the user's own account: **the scratch memory fix did not change what is drawn**.

### I3. VERIFICATO from the log, run 38 against run 37

| | run 37 stage 3 | run 38 |
|---|---|---|
| libdeko3dd errors / warnings, front end rejected | 0 / 0 / 0 | 0 / 0 / 0 |
| queue scratch, largest program loaded | 2048 B vs 92,672 B | 98,304 B (log: `96 KiB of scratch per warp`), 92,672 B, **0 refused** |
| the stall (GPU slow, fps 0.1-0.2) | works 3771-3804, **5 s per work** (`has not finished slot` after 2 s and 4 s, 55 warnings) | frames 3791-3810 (log lines 10153-10359): 0.6 fps, **~1.5 s per work**, **0 warnings** (each wait is under the 2 s threshold) |
| time waiting for slot fences (counter) | not counted | 46 ms before the stall; 1,509 ms → 10,495 → 19,080 → 29,076 → 32,121 ms over the four reports of the stall; it does not move after (30.6 s waited in ~45 s) |
| after the stall | 1.0 fps, ~2700 draws per frame | the same: 0.9-1.0 fps, 2700-3000 draws per frame, no fence wait |

So: the stall is **reproducible at the same point** (frame ~3790, with the 64 → 135 → 150 → 241 shaders and 52 → 312 →
1382 textures of that stretch) and ends by itself. With the queue fixed each work takes about a third of the time
(~1.5 s against ~5 s), but it does not disappear. VERIFICATO: the numbers. STIMA: the scratch mismatch accounted for
part of the stall's GPU time (run 37's frames also had more draws: up to 2000 per work, against 522-564 here, and the
per-work time was independent of the draw count, so the comparison is weak); the cause of the remaining ~1.5 s per work
is **not identified**. What the counters show in those windows only: z-fail light volumes 95 → 474 (+380 in four
reports, ~24 per frame, against 0 in the minutes before), `vertex inputs without an element` 8 → 84.

### I4. VERIFICATO from the log: the depth is read as bytes

- `textures found by base address`: **4,703 at the last report, of which "the wrong type" 4,702** (a depth image
  returned for a colour fetch, or the reverse). They start with the first 3D resolves (118 at the first scene report) and
  grow about 900 per 10 s of play. Run 37 could not count them.
- The resolves of depth are `guest format 23` (D24FS8, Xenos `k_24_8_FLOAT`), host `ZF32_X24S8`: e.g. `resolved
  texture DA1A6320 at 0608F000: 1280x720, guest format 23, host format 45`.
- The title samples such a resolve through **a second texture object with an A8R8G8B8 fetch constant at the same base
  address** (its `sub_828D9768`; the front end receives it as `kRegisterVirtualResource` with `packed_depth_source`, 168
  of them in the run, and drops it: `commands dropped: ... virtual resources 168`). On the Xenos this is memory aliasing:
  the bytes read are the resolve's `(depth24 << 8) | stencil` word. The Vulkan path builds that texture with
  `packed_depth_alias_ps.glsl` (`RecordPackedDepthAlias`); this recorder had no equivalent, found the depth image by base
  address and handed the shader **the float depth in all four channels**. Every shader that unpacks depth from those
  bytes (the deferred lights, whose z-fail volumes also grow in the logs) reads a wrong depth.
- STIMA: that wrong depth explains the wedge-shaped lit areas, the black ground and the blown-out surfaces of I2 (the light
  of each volume is computed from a depth that is not the scene's). It does not by itself prove the sub-pictures of I2
  point 2 (the frame to frame brightness: the exposure chain of 640x360 → 1x1 `R32F` resolves, lines 9660-9691, reads
  this lighting) and 3 (the subtitle).

### I5. Ranking after run 38

| # | Candidate | Evidence | Status |
|---|---|---|---|
| 1 | **Packed depth alias missing** (H4 row 3) | 4,702 of 4,703 base-address lookups are a depth/colour mix-up; the picture is wrong in the way wrong-depth lighting looks; the Vulkan path has the conversion | **fixed in this change**, DA VERIFICARE on the console |
| 2 | Depth hand-off dropped (H4 row 2) | `commands dropped: depth hand-offs` grows by ~1 per frame (198 → 360 over 160 frames); it feeds the forward phase's depth only | not touched; the next candidate if I4's fix leaves the forward objects wrong |
| 3 | Resolve colour exponent (H4 row 5) | 1,303 resolves with a non-zero exponent by the last report; the value was never logged | **instrumented only**: the first 16 distinct (exponent, format) pairs are logged, "not applied" |
| 4 | The per-work GPU time at the stall | I3 | **not identified**; next instrument: per-work GPU timestamps (the core has the query block, no timestamps yet) |
| 5 | The scratch memory (H4 row 1) | I3 | kept: 0 refused, run 38 stall 3x shorter per work; not the cause of the picture |
| 6 | CPU cost of the recorder | I6 | measured by section, below |

### I6. Performance and audio (CPU), from the log: what is measured and what is not

- In play (frames 4133-4211) the game runs at 0.9-1.0 frames/s with 2700-3000 draws per frame. The cumulative
  `record N us per drawn draw` goes 154.75 → 159.65 µs between two reports (626,554 → 652,768 drawn draws): the
  interval's own mean is **(652,768 × 159.65 − 626,554 × 154.75) / 26,214 = 277 µs per drawn draw**, i.e. **~7.3 s of the
  ~10 s window are spent inside `DkRecorder::Draw`** (it was 30 µs per draw at 316 draws per frame, frame 3642). VERIFICATO
  by that arithmetic from the report's own numbers. No GPU wait in those windows (the fence counter does not move).
  So in play the frame rate is limited by the recorder's CPU time on the CPU, not by the GPU.
- The audio: `output ran dry` 505-518 times per 10 s window and "frames in the window" 30-32 % of real time while it is
  1 fps (it was 39-66 % at 13-60 fps, and 37-43 % at 1.5 fps in run 30, before any deko3d). STIMA: the title's audio
  engine is paced by the main loop, so the crackle follows the frame rate; nothing in the log says the audio thread is
  starved (guest threads 142 %, host 29 % of 400 %). DA VERIFICARE: repeat the audio test only when the frame rate is up.
- **Where the 277 µs go is not known.** Nothing is optimised in this change; the recorder now reports its time per drawn
  draw split into textures (including the new alias pass), vertices and indices (the copy into the upload ring with the
  endian fixes), constants, and the rest, per report interval (`recorder time per drawn draw in the interval`). The
  suspects, from reading `Draw()`, are the per-draw copy of each stream's range into uncached memory
  (`CopyGuestWords` and the per-vertex fix-up loop), the per-draw texture work and the 480-register diff; not measured.

### I7. The change (uncommitted, tree `17f3a18-dirty`)

Evidence: I4. Cause: the recorder returns a resolved **depth image as a float** for an **A8R8G8B8 fetch of the same memory**.
Expected: the lights' depth reads become the title's packed bytes, as on the Vulkan path, so the lit areas follow the
scene. Distinguishing test: the captures of run 39 (the ground lit, the wedges gone) and the `packed depth alias` report line
(conversions > 0, `other depth/colour mix-ups` ~0); a failure would be unchanged pictures with conversions > 0 (then the
swizzle, the stencil plane or the depth encoding is wrong, and the log gives the sizes and formats to check).

- `deko3d/dk_depth_alias.h` (new, no Switch headers): the decision `IsPackedDepthFetch` (the image is depth, the fetch
  format is 6 or 50, same width and height), the C++ mirror of the pass's arithmetic (`Float32To20e4`, `PackDepthStencil`)
  and the two GLSL sources (a full-screen triangle; a pixel shader that writes `(depth24 << 8) | stencil` as RGBA8 bytes,
  with the 20e4 float encoding for guest format 23 and plain 24-bit rounding for 22; the same `depth20e4` text as
  `packed_depth_alias_ps.glsl`).
- `dk_recorder.cpp/.h`: `PrepareTexture` asks `IsPackedDepthFetch` when `ResolvedTexture` finds a depth image (by handle
  or by base address, now returning the destination object); `PackedDepthAlias(object)` keeps one RGBA8 image per
  resolved depth, converts it with a full-screen pass when the depth was resolved again since (`Resolved::writes`) or its
  image was re-created, and returns it; the fetch's own swizzle is applied by the sampled view, as for any texture (the
  Vulkan path applies it in the pass; the view of the alias there is identity: same result, `0x60A` puts the stencil in
  blue); point sampling; the stencil plane is read through a `DkDsSource_Stencil` view (`ViewOf(..., stencil)`). The
  pass is compiled by the console's uam at start like the output pass, and is not fatal: without it the old behaviour
  stays and the report says `NOT available`. Aliases are freed with their resolved depth (`ResourceEvent`).
  The pass sets the state it needs and calls `ForgetState()`, as the target creation and the resolve clears do.
- Instrumentation only, no behaviour: the first 16 (exponent, format) pairs of resolves with a colour exponent are
  logged; `recorder time per drawn draw in the interval` (I6); a `packed depth alias` report line (conversions, reuses,
  failures, other mix-ups, live aliases).
- What the change does **not** do: the depth hand-off, the virtual-resource registration itself (the alias is found by the
  fetch, which is what the Xenos does), the colour exponent, the depth clamp, the stall, any optimisation.
- Risks, DA VERIFICARE on the console: (a) the stencil plane of a copied `ZF32_X24S8` resolve may not be what the guest wrote
  (the copy engine copies the image; if the stencil is 0 the blue channel is 0); (b) `usampler2D` of a stencil view under
  uam has never run here; (c) the pass runs in the middle of `Draw()`'s texture preparation (before any state of the draw
  is emitted, then `ForgetState()`): the same pattern as `Surface()` but not exercised; a hang or a libdeko3dd error would
  appear in the first report.

### I8. Offline tests (VERIFICATO)

`python tests/deko3d/test_depth_alias.py` (host clang of `THEFT4_VS_ROOT`, the PC `uam.exe`):
- the decision: 9 of 9 cases (a colour fetch of a depth of the same size: yes, formats 6 and 50; depth fetches, colour of
  colour, other colour formats, other sizes, zero size: no);
- 13,030 packed words (edge values and a fixed pseudo-random set, both depth formats, five stencil values): the C++
  and an independent Python version agree on all of them;
- semantics: the 20e4 encoding is monotonic over [0, 1], decodes (Xenia's `Float20e4To32` written from the format) to
  within 8.3e-7 relative (limit 2^-17), 1.0 is `0xF00000`, 2.0 is `0xFFFFFF`; the bytes are R stencil, G/B/A depth bits 7..0 /
  15..8 / 23..16 and the title's swizzle `0x60A` puts the stencil in blue;
- `depth20e4` of the pass is the same text as `packed_depth_alias_ps.glsl`;
- both shaders compile with the PC uam (pixel 1024 bytes, vertex 768 bytes).
The two earlier tests still pass (`test_dksh_scratch.py` 14/14, `test_runtime_package.py`). **What no offline test can
show**: that the pass, the stencil view and the sampling work on the console.

### I9. Build (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run39-build dk-debug` (-j3, one build, no other `ninja`/`cc1plus`
before): exit 0, no compiler warning (the known `rex_version` dirty-tree CMake warning only). The first build of this
session started before the timing instrumentation was added and was discarded; the NRO below is the second one, from the
final sources. `COMMIT` `17f3a18-dirty`. `theft4-dk-debug.nro` 46,864,581 bytes, SHA-256
`b98dd44be1c6242279feb8ef846d47824b45fc866636d55fe75e7f57f0a60968`; `switch-game-dk-debug.elf` 314,221,504 bytes
(for `addr2line`); the new log strings are in the ELF. `game-stage3.toml` and `game-stage2.toml` copied from `run38-build`
(unchanged). `theft4-dk.nro` and the NVK `theft4.nro` were **not** rebuilt. Nothing run on the console.

### I10. Run 39: the next single hardware test

- NRO `out/console-runs/run39-build/theft4-dk-debug.nro` as `sdmc:/switch/theft4/` NRO, `game-stage3.toml` as `game.toml`
  (`nx_gpu = "deko3d"`, `nx_dk_stage = 3`, `nx_dk_capture_s = 10`, `nx_dk_gamma = 1.0`), the same `deko3d/` package files
  (`gta4_deko3d_runtime.bin` SHA-256 `5139…1839`, `gta4_deko3d_glsl.bin`). Rename the old `game.log` and `game-crash.log`
  first, and delete the whole folder `sdmc:/switch/theft4/runtime-cache` (run-protocol check 2; the screen must say
  `cache files: runtime-cache 0`). There is no deko3d shader cache to delete: the shaders come from `deko3d/`, which stays. Play to the first mission screen (as run 38, ~4 minutes), then hold + to exit.
- Copy back `game.log`, `game.prev.log`, `game-crash.log`, `game-exit.log` and the **whole `captures/`**.
- Read, in order: (1) `packed depth alias pass compiled by the console's uam` at start (or the error); (2) the `packed depth
  alias` report line: conversions > 0 and the wrong-type count of `textures found by base address` no longer growing in step
  with it (the counter still counts base lookups; `other depth/colour mix-ups` should stay small); (3) `libdeko3dd errors`
  (a pass that breaks the state would show up here first); (4) the captures from frame ~3800 on: is the ground lit, are
  the wedges gone, is the brightness steady; (5) `resolve into ... with colour exponent` (the values for row 3);
  (6) `recorder time per drawn draw in the interval` (where the 277 µs go); (7) whether the stall windows (frames ~3790-3810)
  are still there and `waiting for slot fences`.
- If the picture is unchanged although conversions > 0: do not change anything else before looking at what the pass writes
  (a capture of the alias image, or a log of one texel); if it is partly better, row 2 (the depth hand-off) is next.

## 2026-10-27: run 39 read (J): the alias never ran, the exit hang, the cost of the vertex copy

Run 39 = `run39-build/theft4-dk-debug.nro` (SHA-256 `b98dd44b…0968`, packed depth alias pass, per-section timers), stage 3,
`17f3a18-dirty`. Logs and captures are in `out/console-runs/39-logs/` (`39-logs`, not `run39-logs`): `game.log`
1,625,280 bytes, `game-exit.log` (182 bytes, 2 steps), `game-crash.log` (0 bytes), `captures/` with 14 PNGs. Original
files untouched. Branch `sync-clean`, HEAD `17f3a18`, the tree has only the uncommitted work of this series.

### J1. What was examined

- Opened (7 of 14): `dk-f003625` (black), `f003786` (the dark intro scene), `f003788` (`LOADING...`), `f004130`, `f004140`,
  `f004150`, `f004159`. Not opened: `f000277`, `f000875`, `f001472`, `f002075`, `f002671`, `f003068`, `f003456`.
- `game.log`: every `deko3d:` report line, the compile and teardown lines, the exit lines, the cache line at start.

### J2. VERIFICATO: the alias pass compiled but did not run once

- `packed depth alias pass compiled by the console's uam in 25.6 ms` (log line 109); `libdeko3dd errors 0 warnings 0` to the end.
- Every report line: **`0 conversions, 0 fetches served by one already converted, 0 failed, 0 aliases live`**, while
  `other depth/colour mix-ups` grows to **1,661** (the by-base lookups of the wrong type: 1,662). So the fix of
  run 39 was in the NRO and was never executed: the picture of run 39 is the picture of run 38 (the captures agree:
  same dark frames with white cones, same over-exposed frames with a cyan cast, same black ground, same missing glyph
  in `ROMAN'S TAXI`). **The depth alias hypothesis is neither confirmed nor refuted by run 39**; what was refuted
  is the detection I wrote (a depth image, an A8R8G8B8 fetch **and the same size as the fetch constant says**).
- Why the size test could not hold (VERIFICATO from the log and the hooks): the log creates, at the start of the scene,
  `texture 1F1B9000 32x32 format 6 tiled pitch 32 swizzle 60A`, and the same base `1F1B9000` is `resolved texture
  40079D50 ... 1280x720, guest format 23` (the scene depth). `swizzle 60A` is the alias swizzle the Vulkan shader names
  (stencil in blue); `format 6` is A8R8G8B8. That is the title's alias header (sub_828D9768's 52-byte texture header:
  base = the depth's base, size = a 32x32 placeholder) as the recorder saw it **before** the depth existed, and its
  fetch constant says 32x32 where the depth is 1280x720. The Vulkan path does not compare those: it asks the title's own
  registration (`kRegisterVirtualResource` with `packed_depth_source`, 155 of them by frame 276, dropped by this front
  end) and compares the registered logical size.

### J3. The captures of run 39 against run 38 (VERIFICATO as pictures; STIMA as causes)

Same content frame for frame: the taxi at the same place (the game is at 1 fps and has no input), alternating dark
frames (`f004130`, `f004159`: black ground and car, a white wedge and cone of light from the headlights, orange sparks)
and over-exposed frames (`f004140`, `f004150`: pale-blue concrete, white and cyan buildings, black sky and black ground
beyond the car, a red tail-light halo, a large white wedge at the upper right of `f004140`). Subtitle: `ROMAN'S T▲XI`,
the `A` is a dark triangle (a glyph that is not drawn or is replaced). The shapes of the world are right; the light is
wrong in a way that follows light volumes (cones, wedges), and the exposure changes between two captures ten frames apart.
STIMA: the light volumes read a wrong depth (J2) and the exposure of the adaptation chain follows the lighting;
nothing in run 39 tests that.

### J4. VERIFICATO: the recorder time, by section (the new timers)

Interval means per drawn draw, in play (frames ~4133 on, 2,700-3,000 draws per frame, 0.8-1.0 frames/s):

| window | draws | total | textures | vertices and indices | constants | the rest |
|---|---|---|---|---|---|---|
| 11658 | 36,513 | 48.7 µs | 5.4 | 24.6 | 1.2 | 17.4 |
| 11886 | 30,006 | 244.2 | 5.7 | **179.4** | 3.0 | 56.1 |
| 12080 | 29,022 | 251.2 | 5.2 | **171.0** | 3.0 | 72.1 |
| 12169 | 27,498 | 254.2 | 5.5 | **174.1** | 3.1 | 71.5 |
| 12388 | 25,159 | 274.0 | 5.8 | **184.7** | 3.2 | 80.3 |

- **The copy of the vertex and index ranges into the upload ring is ~70 % of the recorder's time in play (172-185 of
  251-274 µs)**; textures are 2 % and constants 1 %. The recorder works ~7 of every 10 s at 1 frame/s: the frame rate in play
  is the recorder's CPU time (VERIFICATO: sums of the report lines), not the GPU (the fence wait does not move after frame ~3810).
- In the stall windows (frames 3790-3810, 0.6 frames/s) "the rest" is 2,000-3,000 µs per draw: the wait for a free work
  slot (`waiting for slot fences` 2,950 → 31,556 ms): the GPU. Same place and cost as run 38 (32,121 ms).
- The 2D start (frames 0-3000) costs 5-60 µs per draw; before the stall at frame ~3640 98 µs (vertices 61).
- STIMA, from reading `Draw()`: the vertex fix-ups (16-bit pairs, DEC3N normals, colours) read and rewrite 4 bytes each
  **in the upload ring, which is CPU-uncached** (`DkMemBlockFlags_CpuUncached`), for every vertex of the draw's index range;
  each read of uncached memory is a bus round trip. Not measured as such: the timers cannot separate the copy from the fix-ups.

### J5. VERIFICATO: why `+` does not leave

- `game-exit.log`: step 1 (chosen, `+` held 1 s) and step 2 (`file operations in flight: 0`); steps 3-9 never appear.
  `game.log` after step 2: `exports called at exit`, ..., then `deko3d: teardown: recorder` (line 12398) and
  `deko3d: teardown: uam thread` (12405), and **never** `teardown: device` or `teardown done`. The audio keeps running
  (it is a host thread) and the screen is black because the recorder, its images and the swapchain are already destroyed.
- `ExitGame()` calls `g_dk_graphics->Stop()` before the cooperative exit of the threads. `Stop()` joins the recorder thread,
  whose last step is `compiler.reset()`, and `Compiler::~Compiler` joins the uam thread **after the compile in progress**
  (`threadWaitForExit`, no timeout). From frame ~4143 the report says `4 compiling` and does not change to the end
  (draws skipped for reason 13: 46 → 365): the console's uam is at one of the variants the PC could not make, on core 2 at
  priority 0x3B while the guest threads (140-235 % of 400 %) run (run 35: 5-30 s per variant even then). Stage 2 never
  compiles anything and exits through all 9 steps; stage 3 of runs 37, 38 and 39 all stop at step 2 (VERIFICATO for 38 and 39
  from their logs; for run 37, "not explained" in H2, this is the same symptom: STIMA, its `compiling` count was not rechecked).
- Not an exit-key problem, not the swapchain, not a deadlock in the recorder: a join without a limit on a long compile.

### J6. Caches (VERIFICATO from the code and the log's own line)

`game.log` line 43: `cache files: runtime-cache 3 (11.9 MiB), of which shaders 3 (11.9 MiB); cache 16 (618.8 MiB), cache1 36
(217.0 MiB); runtime-cache NOT EMPTY`. The cache folder was **not** empty in run 39 and nothing in the deko3d build
reads it, so it did not matter:
| path | who reads/writes it | in the deko3d build | cleared automatically? | safe to delete |
|---|---|---|---|---|
| `sdmc:/switch/theft4/runtime-cache/` (+ `shaders/`) | `rex::Runtime`'s cache root; the Vulkan shader storage (`nx_shader_storage`, `InitializeShaderStorage`, `#if THEFT4_NX_VULKAN`) and NVK's pipeline cache | **not used** (`REXGLUE_SWITCH_NO_NVK=ON`: the block is compiled out); the three files are left from NVK runs | no: the start only counts the files and warns when there are any | yes; it only costs the NVK build a slower first start |
| `sdmc:/switch/theft4/cache/`, `cache1/` | the guest's `cache:` and `cache1:` devices | **not mounted** (`nx_mount_cache = false`, log line 102); 16 and 36 old files | no | yes, nothing reads them with the default |
| `sdmc:/switch/theft4/deko3d/gta4_deko3d_runtime.bin`, `gta4_deko3d_glsl.bin` | `dk_shader_package.cpp` (the recorder's package) | **required** (log lines 78-80) | no | **no**: it is the renderer's input |
| `sdmc:/switch/theft4/deko3d/gta4_deko3d_shaders.bin` | `nx_deko3d.cpp` (the M2 test pattern and its background compile, `nx_dk_compile_s`) | not opened by the recorder path (`graphics system: ... the null GPU keeps the guest GPU services`) | no | not needed for the recorder; kept (rule: no deletion without proof, and the M2 path still names it) |
| `sdmc:/switch/theft4/captures/` | the recorder's PNG writer (`dk-fNNNNNN.png`, names by frame number) | written every `nx_dk_capture_s` seconds | no: old files stay and a new run adds its own | delete or move before a run so the folder holds that run only |
| `sdmc:/switch/theft4/user`, `saves`, `marketplace` | the runtime's profile and saves | used | no | **no** (the game's progress) |

The answer to "which cache do I delete": **none is needed for this test.** The `runtime-cache` of run 39 was not empty and
the run behaved as run 38, which had the same line.


### J7. The next obstacle, and why this one (priority, not a list)

Run 39 leaves three facts: (1) the lights are wrong and the depth alias is the best explanation, but nothing has tested it,
because the detection never fired; (2) in play the recorder's CPU time (~250-270 µs per draw, 70 % of it the vertex and
index copy) holds the game at ~1 frame/s; (3) `+` hangs in a join. A correct picture is worth nothing at 1 frame/s, and a
fast wrong picture is worth nothing either, so the order is: make the alias run (correctness test), take the measured
dominant CPU cost out (frame rate), and let the game leave (needed for every test).
- The stall at frame ~3790-3810 (GPU, ~1.5 s per work, 20 frames) is not first: it is episodic and ends, and nothing
  measures it; the next instrument for it is per-pass GPU timestamps (not written; it would be a second change in the same run).
- Shaders: `4 compiling` for the whole stage and 365 draws skipped (reason 13) out of 653,000: not what makes the picture wrong.
- Not tried: FSR1, ZCULL, shadows, reflections, the depth hand-off (1 per frame is still dropped), the exponent (one pair only
  in the log: exponent 1 into guest format 29, `flags 04000010`, which does not look like the cause of a 2x-exposure error
  because the fetch side's `exp_adjust` is also not applied: they cancel; DA VERIFICARE).

### J8. The changes (uncommitted, tree `17f3a18-dirty`)

1. **Alias through the title's own registration (correctness).** Evidence: J2 (0 conversions; the placeholder 32x32 fetch;
   155 registrations dropped). `native_alias_registration.h` (new: `IsPackedDepthAliasRegistration`: texture kind, a
   source that is another object), `native_front_end.cpp` passes such a registration to the recorder as a
   `RecorderResourceEvent` with the new field `packed_depth_source` (`native_recorder.h`; the other registrations are still
   counted and dropped), `dk_recorder.cpp/.h`: `alias_sources_` (alias object → resolved depth object), `PrepareTexture`
   serves a fetch of a registered alias through `PackedDepthAlias(source)` (the size of the **depth**, not the fetch's),
   erased when the title releases the alias. The by-address route stays as it was (same-size test). Expected: `title
   registrations` > 0 and `fetches served through one` > 0 in the report line, `conversions` > 0, and the wrong-type count
   of base lookups falling to what is left. The log now says what the remaining mix-ups are: the first 16 distinct fetch
   handles with base, format, fetch size, swizzle, the object found, its size and kind and guest format.
2. **Vertex and index data converted in cached memory (frame rate; no change of the data).** Evidence: J4. In `Draw()` the
   draw's vertex range is converted (word swap, then the per-vertex fix-ups that read and rewrite the same words) in
   `vertex_scratch_` and copied once into the upload ring; the indices likewise (`index_scratch_`). Same bytes in the
   ring as before. The timers now split `vertices`, `indices`, `index range scan`, `constants`, `textures`, `the rest`.
   Expected, STIMA: `vertices` falls from ~170 µs to a fraction (the copy remains), the frame rate in play rises in
   proportion; if it does not, the cost is not the uncached reads and the split timers say which part it is.
3. **The exit (stability).** `Compiler::~Compiler` waits at most 3 s for the uam thread (`svcWaitSynchronizationSingle`);
   if a compile is still running it logs `the uam thread is still compiling after 3 s`, keeps the thread's data
   (`data_.release()`: the thread owns it) and returns, so the recorder's teardown reaches `device` and `done`, `ExitGame`
   continues with steps 3-9 and `exit(0)`. Risk, DA VERIFICARE: the process ends while that thread is still inside uam
   (as mode A leaves the game's threads running; "The software was closed because an error occurred" at that moment
   would be this). It does not call `svcExitProcess`, does not kill or pause a thread.
Not changed: shaders, textures, depth hand-off, synchronisation, NVK.

### J9. Tests and build

- `python tests/deko3d/test_depth_alias.py`: **OK** (VERIFICATO): decision 9/9, **registrations 6/6** (the title's real
  `RegisterVirtualResourceCommand`, compiled on the PC), 13,030 packed words equal between C++ and Python, the 20e4 semantics,
  `depth20e4` the same text as the Vulkan shader, both shaders compile with the PC uam.
  No offline test exists for items 2 and 3 (the vertex copy is inside `Draw()`, the exit is a Switch join): they
  are checked by the next run's timers and exit log.
- Build `tools/switch-game/build_variants.sh out/console-runs/run40-build dk-debug` (-j3, one build, nothing else running):
  exit 0, no compiler warning (the known `rex_version` CMake warning only). `theft4-dk-debug.nro` 46,872,773 bytes, SHA-256
  `2609128a86f9e8ebbaeaa0448b8998923f4b23a5b0cf97e15694f856410d2265`; `switch-game-dk-debug.elf` 314,316,960 bytes; `COMMIT`
  `17f3a18-dirty`; the new log strings are in the ELF. `game-stage3.toml` (unchanged) and `game-stage2.toml` copied in.
  `theft4-dk.nro` and the NVK NRO not rebuilt. Nothing run on the console.

### J10. Run 40: the next single hardware test

- NRO `out/console-runs/run40-build/theft4-dk-debug.nro`, `out/console-runs/run40-build/game-stage3.toml` as `game.toml`
  (`nx_gpu = "deko3d"`, `nx_dk_stage = 3`, `nx_dk_capture_s = 10`, `nx_dk_gamma = 1.0`), the same `deko3d/` package files.
  No cache has to be deleted (J6). Rename the old `game.log` / `game-crash.log` / `game-exit.log` and empty `captures/`, so the
  folder holds this run only. Same length of play as run 39 (~4 minutes after the mission screen), then hold `+`.
- Copy back `game.log`, `game.prev.log`, `game-crash.log`, `game-exit.log`, `captures/`.
- Read, in order: (1) the `packed depth alias` report line: `title registrations`, `fetches served through one`, `conversions`
  must be > 0, and the log lines `packed depth alias ... registered by the title`; if they are 0, the registration does not arrive
  (then `commands dropped` and `packed depth alias registrations passed on` say whether the front end sees it);
  (2) the `depth/colour mix-up: fetch handle` lines (what is left); (3) `libdeko3dd errors`; (4) the captures from frame ~3800:
  ground lit or black, cones, steady brightness; (5) `recorder time per drawn draw` (is `vertices` down from ~170 µs; the
  frames/s in play against ~1.0); (6) the exit: `game-exit.log` with steps 3-9 and whether the process leaves, and
  `the uam thread is still compiling after 3 s`.
- Decision after run 40: conversions > 0 and the picture unchanged → the next hypothesis is the depth hand-off or the
  stencil plane of the copied depth (a capture of the alias image would decide); conversions 0 → the registration; frame
  rate unchanged → the copy is not the uncached reads (the split timers say what it is).

## 2026-10-28: run 40 read (K): the alias runs for one of two depths, the vertex copy is 6x cheaper, `+` leaves

Run 40 = `run40-build/theft4-dk-debug.nro` (SHA-256 `2609128a…2265`), stage 3, `17f3a18-dirty`. Logs and captures are in
`out/console-runs/40-logs/`: `game.log` 1,628,483 bytes (13,027 lines), `game-exit.log` (765 bytes, 9 steps),
`game-crash.log` (1,281 bytes), `captures/` with 16 PNGs. Nothing was modified in code for this section: analysis only.

### K1. What was examined

- All 16 PNGs of the folder were listed; opened and looked at: `dk-f003835`, `f003838`, `f004175`, `f004190`, `f004203`,
  `f004219`, `f004236`, `f004250` (8 of 16). Not opened: `f000284`, `f000882`, `f001484`, `f002084`, `f002683`, `f003085`,
  `f003465`, `f003659` (logo/loading part, before the 3D scene).
- A luminance measurement of every gameplay capture of runs 38, 39, 40 (mean luma, share of pixels under 8 and over 235, mean
  R/G/B) with a small PNG decoder written for it (sampled every 4th pixel of every 4th row; the numbers are in K4).
- `game.log`: all alias, timer, fps, fence, mix-up and teardown lines (extracted by pattern, not read line by line), the exit
  and crash logs, the CPU and audio lines of the play windows.

### K2. VERIFICATO from the log: the alias, registration by registration

- The title registers **two** packed depth aliases (log lines 2181-2182): alias `DA1A63A0` ← resolved depth `DA1A6320`
  (1280x720, guest format 23, at `0608F000`) and alias `DA1A6420` ← resolved depth `40079D50` (1280x720, guest format 23, at
  `1F1B9000`). The front end counts `packed depth alias registrations passed on 2`; `virtual resources 153` are still dropped.
- `0 conversions` until the 3D scene (line 9856), then, by the last report: **467 conversions, 5,521 fetches served through a
  registration, 0 failed, 0 "before the depth was resolved", 0 "not A8R8G8B8", 1 alias live** (only `DA1A6320`'s).
  `libdeko3dd errors 0 warnings 0`, no uam failure.
- **The second alias is never served.** The log line `depth/colour mix-up: fetch handle 40079DA0 base 1F1B9000 format 6 32x32
  swizzle 60A -> resolved object 40079D50 1280x720 depth (guest format 23, found by base, 2 alias registrations)` (line 9684) is
  the only one of the 16 possible lines: **every one of the remaining `other depth/colour mix-ups` (4,318 at the last report)
  comes from that one fetch handle**, because the log keeps a handle once. Its fetch is the alias signature (format 6,
  swizzle `60A`, the 32x32 placeholder size, the base of the second depth), its source `40079D50` is exactly what the title
  registered for `DA1A6420`, but **the handle the draws bind (`40079DA0`) is not the handle that was registered
  (`DA1A6420`)**. Why they differ is not measured (the hook reads the alias object at registration; the draws may use
  another object): the registration route cannot see it and the by-address route refuses it because of the size.
- Rates in play (the last windows of the report, which are not all the same length): +870 to +1,060 fetches served through the first alias per
  report, +300 to +1,500 mix-ups through the second. The two aliases are used about equally. Which of them the light volumes read is **not measured**.

### K3. VERIFICATO from the log: the recorder time and the frame rate

Interval means per drawn draw (µs), run 40 against run 39, same place of the game:

| window | frames/s | draws | total | textures | vertices | indices | index scan | constants | the rest |
|---|---|---|---|---|---|---|---|---|---|
| run 39, play (4 windows) | 0.8-1.0 | 25-30 k | 244-274 | 5-6 | 171-185 (vertices and indices together) | – | – | 3 | 56-80 |
| run 40, 11864 | 5.9 | 45,952 | 108.5 | 10.0 | 26.5 | 4.1 | 1.3 | 3.1 | 63.5 |
| run 40, 12004 | 1.3 | 40,242 | 113.2 | 5.1 | 28.0 | 4.3 | 1.5 | 3.3 | 70.9 |
| run 40, 12227 | 1.4 | 38,509 | 120.8 | 6.4 | 29.0 | 4.4 | 1.5 | 3.2 | 76.3 |
| run 40, 12358 | 1.6 | 39,847 | 118.4 | 4.9 | 30.0 | 4.6 | 1.6 | 3.1 | 74.3 |
| run 40, 12550 | 1.7 | 40,027 | 116.6 | 5.4 | 29.3 | 4.5 | 1.5 | 3.2 | 72.8 |

- Vertices and indices went from 171-185 to **~34 µs (vertices 28-30 + indices 4-5)**; the recorder from 244-274 to
  **113-121 µs per draw**; the play frame rate from 0.8-1.0 to **1.3-1.7 frames/s** (draws per frame 2,357-3,101 as before).
- "The rest" is now **63-76 µs, ~62 % of the recorder**, and is not decomposed (it holds the shader lookups, the vertex-input
  loops with their per-draw vectors, the state emission and the binds). The fence counter does not move in play
  (`waiting for slot fences 36505 ms` from line 11380 to the end): it is CPU, not a wait.
- **The recorder is not the whole frame.** 2,357 draws × 117 µs = 0.28 s per frame of ~0.59 s (1.7 frames/s): about half. The
  other half is the title and the front end on the guest thread (CPU lines: `id 16 guest` 58-64 %, `id 12` and `id 17` 37-44 % each,
  `id 2 host` 28-30 %): **not measured** which part (the front end's per-draw capture was last estimated at tens of µs per
  draw in stage 2, H2; no timer exists for it).
- The audio keeps its pattern: `output ran dry` 512-525 times per 10 s, 30-34 % of real time, at 1.3-1.7 frames/s; run 38 had 504-518 and
  30-32 % at ~1.0 (run 39's audio lines were not read for this comparison): it does not follow the frame rate in the 1.0 → 1.7 range.
  DA VERIFICARE what it follows.

### K4. The stall, from the log (VERIFICATO as numbers, no cause)

Same place: frames 3806 → 3824 (log lines 10174-10298), 521-564 draws per frame, 0.6 frames/s, `waiting for slot fences`
568 → 3,768 → 13,400 → 22,757 → 31,968 → 36,505 ms (run 38: 32,121; run 39: 31,556). It begins when the shader set goes
64 → 135 → 150 (code 463 KB → 2,546 KB, `scratch per warp 92672 of 98304`), the textures 52 → 312 → 377, and it ends by itself
at the end of the intro scene (frame ~4122 runs at 31 frames/s with 94 draws). In those windows "the rest" is 2,700-3,100 µs
per draw (the slot wait) while the other sections are unchanged, so **the 6x faster vertex copy did not change the stall: it is
GPU time** (VERIFICATO by the same wait in three runs with different CPU costs). Counters in the same windows: z-fail light
volumes 118 → 479, `vertex inputs without an element` 12 → 84. The cause is **not identified**; a correlation with
the first use of the big-scratch programs is possible (run 37 with 2 KB of scratch: ~5 s per work, run 38-40 with 96 KiB:
~1.5 s) and is not proof.

### K5. The captures, run 40 against 39 and 38 (VERIFICATO as pictures and as luma; causes are STIMA)

| run | gameplay frames measured | mean luma (0-255) | black (<8) | bright (>235) | mean B/R |
|---|---|---|---|---|---|
| 38 | 4139 4150 4160 4171 4181 4191 4201 4211 | 3.6 / 88.5 / 13.6 / 3.5 / 57.8 / 70.2 / 4.9 / 69.2 | 39-96 % | 0-9.5 % | 1.15-1.31 (frames above luma 50) |
| 39 | 4130 4140 4150 4159 | 7.2 / 103.2 / 94.6 / 10.2 | 26-80 % | 0-5.1 % | 1.25-1.30 (4140, 4150) |
| 40 | 4175 4190 4203 4219 4236 4250 | 42.9 / 102.0 / 63.5 / 142.2 / 1.9 / 3.0 | 7-95 % | 0-6.7 % | 1.22-1.31 (frames above luma 40) |

- **Terrain:** the asphalt in front of the car is black in `f004190` and `f004203` (as in runs 38 and 39); the concrete behind
  the car is lit pale blue and shows texture and cracks; in `f004219` (the car has moved) the ground is lit over the whole
  foreground with blue road markings and a pink pool of tail-light.
- **Cars:** the taxi is dark blue with a correct red tail-light halo; its underside and rear bumper are broken into wedges
  (as before).
- **Light volumes:** the white wedge/cone at the upper right of the street is still there in `f004175`, `f004190`, `f004203` and
  a pale cone in `f004219`; in the dark frames (`f004236`, `f004250`) soft round lights (orange sparks, a red diamond) look
  like real night coronas, not like errors.
- **Exposure:** the mean luma still swings between ~2 and ~142 between frames ten frames apart. Run 40 shows intermediate
  frames (`f004175` luma 42.9, `f004203` 63.5) that runs 38 and 39 mostly did not, and its brightest frame (`f004219`) has the
  fewest black pixels (6.7 %), but it is a different position of the car, so **no improvement of the exposure instability can
  be claimed** (six frames against four and eight, different content).
- **Colour:** the blue/cyan cast is there in every bright frame of the three runs (mean B/R 1.15-1.31 where luma is above 40), with no systematic change between runs.
- **HUD and text:** radar, money and the mission name are right; subtitle letters are still missing and **different ones per
  frame** (`D ive to Rom n pla .` in `f004236`; `Drive to Rom n's pl ce.` in `f004250`; `ROMA 'S T XI` in `f004203`), always the
  same letters at the same time inside one frame (both `a` missing in `f004250`): glyphs, not draws. The title has a
  vector-font texture path (`vector_font_id` in `SetTexture`, `RegisterVectorFontTexture`, the font identity in the Vulkan
  path) that the front end does not use ("Read from the device at each draw"): a candidate, not tested.
- The intro scene (`f003835`, run 39's `f003786`): the same shattered geometry; a face is readable at the top centre in run 40
  that was a blob in run 39: different time of the cutscene, not comparable.

### K6. VERIFICATO: the exit

`game-exit.log`: steps 1-9 in 6,782 ms (`+` held 1 s; file I/O gate closed at +4,573 ms; `joining 52 threads for at most 2 s`; `8 of 28
threads ended within 2 s; next exit(0)`; `exit`; `_exit`). `game.log`: `teardown: recorder`, `teardown: uam thread`, **`the uam thread is still
compiling after 3 s (4 jobs): not waited for, the exit goes on`**, `teardown: device (0 blocks alive)`, `teardown done`.
`game-crash.log`: `termination: exit(0)` then `_exit(0)`, uptime 206,226 / 206,326 ms, 0 faults. Not measured: what the screen showed
at the end and whether Horizon showed an error dialog (the log cannot say; the user did not report one).

### K7. The three changes of run 40

| # | change | status | evidence |
|---|---|---|---|
| 1 | alias driven by `packed_depth_source` | **PARZIALMENTE VERIFICATA** | executed (2 registrations, 467 conversions, 5,521 fetches served, 0 failed, 0 libdeko3dd errors); **one of the two aliases is never served** (4,318 fetches of `40079DA0`); the expected visual effect is not shown (K5); nobody has looked at the alias image, so the correctness of the converted bytes is unproven |
| 2 | vertex and index copy through cached scratch | **VERIFICATA** for its purpose (time) | vertices 171-185 → 28-30 µs together with indices 4-5, recorder 244-274 → 113-121 µs, play 0.9-1.0 → 1.3-1.7 frames/s; the shapes in the captures are unchanged. The bytes are not compared offline: no data corruption is visible. It did not touch the stall |
| 3 | 3 s limit on the uam join at the exit | **VERIFICATA** | exit through all 9 steps, `_exit` at +6.8 s, 0 faults; the thread was still compiling and was left (the residual risk, a Horizon error dialog at the end, is not measured) |

### K8. Most probable cause of the glitches, and the next experiment

Facts: the lighting is wrong where the title's light volumes are; the exposure swings 70x between ten-frame samples; the cast
is constant; a depth alias is now correctly built for one of the two depths and the picture is not visibly different; the
other alias is read as a float depth in all channels 4,318 times in the run.
- **Most probable cause of the light errors: still the depth the lights read, because one of the two aliases is not
  converted (K2). Confidence: medium-low** (the measured fact is the unserved fetch, 4,318 times, that has the exact signature of
  the defect run 38 found; that the lights read *that* one is not measured; the first alias being right is not measured).
- Alternatives that the data does not exclude: the alias bytes themselves (stencil plane of the copied depth, the 20e4
  encoding, the swizzle), the dropped depth hand-off (1 per frame), the HDR chain (resolves with exponent 1 into format 29, the
  only exponent seen) and the exposure feedback it drives, the vector font.
- **One main change for run 41: serve the second alias.** In `PrepareTexture`, when the lookup by address finds a depth image
  whose destination object is **the registered source of an alias** (`alias_sources_` values: `40079D50` is one) and the fetch is
  A8R8G8B8 (format 6 or 50), go through `PackedDepthAlias(object)` **without the size test**: the size of a registered alias's
  fetch is a placeholder (K2), and the test keeps its meaning for every other depth image. Risk: low (it only widens one
  condition to objects the title itself named; any other texture at that address stays as it is), and the report line says what
  happened (a new counter "served by address from a registered source").
  Result that **confirms**: `other depth/colour mix-ups` stops growing (~0), the new counter grows ~1,000 per 10 s in play, no
  `depth/colour mix-up: fetch handle 40079DA0` line, libdeko3dd 0, and the captures show the ground/cones/exposure different from
  K5 (judged by looking and by the luma table). Result that **refutes** (as the sufficient cause): mix-ups ~0 and the pictures
  as in K5 → the depth alias is not what makes the lights wrong, and the next step is the alias image itself.
- **Diagnostic companion, log-only, not a second behaviour change:** one PNG of the alias image (the depth bytes as colour) next
  to the screen capture, so that the question "are the converted bytes right" is answered by the same run rather than by another.
  It is the smallest test that separates "alias served but wrong" from "alias not the cause".
- **The stall is a separate problem and is not touched.** Its first discriminating test is a cvar that skips the draws of the 27
  vertex programs over 2 KB of scratch (H9) for one run: if the stall (frames 3806-3824) disappears, the programs are the cause;
  if not, per-pass GPU timestamps. Not recommended in the same run as the alias change (two changes with independent effects
  would still be separable, but the user asked for one).
- Not next: the remaining 62 % of the recorder ("the rest": needs a split first), the front end (needs a timer first), the vector
  font (a rendering path with no evidence about the lights), FSR1, ZCULL, shadows, reflections.

### K9. Limits of this reading

- The log keeps one mix-up line per fetch handle: the 4,318 are attributed to one handle only because 16 slots were free and one was
  used; another handle would have been logged.
- Which alias the light volumes sample, and whether the first alias's bytes are right, are not measured; no capture of an alias image exists.
- Eight of the 16 PNGs were not opened; luma comes from a sampled decode; the content of the frames differs between runs (the game is
  at 1 frame/s, the taxi moves slowly), so run-to-run differences in a single frame are not evidence.
- The audio pattern, the part of the frame outside the recorder, the Horizon exit dialog, and the cause of the stall are not measured.

## 2026-10-28: run 41, reference documentation first, then one correction (L)

Run 40 left five open candidates (K8) and a request not to continue with micro-fixes read off the captures. This section
records what the reference ports do, the comparison with our recorder, the discrepancy chosen and why, the change, the
tests, the builds and what the next console run has to show. Earlier sections are not rewritten: K8's recommendation
(serve the second alias) is **not** what this run does, and the reasons are in L4.

### L1. What was read

- **Thorhax deko3d reference** (`https://github.com/Thorhax/Thorhax-deko3d-reference`, shallow clone, commit
  `b7e298f9d0945224c108a298ad590ee8143a28bf`, 2026-10-23; documentation only, CC BY 4.0): README, `docs/01-deko3d-case-study.md` in
  full, `docs/03-game-side-cpu-lessons.md` in full. `02-console-test-rig.md`, `04-shipping-without-game-data.md` and
  `references.md` were listed, not read (rig and shipping, not the renderer).
- **sa2-nx** at `adf3a4eee524dcca5cd7c1b42454ddb8a09124a1` (local clone `C:/dev/refs/sa2-nx`, 1.0.3.1): `docs/deko3d-renderer-guide.md` in
  full; `docs/renut-renderer.md` searched for depth, resolve, hand-over and alias; code read: Renut `render/textures.cpp`
  (`ResolvedAlias`, `DepthWordsSource`, `WordsViewSlot`, `GetDepthResolveTextureLocked`), `render/draw.cpp` (`ResolveDepth`,
  `HandOverResolveLocked`, `MaterialiseLocked`), `render/surfaces.cpp`, `shaders/host/depth_resolve.hlsli`, plume's deko3d
  back end (`plume_deko3d.cpp` queue/scratch, `plume_deko3d_commands.cpp` barriers, copies, Zcull).
- **nfsmw-nx** at `266f943` and **masseffect-nx** (local clones): read by the six read-only auditors below, not by me line by line.
- **Not accessible:** usf4-nx, banjo-nx (not public) and the usf4-nx installer: stated, nothing taken from them. The reNut renderer has no licence file
  (permission by its author for the sister ports): **no reference code was copied**; only behaviour was compared.
- **Our side:** `09-rules.md` in full (rules 7, 8, 13 apply: one build at a time, nothing is "corrected" before a console run, one variable per run),
  devlog sections H-K, `run-index.md`, run 40's logs and captures (section K), the code of `dk_recorder.cpp`, `dk_core.cpp`,
  `dk_graphics_system.cpp`, `dk_compiler.cpp`, `native_front_end.cpp`, and the Vulkan path of the same game
  (`graphics_system.cpp`, the hooks, the policy headers), which is the only reference that knows GTA IV's own command stream.
  `git status`: branch `sync-clean`, HEAD `17f3a18`, only this series' uncommitted work.
- **Method:** one workflow of six read-only auditors (targets/Maxwell, depth/alias/hand-over, shaders, state/draw, sync/stall, upload/CPU),
  each reconstructing the reference behaviour and ours and listing discrepancies with file/function and a strength; I checked the ones I
  rely on against the code myself (marked below).

### L2. What the references say, against ours

| Topic | Reference (commit, place) | Ours | Verdict |
|---|---|---|---|
| Depth target smaller than the colour target clips every draw on Maxwell | Thorhax case study s.5 (usf4-nx, `draw.cpp`); sa2 guide s.12 | `BindTargets` takes the smallest size; counter `target binds of unequal size` is **0** over runs 37-40; no draw binds the 640x360 depth with a 1280x720 colour | not our defect (VERIFICATO by the counter); the "do not bind depth without a depth/stencil test" rule is not implemented and not needed here |
| Depth read as bytes by 8_8_8_8 fetches ("depth words") | sa2 Renut: `GetDepthResolveTextureLocked` (R32F + RGBA8 words in one pass), `DepthWordsSource` (fetch format 8_8_8_8 and the same base page and size), `depth_resolve.hlsli` (word = depth24<<8 \| stencil, stencil in R, 20e4 for format 23) | `PackedDepthAlias` converts at the first fetch after a resolve; same word layout, same 20e4 and 24-bit rules (the test compares the text with LRA's shader) | same semantics (VERIFICATO). Renut finds the alias **by address and size**; ours by registration, and by address only with a size test |
| Depth hand-over (no copy of a whole depth resolve) | sa2 Renut `HandOverResolveLocked` (performance only) | a full copy-engine copy with Full barriers | not linked to the picture (auditor 2: same values reach the shaders) |
| Queue | plume: Graphics\|Compute, MediumPrio, command memory 1 MiB, flush threshold 1/8, scratch 64 KiB, ZCULL off | Graphics, scratch 96 KiB, ZCULL off | same in substance; scratch size cannot throttle (auditor 5 decoded `QueueWorkBuf`: total = per-warp x warps, rounded) |
| Eager kicks (a frame submitted in pieces of 100-200 draws) | sa2 `video.cpp` | one submission per frame | latency/overlap, not linked to the stall (the stall is GPU-bound) |
| Render thread | sa2 `banjo_renut_tuning.cpp`: priority 0x2C, pinned, guests moved off its core; batched wakes (16 draws), swap of the whole queue | the recorder thread runs at libnx defaults (0x3B, unpinned) and the queue is locked and notified per packet; the profiler does not list it | candidate for the frame rate (auditor 6, strength medium) |
| Debug vs release library | Thorhax s.3, sa2 table: ~9 us per draw on the release library | **runs 36-40 used `libdeko3dd` (validating)**; the 117 us per draw were measured with it | open: no run on `theft4-dk.nro` yet |
| Stable linear textures (hash cost) | Thorhax s.6 | our texture check is nfsmw's backoff port; textures are 5-6 us of 117 | not the bottleneck here |
| Vertex streams cached across frames | sa2 (store cards), LRA (persistent buffers) | converted and re-copied per draw (now ~35 us of 117) | candidate for the frame rate after the above |
| **Resolve-attached clears** | LRA/Vulkan `RecordResolveClears` (`graphics_system.cpp:27487-27736`): bit 8 clears the colour target `flags & 7`, **bit 9 clears the device's depth/stencil whatever was resolved**; nfsmw `nfsmw_dk_destinos.cpp:1055-1057` the same | clears only the resolved source; bit 9 on a colour resolve is **counted and not done** | **discrepancy, chosen (L3)** |
| Depth clamp for z-fail light volumes | LRA `graphics_system.cpp:7136-7141` (`ShouldEnableNativeDepthClampForDraw`) | counted (5,816 volumes), `depthClampEnable` never set | discrepancy, next candidate |
| Depth hand-off (`kDepthSurfaceHandoff`) | LRA `RecordDepthSurfaceHandoff` (`graphics_system.cpp:26823-27128`): depth of the resolved scene into the forward depth, stencil 0x80 empty / 0xFF covered | dropped (467 in run 40) | discrepancy, medium risk (new pass), after the two above |
| Second alias (fetch `40079DA0`) | neither Renut nor LRA serves an alias whose header size is wrong (both require it) | unserved (4,318 fetches) | discrepancy, the K8 plan |
| Depth bias | LRA applies it always | off at `nx_dk_stage 3` (the code gate is `stage >= 4`) | config-only test: `nx_dk_stage = 4` (slope term is 16x too large: auditor 4) |
| Loop budget on the state-machine vertex shaders | LRA `AddSpirvLoopWatchdog` (16,384 iterations) | none; 54 programs need 3-92 KiB of scratch | candidate for the stall |

### L3. The discrepancy chosen: the depth/stencil clear that rides on the end-of-deferred resolve

**Evidence (all VERIFICATO from code and log):**
1. The title ends the deferred phase with a sequence of resolves (`gta4_native_hooks.cpp:6929-6944`): the depth (flags 20), the
   colour (16), the "alias" colour target with **flags 768 = `0x300`: clear colour and clear depth**, and two more colour outputs
   with clear colour (273, 274). `SubmitNativeResolve` takes the clear depth 0.0 and stencil 0 for them.
2. The Vulkan path executes bit 9 on the **device's bound depth/stencil surface**, independent of what the resolve read
   (`RecordResolveClears`, lines 27720-27734), and refuses both clears when the command carries a clear-format override.
3. Our `DkRecorder::Resolve` cleared only the resolved source (`dk_recorder.cpp`, run 40 lines 897-935) and counted bit 9 on a colour resolve
   in `gaps_.resolve_cross_clear`: **467 in run 40 = exactly one per deferred frame** (the same count as the hand-offs and the alias
   conversions). The title's other clears are 1.6 per frame in total (6,902 over 4,241 frames), so an explicit per-frame depth clear is **not evidenced**
   (not measured by type).
4. Two auditors found it independently (targets, depth).

**What it would explain (STIMA):** the deferred depth and stencil enter every frame with the previous frame's content: the depth test of the
G-buffer pass runs against last frame's geometry (shattered silhouettes where anything moves, parts of the ground rejected), and the
stencil the light volumes count in never returns to zero (the wedge- and cone-shaped lit areas, black where the count is wrong, the same
shapes in every frame). The static camera of a game at 1 fps hides part of it. It does not explain the blue cast or the glyphs.

**Why this and not the others:**
- *Second alias* (K8): strongest log evidence of a wrong read (4,318), but **no source says the lights read that alias**, and the first alias (served, 467
  conversions) changed nothing visible in run 40; a second alias fix would be the same experiment again.
- *Depth clamp*: strong in the Vulkan path but it needs the title's `LightingContext` (`IsLocalStencilSetup`) which the front end does not carry; without the gate it is an
  approximation. It is the **next** change if the picture is still wrong.
- *Depth hand-off*: a new pass and shader, medium risk; the evidence for its effect is theory (forward-phase draws unoccluded).
- *Performance* (release library, thread priority, queue batching): measurable and low risk, but a correct picture at a few frames per second is
  still not a game; the debug/release comparison needs **no code**, so it travels with this run as a second NRO (L6), not as a second change.
- *Stall*: no discrepancy with a reference explains it; the auditors' only supported test is skipping the big-scratch programs or per-pass GPU
  timestamps, both separate runs.

### L4. The change (uncommitted, tree `17f3a18-dirty`)

- `native_resolve_clear.h` (new, no heavy headers): `PlanResolveClears(flags, parameters_valid)` returns what the clears of a resolve apply to:
  colour clear of the resolved colour target (bit 8), depth clear of the resolved depth (bit 9 on a depth resolve), **depth/stencil clear of the
  device's surface (bit 9 on a colour resolve, refused with `parameters_valid`)**, and the unrepresentable case (bit 8 on a depth resolve, not done
  here either).
- `native_recorder.h`: `RecorderResolve::depth_stencil`. `native_front_end.cpp`, case `kResolve`: when bit 9 is set, the device's depth/stencil surface is captured
  (as `kClear` does) at the command's position in the stream.
- `dk_recorder.cpp`, `Resolve()`: for a colour resolve with bit 9 (`plan.clear_device_depth`): `Surface(depth_stencil, true)`, bind it as the depth target, scissor =
  the resolve's rectangle clamped to the depth surface (the whole surface without one, as the Vulkan path), `dkCmdBufClearDepthStencil` with the
  command's depth (a double in the command) and stencil, `ForgetState()`. A depth surface that cannot be made is counted. The old counter now counts only what stays undone.
- Diagnostics strictly for this change: the first 6 clears are logged (resolve, flags, surface, size, values, rectangle) and the report says `depth/stencil clears of colour
  resolves (flags bit 9; totals): N done, M without a depth surface, K refused`.
- Also in this tree (verification requested for run 40's copy, **no behaviour change**): `dk_vertex_convert.h` holds the vertex fix-ups that run 40 moved into a cached scratch
  (`ApplyVertexFixes`, `VertexElementBytes`); `Draw()` calls it.

### L5. Offline tests (VERIFICATO)

`python tests/deko3d/test_resolve_clear.py` (host clang of `THEFT4_VS_ROOT`): **OK**.
- Resolve clears: 12 of 12 cases: flags 20 and 16 clear nothing; **768 clears the colour target and the device's depth**; 273 and 274 only the colour; a depth clear on a
  colour resolve alone (`0x200`); a depth clear on a depth resolve stays on the source; bit 8 on a depth resolve is the unrepresentable case; `parameters_valid` refuses the
  device-depth clear; the exponent bits of the flags (`0x04000000`) do not matter.
- Vertex fix-ups: **9,000 random streams** (three seeds; strides 4-64 B, 0-4 fixes of the three kinds on 14 element types, including offsets past the stride) compared byte for byte
  between the in-place algorithm of run 39 and the cached-scratch path of run 40 (`ApplyVertexFixes` + one copy): 0 differences. The reference in the probe is checked against git:
  its fix loop and size table are **the text of commit `17f3a18`'s `Draw()`**, verbatim (26 lines). This verifies the **fix-ups**; the word swap (`CopyGuestWords`, NEON) is not compiled
  on the PC and is not part of this comparison; the picture of run 40 showed no corruption.
- Not testable offline: that the clear happens where the title expects it on the console, and its effect on the picture.

### L6. Builds (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run41-build dk-debug dk` (-j3, one build at a time, no other `ninja`/`cc1plus` before).
Both exit 0, no compiler warning (the known `rex_version` dirty-tree CMake warning only); `COMMIT` `17f3a18-dirty`; the new log strings are in the debug ELF.
| artefact | bytes | SHA-256 |
|---|---|---|
| `out/console-runs/run41-build/theft4-dk-debug.nro` (debug library, the run's NRO) | 46,876,869 | `3272f7631ef915202ac320a5ed87984ac1678f3c1f95855f062840a1e33ecdc0` |
| `out/console-runs/run41-build/theft4-dk.nro` (release library, same source, timing only) | 46,844,101 | `26a5b4377a67b899b43bbe79662b3d274a34f7759d28bb872232d4ea8f7f9907` |
`switch-game-dk-debug.elf` 314,359,088 bytes and `switch-game-dk.elf` 314,210,064 bytes for `addr2line`; `game-stage3.toml` (unchanged: `nx_gpu = "deko3d"`, `nx_dk_stage = 3`, `nx_dk_capture_s = 10`, `nx_dk_gamma = 1.0`) copied in. The earlier NROs (`run38-build`, `run39-build`, `run40-build`) are untouched.
`theft4.nro` (NVK) not rebuilt. Nothing run on the console.

### L7. Expected behaviour on the console, and what confirms or refutes it

- **Run:** `out/console-runs/run41-build/theft4-dk-debug.nro` with `game-stage3.toml` as `game.toml` (the debug library, as runs 36-40, so the picture and the counters compare), same `deko3d/` files,
  no cache to delete, empty `captures/` first. Optional second session, **same toml**: `theft4-dk.nro` (release library, same source) to read only the `recorder time per drawn draw` lines and the frame rate.
- **Confirms (STIMA until seen):** the log line `deko3d: resolve ... clears the device's depth/stencil` appears, the report says `depth/stencil clears of colour resolves ... N done` with N about one per
  deferred frame (467 in run 40's length), 0 `without a depth surface`, libdeko3dd errors 0; the captures from frame ~3800 on show the silhouettes of the taxi and the ground without the wedge-shaped losses, the white wedge/cone
  weaker or gone, the luma swing between ten-frame samples (K5: ~2 to ~142) smaller. Compare with the K5 table, not with a single frame.
- **Refutes (as the cause of those defects):** N about 467 and the captures as in K5 (the same black ground, wedge and swing): the stale depth/stencil was not what they come from, and the next candidates are the depth
  clamp of the z-fail volumes, the hand-off, the second alias, in that order.
- **Fails:** `without a depth surface` > 0, a libdeko3dd error or warning, a different frame rate by more than the noise (the change adds one clear per frame), a missing picture: then the depth surface bound at that resolve is not the one the next frame uses.

### L8. Limits

- No reference documents GTA IV's deferred phase: the semantics come from our own Vulkan path, which the same authors tuned on a PC; its choices (clear the bound depth on bit 9) are evidence of what the title does, not a proof that the console needs it.
- It is not measured which depth surface is bound at that resolve (the log does not print it); the new log line will.
- Whether the title also clears the deferred depth with an explicit `Clear` each frame is not measured; if it does, the effect of this change shrinks to the stencil.
- The CPU findings (render thread priority and pinning, queue batching, release library, persistent vertex buffers) are **not applied**: only the library comparison is prepared (an NRO, no code).
- The audit's census statements about the shader package (54 programs over 2 KiB of scratch, 18 variants uam fails on) come from the auditors' parsing of the package, not from my own check.

## 2026-10-28: run 41 read (M): the clear runs and changes nothing visible; red and blue are exchanged in the lit scene

Declaration (run-protocol.md): **single sample per configuration: no difference between runs is proven**; the numbers below are
clues to repeat, except where the log counts something exactly. Both sessions had the protocol's three checks in the log: NRO
`17f3a18-dirty` (the built one), clocks read = wanted (1020 / 460.8 / 1331.2 MHz, apm `0x92220008`, Horizon OC running with the default
profile), `runtime-cache` not empty (3 files, 11.9 MiB; unused by the deko3d build, K7). Logs: `out/console-runs/41-logs/debug/` (debug
library, uptime 215 s, frames to 4204, 16 PNG) and `normal/` (release library, uptime 675 s, frames to 4719, 54 PNG). Both exits: 9 steps,
`_exit` at +6.9 s, 0 faults.

### M1. Was the run-41 change executed? (VERIFICATO from the logs)

| Counter (last report of each session) | debug | normal (release) |
|---|---|---|
| depth/stencil clears of colour resolves: done / without a depth surface / refused | **485 / 0 / 0** | **991 / 0 / 0** |
| packed depth alias conversions (= deferred frames) | 485 | 991 |
| `kDepthSurfaceHandoff` dropped | 485 | 991 |
| `resolve clears of the other aspect` (what stays undone) | 0 | 0 |
| libdeko3d errors / warnings | 0 / 0 | 0 / 0 |
| draws, drawn, skipped by reason 13 (uam variant still compiling) / reason 10 | 685,054 / 684,761 / 289 / 4 | 2,567,476 / 2,564,795 / 2,677 / 4 |
| shaders: from the PC's DKSH / compiled by the console / failed / still compiling | 255 / 0 / 0 / 4 | 280 / 0 / 0 / 4 |

- The clears run once per deferred frame (clears = conversions = hand-offs, to the unit). The log names the surface: resolve `402268E0`
  with **flags `0x350`** (not the `0x300` the hook comment suggests: bits 4 and 6 as well) clears the device's depth/stencil **`402266F0`,
  1280x720**, which is the deferred scene depth ("depth target 402266F0" in the same log), to depth 0 stencil 0, rectangle 0,0 1280x720.
  So the question of L8 (which surface) is answered for the clear: it is the G-buffer depth, not another one.
- **The change is executed and changes nothing visible** (M3). The "refutes" case of L7 holds: the stale depth/stencil of the deferred
  phase was not what makes the picture wrong.
- Four vertex shader variants (`5A24DA93B3BA4BF1`, `1104682CE398E60A`, `2990720A168E7B19`, `006C0FA37B4FD4A0`, variant 0: among the 18
  that uam fails on the PC, E) stay "compiling" for the whole run in both sessions (450 s in the release one): their draws are skipped, 0.1 % of
  all draws, ~2.7 to 4.8 per frame in play. DA VERIFICARE: that these draws are the subtitle glyph quads that are missing in every capture
  (the order of magnitude, about three missing spans per frame, fits; nothing identifies the draws).

### M2. Debug library against release (VERIFICATO as numbers, one sample each; the phases differ, so read the ranges)

Comparable windows: the play phase after the cut-scene, 2,450-3,100 draws per frame, the taxi at the start of the mission.

| | debug (hb 150-190, frames 4130-4204) | release (hb 220-280, frames 4198-4290) |
|---|---|---|
| frames/s | 1.3-1.6 | 1.1-1.9 |
| recorder per drawn draw (interval) | 110-120 µs | 98-124 µs |
| textures / vertices / indices / index scan / constants | 5 / 29-30 / 4.6 / 1.5 / 3.1 µs | 5 / 31-35 / 5 / 1.6 / 3.0 µs |
| "the rest" | 66-76 µs | 50-68 µs |
| recorder thread (host id 2) CPU | 29-33 % of a core | 22-25 % of a core |
| fence waits after the stall | 0 | 0 |

- **What the release library buys:** the recorder's CPU per drawn draw goes from about 77 µs (debug: 30 % of a core at ~3,900 draws/s)
  to about 42-53 µs (release: 22-25 % at 4,350-6,000 draws/s), i.e. **a third to 45 % less recorder CPU**, in line with the
  references (the debug library validates every call). **It buys nothing visible in the frame rate**: 1.3-1.6 against 1.1-1.9 fps, the
  same range. The reason is in the next point. It is not a fix for the glitches and is not presented as one.
- **What is the dominant cost, as far as it can be measured (VERIFICATO from the heartbeat `cpu` lines, steady play, release):** the
  recorder thread is only **24-26 % busy**; the guest threads together use 170-180 % of a core (id 16: 68 %, id 17: 45 %, id 12: 38 %);
  `waiting for slot fences` does not grow after the stall (109,245 ms from frame 3808 to 3874, then 0). So the frame (0.7 s) is **not
  recorder CPU-bound and not GPU-bound at the slot fences**: the guest side (the title's recompiled code, with the front end that runs
  in its threads, on a 1020 MHz CPU) is the biggest measured block, and what the render thread waits on while it is not running (it is at 68 %,
  not 100 %) is **not measured**. The recorder's own 100 µs "per draw" is about 45 µs of CPU (textures + vertices + indices + scan + constants
  = 43 µs measured) and about 55 µs that are not CPU: waiting inside the recorder. DA VERIFICARE: what that wait is (a packet queue that is
  empty because the producer is slow is the first candidate; the timer starts at the packet, so it should not be included, and this needs reading
  before anything is changed).
- Not compared: audio, memory (both fine: 1.2-1.4 GiB headroom).

### M3. The captures (VERIFICATO by opening them; no numbers from a decode that I did not look at)

Statistics of every capture (mean luma, share of near-black pixels, B/R ratio) are in `scratchpad/pngstats_out.txt`; I opened 12 of the 70
PNG (debug 4122, 4138, 4151, 4167, 4184, 4201; normal 3654, 3865, 4200, 4389, 4532, 4711, 4727).

- **Same phase, same place, the pictures of runs 38-41 are the same picture**: bright-state luma 88-103 in runs 38/39/41 (142 once in run 40), dark
  state 3.5-14, the same black quads on the ground, the same white wedge at the right, the same blue/cyan cast (B/R 1.15-1.43), the missing
  subtitle glyphs. The clear did not move any of these.
- **Three discrete states in a static scene** (normal session, the taxi stands still from frame 4182 to 4727 with the mission text on screen):
  bright (luma 94-100, 85 % of the 10-frame samples; 69-77 from frame 4610), dark (10-15, the same picture about seven times darker, 7 % of the
  samples), and a "lights only" state (22-24, B/R 0.77-1.0: the albedo black, tail lights and cones bright; frames 4273, 4664, 4711). Not a drift:
  the bright level repeats to ±3 % over 230 frames and the dark ones are isolated samples. Run 38's captures (4139-4211) flip between the same
  states. DA VERIFICARE: what flips (candidates in M5).
- **Frame 3865 (cut-scene): two faceted spheres lit yellow with a dark cut-out inside**, in a dark scene: the light-volume mesh itself is being shaded.
- **Red and blue are exchanged in the lit 3D scene (VERIFICATO on the images, cause M4).** I swapped R and B of `debug/dk-f004138.png`
  (`scratchpad/swap4138.png`) and of `dk-f004184.png` (+ gain, `swapgain4184.png`): with the swap the warehouse walls are red brick, the parking
  line on the asphalt is yellow, the road is warm grey, a fire is orange; without it everything is blue/cyan. **The 2D HUD (yellow route on the
  radar) and the light accumulation (red tail lights, red glow on the road) are right without the swap**, so the exchange is in the 3D albedo path
  only. It is in every capture of runs 38-41 (B/R 1.2-1.4 against 0.5-0.64 in the 2D menu/intro captures).

### M4. The cause of the blue cast, and the documented fix (this is the run-42 intervention)

- **VERIFICATO (code):** our `ViewOf`/`ViewSwizzle` compose the fetch constant's swizzle into the view of every texture, resolved ones included
  (`dk_recorder.cpp` resolved branch `ViewOf(*resolved, swizzle, kSwizzleRGBA)`). The title's A8R8G8B8 fetches carry `0x60A` (Z, Y, X, W): the log shows
  it on every format-6 texture it prints (`186E0000`, `05AC3000`, the alias placeholder `1F1B9000`, the mix-up line `40079DA0`), and `0x688` (identity)
  on every DXT. For a texture read from guest memory the exchange is right (its bytes, after the 4-byte swap, are B, G, R, A). For a **resolved**
  render target it is not: the render target's lanes are R, G, B, A in both our path and LRA's (`RGBA8` in `SurfaceFormat` and in
  `ConvertSurfaceFormat`), the resolve is a copy (`dkCmdBufCopyImage`; LRA's conversion pass has no channel permutation, `resolve_convert_ps.glsl`),
  and the view then applies the `0x60A` permutation to lanes that were already in order: red and blue are exchanged. The G-buffer albedo
  (3 RGBA8 resolves per deferred frame: `402268E0`, `40218F40`, `402267E0`) is the only 8-bit colour the lit picture is built from; the light accumulation
  is RGBA16F (swizzle identity) and is why the tail lights are right.
- **The reference says the same, twice.** (1) `LibertyRecompAndroid/docs/android/PORTING.md` (`lra-7c97c58` family, "Driver-specific problems found and fixed"):
  "**Wrong colors (blue instead of yellow).** The renderer is tuned for identity image-view swizzles, and on MoltenVK it uses them. Android now does the
  same." and LRA's own code, `graphics_system.cpp:18646-18655`: on `portabilitySubset || !imageViewFormatSwizzle || ANDROID` the view components are
  IDENTITY with the comment "applying the guest swizzle instead swaps red and blue in GTA IV's ARGB light textures". (2) Our own record of the
  kabba port, `docs/switch-port/15-kabba-port.md:65`: the cvar `gta4_native_identity_texture_views` "corregge rosso/blu scambiati su NVK (taxi e pelle blu)": the
  same build on the same driver family had the same symptom and this fix. Only LRA's desktop branch applies the guest swizzle.
- **Strength of that evidence (adversarial cross-check, verdict PARTLY):** the diagnosis holds (the render target, the copy and the vertex colours add no
  exchange; the shaders do not compensate: `xsh_to_spirv.cpp:65` sets `image_view_format_swizzle = true`). The kabba line (15-kabba-port.md:65) is a
  one-line table entry for a cvar that is not in our tree and is weak (taxis and skin are DXT, swizzle 0x688). The PORTING.md quote was checked by me in the
  local clone `C:\dev\refs\LibertyRecompAndroid\docs\android\PORTING.md:120-121` (the cross-check looked only inside this repo). The "hardware copy
  swap" model (Xenia's `copy_dest_swap`, absent from our resolve command) is a **HYPOTHESIS**; the rule is justified by LRA parity and by the picture.
- **The change (run 42), the narrow form the cross-check recommended:** the view of a resolved COLOUR image whose fetch swizzle is the memory-order exchange `0x60A`
  is made with the identity (`ResolvedColourGuestSwizzle`, `dk_view_swizzle.h`); any other swizzle on a resolved colour (16F, R32F, constant-one alpha)
  is applied as before (LRA's identity branch drops all of them: equal for the fetches run 41 showed, safer for the unlogged ones). Guest-memory textures
  (including the A8R8G8B8 ones: the radar and HUD are right) keep the guest swizzle, depth images and the packed depth alias are untouched (the alias
  applies the swizzle by design, as in LRA). `ViewSwizzle`'s composition moved to the header unchanged. One new log line per distinct (fetch swizzle,
  host format) of the resolved colour fetches (16 at most): it says which swizzles the title sends for them. **Second place where we diverge from LRA,
  not changed in this run:** LRA applies identity to guest-memory textures too, and the log has real format-6 `0x60A` textures (`186E0000`, `18670000`,
  `05AC3000`, `06003000`); if light cookies or LUTs among them still look exchanged after run 42, that is the next one-line change.
- **Why this one and not the others (the table the task asked for):**

| Problem | Evidence in run 41 | Reference | Discrepancy in our code | Correction | What proves it |
|---|---|---|---|---|---|
| depth/stencil clear (run 41) | executed 991 of 991 deferred frames, surface `402266F0`, 0 errors | LRA `RecordResolveClears` | none left | done | refuted as the cause of the picture (M1, M3) |
| **red/blue exchange of the lit scene** | every capture, 38-41; swap test (M3) | LRA PORTING.md, `graphics_system.cpp:18646-18655`, kabba doc 15:65 | guest swizzle on resolved images | **identity view for resolved colour (done)** | the same capture with brick red / yellow lines; `resolved colour ... swizzle` log lines |
| z-fail volumes without depth clamp | 108 per deferred frame, "depth clamp 0"; spheres visible in the cut-scene; black polygons | LRA `depthClampEnable` for two-sided z-fail volumes (`graphics_system.cpp:7121-7141`) | dk counts them (`dk_recorder.cpp:1954-1974`) and never sets `rs.depthClampEnable` | one line + the policy gate | next candidate, ranked first by the investigator for the black regions and the only one that fits the cut-scene spheres |
| second packed alias never served | 96 mix-ups per deferred frame against 98 served by the first and 108 z-fail volumes: every light draw fetches both | K8 plan | the registered handle is `DA1A6420`, the fetch is `40079DA0` (K) | serve by address when the depth is a registered alias source | `other depth/colour mix-ups` ~0; in the cut-scene the ratio is 17 fetches for 148 volumes, so it is not the cause of the spheres |
| depth hand-off dropped | 1 per deferred frame; sky black in every capture | LRA hook + `scene_depth_handoff_ps.glsl` | command dropped (`native_front_end.cpp:265`) | new pass | first log the stencil state of the first forward draws |
| fourth G-buffer texture | `06090000` 1280x720 format 6 built from guest memory next to the four resolves (log 9622) | – | one destination never resolved | log the five resolves of a deferred frame | uniform plane, cannot make polygons; may feed the brightness states |
| HDR exponent / brightness states | 2,945 resolves with exponent 1 into format 29, not applied; three discrete states | LRA `resolve_convert_ps.glsl` (x 2^N, NaN scrub) | copy only | log the render target's exponent first (`net = 2^(N+E_rt)`; the reference pairs -1 and +1) | a constant factor cannot give the 230-frame plateau + isolated 7x dips: the NaN/inf scrub is the better candidate |
| GPU stall (frames 3808-3874: 109 s of fence waits; debug 3751-3770: 36 s) | no fence waits afterwards | **LRA skips the draws of PS `2673E2AF` by default** ("a deferred light-volume shader that hangs the GPU", `docs/android/ADVANCED.md`; VS `0872615B`, 8,400 vertices, 92,672 B of scratch) | dk has no skip list; the cut-scene is at the docks, near water | a cvar listing pixel shader hashes to skip | the stall disappears; second session, same NRO |
| debug library / CPU | M2 | Thorhax 01, 03 | debug library in runs 36-40 | release NRO built | recorder CPU -30..45 %, fps unchanged: not the bottleneck |

  **Chosen: the first row with a documented fix that explains a severe, certain defect and is safe: the exchange of red and blue.** It is the only
  discrepancy with a reference that documents the same symptom ("blue instead of yellow") and the same remedy, it affects every pixel of the 3D scene, it
  cannot affect the 2D path or the lights, and the proof is a picture. It does not explain the black polygons, the brightness states or the missing glyphs
  and it is **not presented as the fix of those**. The depth clamp is the next candidate; the PS-skip cvar for the stall is documented above as a separate,
  one-variable session.

### M5. Candidates for the brightness states (not touched, DA VERIFICARE)

The three states (M3) are not explained by a constant factor. The investigators' ranking: (1) NaN/inf reaching the luminance chain: dk copies the three HDR
resolves raw while LRA scrubs NaN to 0 and clamps ±65504 (`resolve_convert_ps.glsl:138-142`); (2) the exposure feedback running with the game's frame time at 0.7 s per
frame; (3) a lookup that sometimes misses a resolved texture (`06090000`). None of the three is measured; the cheapest first step is to log the render
target's colour-info exponent for the source of every exponent resolve.

### M6. Offline test and builds (VERIFICATO)

- `python tests/deko3d/test_view_swizzle.py` (host clang of Visual Studio, `THEFT4_VS_ROOT`): **OK**. `ComposeViewSwizzle` equals the `ViewSwizzle` of commit
  `17f3a18` for 16,384 cases (4,096 fetch swizzles x 4 host tables); a guest-memory A8R8G8B8 (`0x60A`) still shows R, G, B, A from stored B, G, R, A and a DXT (`0x688`)
  is as stored; a resolved colour with `0x60A` shows the same R, G, B, A the console gets from memory order + `0x60A` (2,000 random colours), any other
  swizzle on a resolved colour passes through unchanged (8,000 cases, 0 bad); the old behaviour is the exchange of run 41; single-channel formats still replicate. **Not tested
  offline:** the console effect (it is a picture), and the earlier tests were not re-run (nothing they cover changed; `ViewSwizzle` was only moved).
- Builds (`tools/switch-game/build_variants.sh out/console-runs/run42-build dk-debug dk`, `-j3`, one at a time, no ninja/cc1plus before, exit 0, 0 warnings),
  tree `17f3a18-dirty`; the new log string `viewed as stored` is in both ELFs:

| File (`out/console-runs/run42-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` (debug library: **the run**, comparable with runs 36-41) | 46,876,869 | `be15feb50f6fa16b6a3e857813ee899da44001c366d437f1c9c025efd900ef1f` |
| `theft4-dk.nro` (release library, same source: timing only) | 46,844,101 | `aef5875f5ce77b8392c8f4cdd8c9c93440b0cd41530e381c2262c3a957c8d0bf` |
| `game-stage3.toml` | – | copy of run 41's (`nx_gpu = "deko3d"`, `nx_dk_stage = 3`, `nx_dk_capture_s = 10`, `nx_dk_gamma = 1.0`) |

  `superseded-identity-all/` keeps a first build of the same day with the identity view for **every** resolved colour (not narrowed); it is not for a run. Run
  41's, 40's, 39's and 38's folders are untouched.

### M7. Run 42: what to do, what confirms, what refutes

- **Run:** `theft4-dk-debug.nro` with `game-stage3.toml` as `game.toml`, the same `deko3d/` files (`gta4_deko3d_runtime.bin` SHA `5139…1839`, `gta4_deko3d_glsl.bin`), no cache to delete,
  empty `captures/`, old logs renamed, ~4 minutes after the mission screen (the stall comes at ~100-200 s), then `+` held 1 s. Copy back `game.log`, `game.prev.log`, `game-crash.log`,
  `game-exit.log` and the whole `captures/` into `out/console-runs/42-logs/`. A second session is **not needed** (the release library gave nothing in the frame rate, M2).
- **Confirms (STIMA until seen):** the log shows `resolved colour ... fetched as format 6 with swizzle 60A: viewed as stored (host format 28)` (at least one line); in the
  captures after the cut-scene the brick walls are red, the parking lines yellow, the cast B/R falls from 1.2-1.4 to about 0.5-0.8 (the intro frames' range) in the same
  phase, the HUD and the red tail lights unchanged. The counters, the frame rate, the stall (frames ~3800-3870) and the black polygons are **expected to stay as in run 41** (this change does not touch them).
- **Refutes:** the same cast and no `resolved colour` line with swizzle 60A (then the exchange is somewhere else: a guest-memory format-6 texture, the second place of M4); or the HUD / lights
  turning blue (something else samples a resolved 8888).
- **Fails:** a libdeko3dd error or warning, a draw count or frame rate different beyond the noise, a missing picture.

### M8. Next, in the order the evidence supports (not done)

1. **Depth clamp for the two-sided z-fail light volumes** (`rs.depthClampEnable`, LRA `graphics_system.cpp:7121-7141`, policy `native_stencil_volume_policy.h`): 108 volumes per deferred frame,
   spheres visible in the cut-scene; ranked first by the investigator for the black regions. One line plus the gate (`IsLocalStencilSetup`).
2. **A cvar listing pixel-shader hashes to skip, default `2673E2AF`** (LRA's default; `docs/android/ADVANCED.md`) as a separate one-variable session for the stall (109 s of fence waits in the normal session).
3. The second packed alias by address (K8), the guest-memory format-6 `0x60A` identity, the render target exponent log + NaN scrub of the HDR resolves (M5), the stencil state of the first forward draws (hand-off).
4. The recorder's 55 µs of non-CPU wait per draw, and what the render thread waits on while at 68 % of a core (M2): needs a timer split, not a change.

### M9. Limits

- One sample per configuration; the captures are 10-second samples of a 1.4 fps game; I opened 12 of 70. The swap test is a visual judgement by me on two frames, not a measurement.
- Which draws are the 4 stuck vertex shaders, whether they are the glyph quads, and what the 06090000 fetch is, are not measured.
- The cross-check could not read the LRA clone (outside the repo); I did (PORTING.md:120-121, ADVANCED.md:119).
- The census of the shader package (VS `0872615B` = the 8,400-vertex light volume, 92,672 B of scratch) comes from LRA's code comment and the earlier audit, not from this run's logs (no log names it).

## 2026-10-28: run 42 read (N): the swizzle change ran and changed the colours; the lighting is still wrong; the depth clamp is run 43

Sample: one session, `out/console-runs/42-logs/` (`game.log` 1,924,800 B, `game-exit.log`, `game-crash.log`, 23 PNG, all 23 measured, 5 opened). **Campione unico: nessuna differenza è dimostrata**; what follows are
observations of one run against one run.

### N1. Was the run-42 change executed? (VERIFICATO from the log and the source)

The run-42 report said the picture was identical to the earlier runs. It is not: see N3. The execution itself:

| Question | Evidence | Verdict |
|---|---|---|
| Which NRO was started? | `game.log`: `build: commit 17f3a18-dirty, deko3d (debug library libdeko3dd) NRO; config read: game.toml`. The commit string is the same as run 41's, so **it does not identify the build**. What does: the line `deko3d: resolved colour ... viewed as stored` exists only in the run-42 source (0 matches in all of `41-logs/`, 4 in `42-logs/game.log`, lines 9544, 9616, 9632, 10118). | the run-42 build ran (not proved by a hash: the loader does not log one) |
| Which `game.toml`? | `Config: nx_dk_capture_s = 10`, `nx_dk_gamma = 1.0`, `nx_dk_stage = 3`, `nx_gpu = deko3d`, `cvars not at the default (1): nx_gpu=deko3d nx_dk_stage=3 nx_dk_capture_s=10` | = `game-stage3.toml` |
| Is the change in the build's source and in its path? | `dk_recorder.cpp` `PrepareTexture`, resolved-colour branch (`ResolvedColourGuestSwizzle`); the log line is written from inside that branch, so the branch was reached | yes |
| Did the line expected by M7 appear? | line 9616: `resolved colour 402183D0 fetched as format 6 with swizzle 60A: viewed as stored (host format 28)`; `402183D0` is the 1280x720 colour resolved at `0608E000` (line 9610), one of the G-buffer targets resolved next to the depth/stencil clear (`resolve 402268A0 flags 00000350 clears ... 402266E0`, line 9614) | yes |
| How many times, how many textures? | **not measured**: the log writes one line per distinct (swizzle, host format) pair (4 pairs: `688`/format 29, `60A`/format 6, `B68`/format 36, `B48`/format 31). **Only one image is named for the 60A pair** (the first). Counters added in run 43 (`resolved colour fetches`, `0x60A viewed as stored`) | gap, closed for the next run |
| A wording flaw | the message said `viewed as stored` for all four pairs, but only `60A` is changed (`ResolvedColourGuestSwizzle` passes the others through). Run 43 prints `swizzle applied as fetched` for them. No behaviour change. | fixed in the log text |
| Other paths that bypass it? | a resolved colour reached by a handle/base lookup goes through the same branch; guest-memory textures (`texture 186E0000 128x128 format 6 tiled pitch 128 swizzle 60A`, also `18670000`, `05AC3000`, `06003000`, `06090000`) and the packed depth alias do not, by design | by design, see N2 |
| Faults, errors | 0 faults, `libdeko3dd errors 0 warnings 0` in all 32 reports, exit 9 steps `_exit` at +6.7 s, clocks CPU 1020 / GPU 460.8 / memory 1331.2 MHz = wanted, Horizon OC running, runtime-cache not empty (irrelevant for deko3d), 349 s, frames to 4235 | clean |

### N2. The chain, step by step (VERIFICATO in code; the hardware behaviour is read from the captures, N3)

1. **What the title asks:** an `A8R8G8B8` fetch (Xenos format 6) with fetch swizzle `0x60A` = (Z, Y, X, W): `dk_view_swizzle.h`, `ComposeViewSwizzle`.
2. **What memory holds for a texture from guest memory:** the big-endian word 0xAARRGGBB is bytes A, R, G, B; after the 4-byte swap the image the recorder uploads has lanes
   (R,G,B,A) = (B, G, R, A). `0x60A` reads them as (R, G, B, A). Correct, and this is also the convention of LRA's own alias shader
   (`packed_depth_alias_ps.glsl:46-47`: raw bytes in memory order, then the guest swizzle; the comment says "0x60A puts stencil in BLUE"). The dk alias pass follows it (`dk_depth_alias.h`
   lines 7-8, 69-93) and its view carries the swizzle: equal to LRA's, **not** a second instance of the bug.
3. **What a resolve of a colour render target holds on the host:** the render target is RGBA8 (host format 28) whose lanes are what the shader wrote: R, G, B, A. The resolve is a copy
   (`resolved ... copied`), with no shader compensation (`xsh_to_spirv.cpp:65`). On the console the same colour in memory is in the order the game's resolve asked for (the copy-swap
   hypothesis, Xenia's `copy_dest_swap`: **not proven**, not in our tree); the title's `0x60A` fetch puts it back. On the host there is no swap, so `0x60A` **exchanges red and blue**.
4. **View construction:** `ViewOf(*resolved, swizzle, kSwizzleRGBA)` composes the guest swizzle with the host's: `0x60A` on lanes (R,G,B,A) gives (B,G,R,A). Run 42 passes `0x688` instead for `0x60A`.
5. **What the shader receives after the change:** (R, G, B, A) as stored. No later compensation exists.
6. **LRA:** `graphics_system.cpp:18646-18655`: identity views on portability devices and Android, "applying the guest swizzle instead swaps red and blue in GTA IV's ARGB light textures";
   `docs/android/PORTING.md:120-121`: "Wrong colors (blue instead of yellow)". Same defect, same fix. LRA applies it to **every** native texture on those hosts (our change is narrower: only resolved colours with `0x60A`).

What the offline test shows: the composition is mathematically what step 4-5 say (16,384 cases against commit `17f3a18`; 2,000 colours). It does **not** show the hardware behaviour or the binding; that
rests on the captures (N3). I do not claim that `0x60A` "means" a red/blue error: it means one for a host image that is already in R,G,B,A order, which is the case of the resolves.

### N3. The captures (VERIFICATO; 23 measured with the scratchpad's `pngstats.py`, 5 opened by eye)

- **Frames that do not depend on the change are identical:** frames 289-3625 (menus, intro, loading) of run 42 equal run 41's debug session value for value in the 8 captures I compared (frame numbers differ by 3-5; luma 14.4, 29.9, 154.5, 108.5, 0.3, 0.3, 0.0, 0.0; B/R 0.12, 1.26, 0.64, 0.48, ...). So the game is deterministic up to the point where the 3D scene starts, and the comparison is clean there.
- **The 3D scene changed colour.** Opened: `dk-f004123.png`, `dk-f004181.png`, `dk-f004199.png` (play), `dk-f003842.png` (cut-scene). The brick buildings are red/orange, the parking line is yellow, the taxi's tail lights red, the subtitle
  "Drive to Roman's place." has its yellow, the face in the cut-scene has skin tones. In run 41's frame 4138 the same places were blue/cyan.
- **Numbers (whole frame; regions are 2-pixel samples):**

| Frame | whole B/R | building (150-450, 250-300) B/R | road (300-1000, 520-700) B/R, saturated pixels |
|---|---|---|---|
| run 41 debug 4138 (bright state) | 1.33 | 1.64 | 1.34, 22.5 % |
| run 42 4123 (bright) | 1.06 | 1.12 | 1.04, 75.1 % |
| run 42 4151 (bright) | 1.07 | 1.21 | 1.05, 61.2 % |
| run 42 4199 (bright) | 1.11 | 1.40 | 1.07, 48.2 % |

  The frames are **not the same moment** (the car moves; 4199 shows another view) and the whole-frame ratio is not a hue measure when a third of the pixels are saturated. Read it as: the blue excess fell from 33 % to 6-11 % overall.
- **M7's numeric criterion (B/R 0.5-0.8) was not met**; its other criteria were (line present, brick red, yellow lines, HUD and tail lights unchanged, counters and stall unchanged, no error). I record both. The reason the
  ratio does not reach 0.5-0.8 is STIMA: the lit areas are saturated white now (road 48-75 % of the pixels at 250+, against 22 % in run 41), which pulls any ratio to 1; the cast was removed from the albedo, the overexposure of the
  lighting stayed.
- **What did not change (VERIFICATO by eye and by the counters):** the black quads on the ground and the car body, the white and black wedges bounded by straight diagonal edges (spot-light cones), three discrete brightness states
  in the same scene (bright ~110-130 luma, dark ~9-36, with a dark frame every ~15-40 frames: 4140, 4165, 4181, 4217, 4235), missing letters in the tutorial text ("ess RT t acc at ..."; the subtitle is complete), the stall (frames 3809-3879,
  ~122 s of slot-fence waits in 13 windows; run 41 release: 109 s in 3808-3874), 1.1-1.8 fps at 2,400-3,600 draws per frame, 282 draws skipped for a shader still compiling (0.03 %).
  The dark frame (4181) is the one that looks like a night scene lit by ambient only, with the right colours; the bright frames look like the lights added without the ambient and far too strong.

### N4. Diagnosis (the three cases of the run-43 prompt)

**Modifica eseguita ed efficace per ciò che copre; il difetto che restava non era più il canale.** The evidence: the build's own log line, the visible change of hue in exactly the places the diagnosis predicted (brick, lines, subtitle, skin, tail lights),
the frames that never reach the branch identical to run 41. What the swizzle never explained (black quads, wedges, three states, overexposure, glyph holes) was announced in M7 as unchanged and it is unchanged. Nothing in the run
refutes the diagnosis; the "no improvement" reading came from looking for the other defects.

### N5. The next discrepancy, and why this one

Candidates, with what exists as evidence (nothing below is proven):

| Candidate | Evidence for | Evidence against / missing | Cost |
|---|---|---|---|
| **Depth clamp for two-sided z-fail light volumes** | a **documented divergence** on the very draws the artefacts live in: the Vulkan path clamps them (`graphics_system.cpp:7121-7141`, `native_stencil_volume_policy.h`), dk counts them (**14,302 in 622 deferred frames, 23 per frame over the run, ~88 per frame in play**: 12,721 → 14,302 between frames 4204 and 4222) and sets `depthClampEnable` for none (the title requests it for 0). The artefacts are what an unbalanced z-fail count looks like: lit/unlit regions with straight edges (the cones), volumes lit as geometry (the cut-scene spheres of run 41, frame 3865), black where the count is wrong | not measured that these volumes cross a clip plane in this scene; the gate (`IsLocalStencilSetup`, a title lighting context) is not in this front end; does not explain the three states, the glyphs or the stall | one assignment + tests + counters |
| Second packed alias by address | 9,993 wrong-type fetches (~86 per frame in play), a documented mismatch (K) | LRA's pass equals ours (N2.2); no source says the lights read it; the first alias (12,802 fetches served) changed nothing visible | a lookup rule |
| HDR colour exponent / NaN scrub | exponent 1 on 1,831 resolves, "not applied"; LRA scrubs NaN (`resolve_convert_ps.glsl:138-142`) | no log of the render target's exponent; the three states are not explained by a constant | a shader pass |
| Depth hand-off | 1 per deferred frame dropped (623) | effect is theory (forward phase) | a new pass and shader |
| Skip PS `2673E2AF` | LRA's default; the stall is GPU-side in three runs | it is a stall fix, not a picture fix; Android-specific hang? | a cvar, separate session |

Chosen: the depth clamp: the best-documented divergence of the draws the artefacts belong to, one variable, a measurable counter, testable decision. **Why not automatically:** it is chosen after the run-42 result shows
that the colour chain was the only part the change touched, and its gate is an approximation (N6).

### N6. The change (uncommitted, tree `17f3a18-dirty`)

- `dk_recorder.cpp`, draw path: before the fixed-state block, `depth_clamp = ShouldEnableNativeDepthClampForDraw(x.depth_clamp_enable != 0, true, volume)` with the same `volume` state
  (`NativeStencilVolumeDepthState`) that the counter of the gaps already built; then `rs.depthClampEnable = depth_clamp ? 1 : 0` before `dkCmdBufBindRasterizerState`. It is the **Vulkan path's own policy header, unchanged from commit
  `17f3a18`**, called with `light_setup_phase = true`: **the gate is the volume's own state** (depth test, no depth write, stencil on, two-sided, no cull, no colour write, a face with a depth-fail op and a keep pass op), because the title's `LightingContext` is
  not carried by this front end. The draws that also carry the title's light-setup phase marker (`RenderPhase::kLightSetup`) are counted, to say how far the two agree.
  The title's own clamp request (never seen) is honoured too.
- Counters in a new log line per interval: `deko3d: depth clamp (totals): requested by the title N, z-fail light volumes N (under the light-setup phase marker N), draws drawn with depthClampEnable N; resolved colour fetches N (0x60A viewed as stored N)`.
- The resolved-colour log text fixed (N1). Nothing else changed.

### N7. Offline tests (VERIFICATO)

- `python tests/deko3d/test_depth_clamp.py`: **OK**. A z-fail light volume is clamped; a light contribution (colour write), a draw that writes depth, a z-pass volume, a culled or one-sided draw, a zero stencil mask and a plain draw are not; the title's own
  request is; one face z-fail is enough; 12,288 combinations of the fields the rule reads agree with the rule written out again (6,168 clamped, 0 bad). Wiring checked in the text: the decision comes before the rasterizer state and the
  state is set from it before the bind; the Vulkan call is the same; the policy header is byte-equal to commit `17f3a18`'s.
- `python tests/deko3d/test_view_swizzle.py` re-run: **OK**.
- **Not tested offline:** the effect of the clamp on the picture, and on the pipeline of the Maxwell GPU (a libdeko3dd error would be the first sign). That is run 43's measurement.

### N8. Builds (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run43-build dk-debug dk`, `-j3`, one at a time (no ninja/cc1plus before), exit 0, 0 compiler warnings (the only CMake notice is the usual `rex_version` one), tree `17f3a18-dirty`
(`COMMIT`); the new strings `draws drawn with depthClampEnable` and `swizzle applied as fetched` are in both ELFs:

| File (`out/console-runs/run43-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` (debug library: **the run**, comparable with runs 36-42) | 46,876,869 | `b0cc4fa54d16d70c5d8e7f50c8d5a524daaed093cf33b04346a1a25998e113a5` |
| `theft4-dk.nro` (release library, same source: not needed) | 46,844,101 | `815f379cd14ed112b45ea08b950d6a1ef3c8fe6400de17dfdaaf02301745bd14` |
| `switch-game-dk-debug.elf` / `switch-game-dk.elf` | 314,400,064 / 314,251,040 | – |
| `game-stage3.toml` | – | byte-equal to run 42's (SHA-256 `d7bf6677…95d3`) |

Runs 38-42's folders (`run38-build` ... `run42-build`) are untouched; run 42's debug NRO stays `be15feb50f6fa16b6a3e857813ee899da44001c366d437f1c9c025efd900ef1f`.

### N9. Run 43: what to do, what confirms, what refutes

- **Run:** `run43-build/theft4-dk-debug.nro` with `game-stage3.toml` as `game.toml`, the same `deko3d/` files, no cache to delete, empty `captures/`, old logs renamed, ~4 minutes after the mission screen, `+` held 1 s. Before starting, compare the NRO's SHA-256 on the card
  with the table above (`certutil -hashfile theft4-dk-debug.nro SHA256` on the PC): the log's commit string cannot tell builds apart, the new line `depth clamp (totals)` can. Copy back `game.log`, `game.prev.log`, `game-crash.log`, `game-exit.log` and the whole `captures/` into `out/console-runs/43-logs/`.
- **Confirms (STIMA until seen):** `depth clamp (totals)` shows `draws drawn with depthClampEnable` ≈ `z-fail light volumes` (≈ 88 per deferred frame); no `libdeko3dd` error/warning; and in the frames after the cut-scene the straight-edged wedges and the black quads are smaller or gone, and the cut-scene frame
  (3857-3885) no longer shows volume shapes. Measure with the same regions as N3 (dark pixels in the bright state were 34.6-36.2 %).
- **Refutes:** the counters as above and no change of the wedges / black quads in a comparable frame (then the stencil count is not what is wrong here; the hand-off, the second alias or the HDR path come next, in that order of the evidence).
- **Fails:** a libdeko3dd error or warning (a depth-clamp state refused), a different draw count, a missing picture, frames drawn black where they were not.
- Expected **unchanged**: fps, the stall, the three brightness states, the glyph holes. If they change, I report it as a difference of one sample, not as an effect.

### N10. Limits

- One sample per configuration; 10-second captures of a 1.4 fps game; the frames of run 41 and 42 are not the same moment, so every number in N3 is a range, not a before/after pair.
- The number of resolved-colour fetches viewed as stored is **not** known for run 42; only that at least one 60A fetch existed (and which image).
- That the volumes cross a clip plane in these scenes is not measured; the clamp is chosen from the divergence and the shape of the artefacts. If run 43 shows no change, the diagnosis of the wedges stays open.
- The copy-swap model of step N2.3 is an explanation, not shown.

## 2026-10-29: run 43 read (O): the clamp ran on 7,931 volumes and changed nothing visible; the scene depth never reaches the surface the lights use; run 44

Sample: one session, `out/console-runs/43-logs/` (`game.log` 1,713,723 B, SHA-256 `272f948f…0d8c`; 18 PNG, all measured, 5 opened). **Campione unico: nessuna differenza è dimostrata.**

### O1. Was run 43 executed as built? (VERIFICATO from the log)

| Question | Evidence |
|---|---|
| NRO / config | `build: commit 17f3a18-dirty, deko3d (debug library libdeko3dd)`; `cvars not at the default (1): nx_gpu=deko3d nx_dk_stage=3 nx_dk_capture_s=10` (+ gamma 1.0) = `game-stage3.toml`. The commit string does not identify the build; the new line `deko3d: depth clamp (totals)` exists only in run 43's source and is in every report. |
| The clamp reached the draws | last report: `requested by the title 0, z-fail light volumes 7931 (under the light-setup phase marker 0), draws drawn with depthClampEnable 7931`: **every one of the 7,931 z-fail volumes was drawn with `depthClampEnable`**. The phase marker matched **0** of them: `kLightSetup` is never emitted to this front end (the hooks do not send it, and no `LightingContext` travels), so the marker cannot be the gate; the volume's own state is. |
| Run 42's change | `resolved colour fetches 99092 (0x60A viewed as stored 54292)`: 55 % of the resolved-colour fetches had the `60A` swizzle (answers N1's open count). |
| Health | 0 faults, `libdeko3dd errors 0 warnings 0` in every report, exit 9 steps `_exit` at +7.06 s, clocks 1020 / 460.8 / 1331.2 MHz = wanted, 251 s, frames to 4391 |
| Counters unchanged from 42 | alias: 530 conversions, 6,801 first-alias fetches served, 6,823 wrong-type fetches (~86 per deferred frame); `depth hand-offs 530` dropped = one per deferred frame; resolve clears 530 done |

### O2. What changed in the pictures (VERIFICATO by eye and by numbers; frames are not the same moment)

- Opened `dk-f004317.png` (play, bright state), `dk-f004372.png` (dark state), `dk-f003864.png` and `dk-f003842`-like cut-scene frames of run 42. **Frame 4317 shows the same picture as run 42's 4123 and run 41's 4138**: the same black quads on the ground and the car body, the same white wedge and
  straight-edged diagonal boundaries, the white road. Numbers (bright state, whole frame): dark pixels 32.8-35.7 % (run 42: 34.6-36.2 %), B/R 1.04-1.10 (1.06-1.11), luma 97-110 (109-132).
  Frames 284-3677 equal run 42's value for value (deterministic up to the 3D scene).
- **The depth clamp had no visible effect.** The straight-edged shapes and the black quads are therefore not (only) near/far clipping of the z-fail caps. The clamp stays in the tree (it is the Vulkan path's behaviour and costs nothing), but it is **not the cause**: REFUTED as the explanation of the wedges, not as a correctness item.
- The subtitle glyph holes are intermittent (4372: "D i e to Ro  plac ."; run 42's 4181 and 4199 show the whole line): not a fixed missing glyph. Not explained.
- Brightness states: bright (4305-4340: luma 97, 110, 62, 75) and dark (4355-4385: 3, 10, 15) alternate as before.

### O3. The stall and the frame rate (VERIFICATO from the log; the cause of the GPU time is NOT known)

- Series (`series43.py`, per 10-s report): 28 fps (menus) → 59 fps (2D) → 40 fps (3D, 174-182 draws/frame) → **18.8 fps at 315 draws (cut-scene)** → **0.6 fps at 570-630 draws, frames 3860-3890, slot-fence waits 9.4-9.8 s per 10 s (43 s in total)** →
  30 fps at 94 draws (a black phase) → **1.1-1.5 fps at 2,250-3,500 draws (play)** with **0 ms of fence waits**. Run 42's stall had ~122 s of fence waits and run 41's 109 s: the same cut-scene cost 43 s here, 3x less. Do not read a cause into one sample.
- The window starts with a new target: a **4096x4096 resolved texture (guest format 31, host RG16F, 64 MB image chunk)** and a 512x512 RT + depth (log 10259-10262), after the 2560x2560 depth/colour targets of the cut-scene's shadow pass.
  Correlation, not cause.
- **What the CPU does in the stall** (the `cpu` line at heartbeats 100-130 s, 100 % = one core): guest render thread (id 16) **82-84 %**, id 17 25-29 %, **host id 2 13-15 %**, everything else ~2 %. **Which thread "host id 2" is, is NOT established**: the CPU line lists only XThreads, the recorder is a plain `std::thread` (audit D2), and id 2 may be the null GPU's command thread. The fence-wait total (9.4 s of 10 s) says the recorder is blocked on the
  GPU in that window; it is **not** proof that the GPU is saturated (what the GPU executes is unmeasured), and an 83 % busy render thread at 0.6 fps is odd (spinning on something, or real guest work) and is unexplained. Run 44 gives the recorder its own CPU row.
- **Play (1.1-1.5 fps)**: render thread 55-58 %, id 17 36-44 %, id 12 32-45 %, host id 2 ~30 % (identity open, see above), **no fence wait** (recorder `rest` 53-76 µs per draw: time inside `Draw()` that the component timers do not name, with libdeko3dd checking every call; the release library cut it 30-45 % in run 41 and the frame rate did not move).
  Arithmetic: 3,489 draws x ~117 µs of wall time inside `Draw()` = 0.41 s of a frame of ~0.83 s, so the recorder is **not busy the whole frame**, and the GPU does not make it wait (0 ms of fences). That puts the limiter **upstream of the recorder** (guest threads, the front end on them, waits between them) **or** in a wait the timers do not see.
  VERIFICATO: not the fence, not the recorder's wall time inside `Draw()`. **Not measured**: where the title's threads spend their time (the audit finds no place where the title waits on the completed-frame word; its spin waits are on the ring read pointer), whether `front_mutex_` held across a Present wait serialises the title's threads (audit D1), and the allocator contention of ~3,300 packets/s (D6). Run 44 adds the first measures (O6).
- uam: 255 shaders from the PC's DKSH, **0 compiled by the console's uam, 4 compiling for the whole session** (jobs that never finish; their draws are skipped: reason 13 = 320 draws of 755,307, reason 10 = 4). Not the cause of the stall; possibly of text quads (DA VERIFICARE).

### O4. The lighting pipeline, rebuilt from the title's commands (VERIFICATO in code unless marked; investigators' reports checked against the lines they cite)

Order of one deferred frame (hooks `gta4_native_hooks.cpp`, LRA's `graphics_system.cpp`, the run-42/43 logs):

1. Offscreen passes (shadow atlas 2560x2560, reflections).
2. `kSceneToGBuffer`: three RGBA8 MRTs + the **deferred depth** `40218360` (1280x720, run 43; `402266E0` in run 42).
3. `sub_824F6AF0`, the end-of-deferred hook (hooks 6837-6989): end marker; the resolves of depth (flags 20), colour (16), the alias (768 = colour + **clear depth/stencil**), 273, 274, all with clear depth **0.0**, stencil 0; **then `DepthSurfaceHandoffCommand`** from the deferred depth to
   `primary_depth_surface` (the title's global `0x831C23B0`) with `kRebuildSceneCoverage`; then the begin marker.
4. `kLightsToScreen`: the two-sided z-fail volumes, then the light contributions; then the forward phase, an end resolve, post-fx.

What the **Vulkan path** does with the hand-off (`RecordDepthSurfaceHandoff`, 26823-27128): clears the primary depth to **0.0 / stencil 0x80** and draws one full-screen triangle with the resolved scene depth: depth test ALWAYS + write, stencil ALWAYS/REPLACE 0xFF, a pixel shader that
**discards where the packed depth is 0** and writes `gl_FragDepth = depth`. The primary depth surface therefore holds the scene depth and a coverage stencil (0xFF covered, 0x80 empty) when the lights and the forward phase start; the Vulkan code says what its absence does (`left the stencil bit
required by deferred_lighting_ps0 uniformly zero`, 21200-21205).

What **our recorder** did through run 43: the front end counted the command as dropped (`depth hand-offs 530`: one per deferred frame), so the primary depth surface `400007C0` (1280x720, created at start with depth 1 / stencil 0, line 1280) **never received the scene**. Predicted from the code (INFERENZA, not seen): the z-fail volumes and the light draws test against a constant plane
with stencil counts that reflect the volume geometry alone, and the forward phase tests against residue: straight-edged lit/unlit shapes, black quads, lights that appear only inside volume silhouettes. That is the best fit for the wedges, the black ground and the sphere shapes of the cut-scene, and **it was
listed as "forward phase only" in L2/M8** from theory; the hook comments and the Vulkan code say otherwise.
**What is not proven:** that the lights bind the primary surface (`400007C0`) rather than the deferred depth: no log names the depth handle of a light draw. Run 44 logs it (O6).

| Visible anomaly | Pipeline stage that can produce it | State |
|---|---|---|
| wedges with straight edges, volume-shaped lighting, black quads, sphere shapes in the cut-scene | z-fail stencil count against a depth buffer without the scene (hand-off dropped); not the near/far clip (run 43) | **run 44 change**; prediction DA VERIFICARE |
| three brightness states (bright / ambient-only dark / lights only) | which of {ambient pass, light accumulation} passes the stencil/depth gate, frame by frame; HDR resolves with an unapplied exponent (1,561 per session, NaN scrub absent) | not touched; the new depth-surface log says whether the gates move |
| blue cast | resolved colour viewed with the memory-order swizzle | fixed in run 42 (VERIFICATO) |
| intermittent holes in text | 4 uam jobs that never finish (320 skipped draws), `vertex inputs without an element` 1,658 | unproven |
| 0.6 fps cut-scene stall | GPU time of an unidentified pass (new 4096x4096 resolve, 2560x2560 targets) | run 44 measures it (timestamps per pass) |
| 1.1-1.5 fps in play | upstream of the recorder (title threads, front end, waits between them) or an unseen wait; not the fences, not the recorder's time inside `Draw()` | run 44 measures it (thread times, recorder CPU row) |

**The depth clamp (run 43) and its gate:** the Vulkan policy is `guest clamp || (light_setup_phase && two-sided z-fail state)`. Our front end has neither the phase marker (0 matches of 7,931) nor the `LightingContext`; the state test alone gave 7,931 clamped volumes and the same pictures. The policy as copied is sufficient to *apply the Vulkan behaviour*; whether it is *needed* is refuted by the picture.

### O5. The run-44 change: the scene depth hand-off (uncommitted, tree `17f3a18-dirty`)

One main change, the same pass as the Vulkan path's, adapted to deko3d (no code is copied: the GLSL `depth20e4` is the one already used by the alias pass, equal to LRA's text; LRA's licence/attribution are those of the project's existing sources):

- `native_front_end.cpp`: `kDepthSurfaceHandoff` is passed on in order as `RecorderDepthHandoff` (malformed ones still counted as dropped); `native_recorder.h`: the packet and `INativeRecorder::DepthHandoff`; `dk_graphics_system.cpp`: dispatch.
- `dk_depth_handoff.h` (new, no Switch headers): the verdict `PlanDepthHandoff` (in the Vulkan path's order of rejections), the pixel model `SceneHandoffPixel`, the pixel shader `kSceneDepthHandoffPs`.
- `dk_recorder.cpp`, `DepthHandoff`: resolved depth `resolved_[source_texture]` (written, depth, same size as the destination, guest format 22/23); destination `Surface(destination, true)`; barrier, clear depth 0 / stencil 0x80, depth test ALWAYS + write, stencil ALWAYS/REPLACE with write mask, reference and compare mask 0xFF on both faces, the alias pass's
  full-screen vertex shader and the new pixel shader (discard where the packed depth is 0, `gl_FragDepth = depth`), a point fetch of the resolved depth, a `Fragments` barrier, `ForgetState`. The recorder's stencil cache is set to an impossible value so the next stencil draw always sends its own (found by the auditor, H1).
  Pass compiled at start with the console's uam (non-fatal: without it the surface stays as before).
- Two defects of the resolved-image bookkeeping the audit found are fixed on the way: a recreated resolved image kept `written`/`writes` of the old one.
- **Kept from run 43:** the depth clamp (one variable per run means this run adds the hand-off; the clamp changed nothing visible, so it cannot confound).

### O6. Read-only measures added in run 44 (no change of behaviour; the answer to "what is the time spent on")

- **Threads** (`dk_graphics_system.cpp`): every 10 s, `deko3d: threads in the last N ms`: time inside the title's `SubmitTitleCommand` split into the wait for the front-end lock, the front end itself, the queue push, and the **wait at a Present for the recorder**; the recorder thread's time waiting for a packet against recording; the queue's maximum depth.
  Reading: if the Present wait is ~0 and the recorder waits for packets most of the time, the frame time is the title's (guest threads and front end), not ours.
- **GPU time per pass** (`dk_recorder.cpp`, `StampPass`/`HarvestStamps`): a GPU timestamp (`dkCmdBufReportCounter`, the core's report block) at the start of each work, at each change of render targets, at each resolve and hand-off and at the end of the work; read back when the slot comes round. Line `deko3d: GPU time from timestamps in the last N s`:
  works finished, **busy ms**, idle/uploads between works, longest work, and the six passes that took most (targets, size, draws); `slow work` lines (> 250 ms) name the longest passes of that work. This separates "the GPU is busy" from "the GPU waits for us", which the fence-wait total cannot. The stamps serialise the GPU at each pass boundary (a small cost; stated so the numbers are read as upper bounds).
- **The recorder's CPU row** (`main.cpp`, `LogCpu`, `dk_graphics_system.cpp`): the recorder's Horizon thread handle is published and the `cpu` line gets a `recorder` row, so the recorder's CPU is no longer inferred from "host id 2".
- **Depth surfaces** (`dk_recorder.cpp`): `draws by depth surface (totals)`: per guest depth handle the draws, the z-fail volumes, the depth writers, the stencil testers, and which one is the hand-off's destination; `depth state on surface ...`: the first 24 distinct depth/stencil states (function, reference, masks, ops, shaders); `title clear of depth/stencil ...`: the first 24 distinct clears.
  These answer the open question of O4 (which surface the lights test against).

### O7. Tests and builds (VERIFICATO)

- `python tests/deko3d/test_depth_handoff.py`: **OK**. (1) The 14 decision cases give the Vulkan path's verdicts in its order; (2) 9,034 pixels (8,398 covered, 636 empty, edge values: 0, -0, inf, denormal-range, half a fixed-24 step) the C++ pixel equals an independent Python one in both depth encodings; (3) a **model of z-fail light volumes**
  (not the title): 4,000 pixels, the lit set is exactly the pixels whose scene surface lies inside the volume (1,143) against the hand-off's buffer, **0 pixels** against a freshly created (depth 1) or a cleared (0) buffer, and 1,133 pixels of which only 475 right against last frame's; (4) `depth20e4` is the same text as LRA's `scene_depth_handoff_ps.glsl`, both discard on 0 and write
  `gl_FragDepth`, and the PC uam compiles the pass (1,024 B); (5) the pass state in the recorder, the clear values, the front end's pass-on and the Vulkan constants are as in `depth_handoff_util.h` and `RecordDepthSurfaceHandoff`. The other tests of the folder were re-run: all OK (alias, clamp, view swizzle, resolve clear, dksh scratch, runtime package).
  **Not tested offline:** that the lights bind that surface, that the pass compiles and runs on the console (the start-up line and `scene depth hand-off (totals)` say), and any picture.
- Build: `tools/switch-game/build_variants.sh out/console-runs/run44-build dk-debug`, `-j3`, one at a time (no ninja/cc1plus before), exit 0, 0 compiler warnings, tree `17f3a18-dirty`; the new strings are in the ELF.

| File (`out/console-runs/run44-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` (**the run**) | 46,909,637 | `4e973ddcfec88153f017eacd56e0b8f800ca94ea8a632536925bce65e5108975` |
| `game-stage3.toml` | – | byte-equal to run 42's/43's |

`run44-build/` holds only the last build of the session; three earlier builds of the same day (SHA `2af611e3…13e0`, before the audit's fixes; `ae56a125…7b11`, before the recorder's CPU row; `4c9a1c23…40b4`, before the lights-marker split of the depth-surface counters) were overwritten and are not for a run. Runs 38-43's folders are untouched.

### O8. Run 44: what to do, what confirms, what refutes

- **Run:** `theft4-dk-debug.nro` + `game-stage3.toml` as `game.toml`, same `deko3d/` files, empty `captures/`, old logs renamed, compare the NRO's SHA-256 on the card first; ~4 minutes after the mission screen, `+` held 1 s. Copy back `game.log`, `game.prev.log`, `game-crash.log`, `game-exit.log` and `captures/` to `out/console-runs/44-logs/`.
- **It ran (log):** `scene depth hand-off pass compiled by the console's uam`; `scene depth hand-off (totals): ... N commands, N done` with `done` ≈ the number of deferred frames (~one per frame) and rejections 0; `depth hand-off ... into the depth surface XXXXXXXX` (names the surface).
- **Confirms (STIMA):** in `draws by depth surface` the z-fail volumes bind the hand-off's surface (`(the hand-off's)`); and in the play frames after the cut-scene the black ground quads and the straight-edged wedges shrink or go, the sphere shapes of the cut-scene are gone (compare dark-pixel share in the bright state, 33-36 % before, and by eye on a comparable frame).
- **Refutes:** the volumes bind another surface (then the hand-off cannot reach them: read the `depth state` lines and the clears: the next candidate follows from them), or the hand-off is done and the pictures are the same (then the stencil/depth gate is not what is wrong).
- **Fails:** a libdeko3dd error/warning (a state or a shader refused), rejections > 0, black frames where there were none, draw counts that change beyond noise.
- **Measures to read first (independent of the pictures):** `threads in the last` (title threads: Present wait vs front end; recorder waiting vs recording) and `GPU time from timestamps` (busy ms against the 10 s, the top passes, and the stall's `slow work` lines). The expected unchanged: fps in play, the three brightness states, the glyph holes.

### O9. Limits

- One sample per configuration. The stall's GPU time and the play frame time are **not explained** by the existing telemetry; run 44's measures are the minimum that can. The GPU timestamps' report layout (value + timestamp, `dkTimestampToNs`) is assumed from the header; the first two reports of a work are logged raw to check it.
- That the lights use the primary depth surface is inferred from the title's hook and the Vulkan code; the run logs it.
- The hand-off could be a necessary condition without being sufficient (investigator's words): the three brightness states and the text holes are not claimed.
- The release library is not measured in this run (run 41: -30..45 % recorder CPU, fps unchanged); a release gain does not prove a renderer fix.

### O11. The adversarial cross-check of the choice (a separate investigator told to refute it; verdict and what I did with it)

**SUPPORTED for the narrow claim, PARTLY for "best single intervention".** Not refuted: nothing else fills the primary depth surface `400007C0` (the title's own depth copy writes a colour RT; our recorder has no colour-to-depth aliasing), and no code or log evidence puts the lights on the deferred depth.
Still an inference: that the lights bind `400007C0` (the hooks emit only four phase markers, `kSceneToGBuffer`, `kLightsToScreen`, `kRadarMap`, `kCompositePostFx`; `kLightSetup`/`kLightDraw` are never sent, so "0 of 7,931" is zero by construction, and the front end never copies the `LightingContext`).
A weak point in favour: the run-43 clamp "changed nothing" is also what a depth buffer without scene content gives (front and back caps cancel whatever the clipping), so the null result mildly supports the hand-off instead of refuting it.
Competing explanations kept open: the 4 never-compiled vertex shader variants for some black quads; stencil residue that is never reset per frame for the discrete brightness states (unverified); the row orientation of `texelFetch(gl_FragCoord)` (R4, unverified); a depth clear gated by the depth-write state (R3, unverified; libdeko3d's source is not on disk).
**Done with it:** the depth-surface counters now also split the draws **under the `kLightsToScreen` marker** (the one lighting marker the hooks do emit) per depth surface, and the depth-state and hand-off log lines print the surfaces' guest `sample_type` (multisampled or not) and the first colour target, so run 44 answers which surface the lights bind and whether the deferred depth is multisampled.
**Expectation to hold myself to:** if the hand-off runs (`done` ≈ frames, 0 rejected) and the sky, the wedges and the black ground are unchanged, the primary depth's content is not the gate: the next suspects, in order, are R4 (orientation), the stencil plane of the copy-engine depth resolve (E2), then the front-face orientation (C8).
### O10. What the references say about this pass (read by an investigator, quotes checked against the lines cited; licences matter, nothing is copied)

- **nfsmw-nx** (`app/src/nfsmw_dk_destinos.cpp:536-614`, `Copiar3D`; shader 285-293) is the one sibling with the same shape on deko3d: a depth-only bind, colour mask 0, depth test ALWAYS + write, a pixel shader writing `gl_FragDepth`, the source sampled through a depth view with RRRR, a `Fragments` barrier with image invalidate before. Our pass has the same shape; the state the reference sets and ours does not: none found.
  UnleashedRecomp copies depth with a pure `SV_Depth` full-screen pass too; Renut (`sa2-nx`) never writes depth (it writes depth into colour targets) and has no licence: behaviour only.
- **Pitfalls documented there:** stencil cannot be exported on Maxwell (masseffect-nx: one masked draw per bit): the hand-off needs only the two values 0x80/0xFF, so one REPLACE pass is enough; a depth export disables early-Z and ZCULL (0.7-1.0 fps when applied to scene shaders; harmless for one pass; our queue has ZCULL off, `DkQueueFlags_DisableZcull`);
  compressed depth must not go through the copy engine (not enabled here); Maxwell clips to the smallest bound target (counter `target binds of unequal size` = 0).
- **Depth clamp on deko3d:** no sibling sets `depthClampEnable`; the rule that also clamps z-fail light volumes exists only in LRA; deko3d has only `depthClampEnable` (no bounds in the depth state). What the Maxwell programs for it (near, far or both) could not be verified (libdeko3d's source is not on disk).
- **Colour exponent:** only masseffect-nx applies it (a pass multiplying by 2^bias) and records "overexposed scene" when ignored; the NaN scrub is in no sibling. Our "not applied" stays a candidate for the brightness level, not for the three states.
- **A risk the references raise (E2, unverified):** our depth resolve is `dkCmdBufCopyImage` between ZF32_X24S8 images; no sibling shows that the copy engine carries the *stencil* plane, and the packed alias reads it. The hand-off does not use it (depth only). If the alias's stencil byte is zero, the title's stencil-gated lighting would see it; a candidate if the hand-off alone is not enough.
- **uam patches 7 and 8** (sa2-nx `PATCHES.md`: no dual issue; a stall before a new block) fix lit-surface warps flickering and light-volume halo flicker; our uam source has both and the package tag says `patches 1-8`; that the DKSH on the card was built from it is not verified.
- **Licences:** nfsmw-nx, sa2-nx, UnleashedRecomp GPL-3.0 (our `COPYING` is GPL-3.0); ReXGlue/Xenia BSD-3; plume MIT; uam zlib/MIT/MPL-2.0; Renut none; the Thorhax text CC BY 4.0, no code. The pass is eight lines of state and a 20-line shader that is LRA's (already in the tree): **nothing was adapted verbatim.** A trap noted for later: sa2-nx's `invalidateZcullAfterCopy` has misleading indentation (the Zcull barrier runs unconditionally): do not copy it.
- **Thorhax docs** (CC BY 4.0, documentation only) say nothing on depth clamp, stencil volumes or depth export beyond the "depth smaller than colour" clip rule.

Audit items not acted on in this run (the order of the evidence, for later): depth bias is off at stage 3 and its slope term lacks the 1/16 (C1); no colour-output epilogue / resolve exponent / NaN scrub (C2); front-face orientation of deko3d never validated on the console (C8: a cvar that inverts `frontFace` for one run is the cheap test); the clamp's gate constant `true` (E1/C5);
cull modes 3/7 and blend-factor defaults differ from the Vulkan path (C3, C4: low).
## 2026-11-02: run 44 read (P): the hand-off runs on every deferred frame and removes the black ground and quads in the dark state; play is bound by the title's thread, not the GPU

Sample: one session, `out/console-runs/44-logs/` (`game.log` 1,967,922 B, SHA-256 `29FA4EDA…20476136`, 310 s; 22 PNG, all measured with
`pngstats.py` (scratchpad), 5 opened). **Campione unico: nessuna differenza è dimostrata.**
State of the tree found at the start of this reading: the run-44 log arrived at 21:25 on 2026-10-29; at 21:54-21:55 a session that left no devlog entry
changed the hooks, the front end, `dk_graphics_system.*` and added `native_constant_diff.h` + its test ("run 45" in the comments). Those changes are
described, completed and built in Q; nothing of them is in run 44's NRO (built 13:22).

### P1. Was run 44 executed as built? (VERIFICATO from the log)

| Question | Evidence |
|---|---|
| NRO / config | `build: commit 17f3a18-dirty, deko3d (debug library libdeko3dd)`, `nx_gpu=deko3d nx_dk_stage=3 nx_dk_capture_s=10`; the run-44-only lines (`scene depth hand-off`, `threads in the last`, `GPU time from timestamps`, `draws by depth surface`) are present |
| The pass | `scene depth hand-off pass compiled by the console's uam in 16.8 ms` (line 110) |
| The hand-off ran | last totals: **455 commands, 455 done, 0 rejected** (every reason 0); `depth hand-off DA1A6320 (guest format 23, 1280x720) from the surface 402266C0 (sample type 1) into the depth surface 400007C0 (sample type 0)` |
| Which surface the lights use (O4's open question) | `draws by depth surface (totals)`: **`400007C0 (the hand-off's)`: 450,075 draws, 12,519 z-fail volumes**, 162 under the lights marker; every other surface 0 z-fail volumes. **VERIFICATO: the volumes test against the surface the hand-off fills.** The deferred depth `402266C0` is multisampled (sample type 1), the primary is not |
| Health | 0 faults, `libdeko3dd errors 0 warnings 0` in every report, `+` → exit in 9 steps (`_exit` at 310.6 s), clocks 1020 / 460.8 / 1331.2 = wanted; 1,300 MiB headroom at the end |

### P2. The pictures (VERIFICATO by eye on 5 frames and by numbers on 22)

- **Dark state, play** (`dk-f004134`, `-4192`, `-4205`, `-4101`, `-4117`; luma 22-30): a dark night scene that reads as GTA IV's docks: lit windows, wet-road reflections, red tail lights glowing on the
  ground, the HUD and the radar right. **The white road, the black ground quads and the straight-edged wedges of runs 38-43 are gone in this state.** Dark pixels **4.6-21 %** (one frame 38 %)
  against **32.8-36.2 %** in runs 42/43's play frames: the run-44 prediction (O8 "confirms") holds for the dark state.
- **Bright state** (`dk-f004088`, `-4146`, `-4155`, `-4162`; luma 161-197, 9-31 % saturated, B/R 1.3-2.2): **still wrong**: an overexposed blue-white scene with large straight-edged
  wedges and pink patches on the road (`dk-f004162` opened). The brightness states (run 41 M5) are **not** fixed by the hand-off: they alternate as before (4075 → 4088 → 4101).
- **Geometry**: the vehicles and the cut-scene face (`dk-f003682`) are **shattered**: triangles missing and spikes, in both states. Not a depth or stencil defect (the shapes are in the
  geometry itself); candidates: the 4 vertex shaders that never compile (draws skipped for reason 13: 565 in the session, rising in play), `vertex inputs without an element` (1,870), the
  skinning constants. DA VERIFICARE.
- **Text**: glyph holes remain ("Press [RT] to accele t  Press", "Rom n s place"), intermittent as before.

### P3. Where the time goes (VERIFICATO from the new measures; first time these are known)

Per 10-s report (`threads in the last`, `GPU time from timestamps`, `cpu`), three regimes:

| Phase | fps | Title threads inside `SubmitTitleCommand` (of 10 s) | GPU busy (of 10 s) | Reading |
|---|---|---|---|---|
| 2D / menus (frames 873-2655) | 59-60 | 9.7 s, of which **9.7 s waiting at a Present** | 1.3-3.0 s | vsync-paced; nothing to fix |
| Cut-scene stall (frames 3642-3694, ~90 s) | 0.6-0.7 | 12.9-17.9 s; **Present wait 9.3 s**, front-end lock wait up to 7.7 s | **9.5-11.6 s** (saturated) | **GPU-bound**: 53 `slow work` lines; single passes of **one draw** take **770-800 ms** (`targets 40076EB0 400007C0 1280x720`) and 280-430 ms (`targets 400007C0 400007C0`, depth only). Which shader is not logged (Q adds it) |
| **Play** (frames 4082-4207) | **1.1-2.0** | 8.3-9.4 s: **front end 5.4-7.7 s + queue push 1.5-3.1 s**, Present wait **0** | **0.87-2.7 s** (9-27 %) | **CPU-bound on the title's side**: the recorder waits for packets 27-40 % of the time and records 48-60 %; the GPU idles 73-91 % |

- Play, per call and per draw (arithmetic on the same lines): 150-256k calls and 17-60k packets per 10 s; front end ≈ 21-30 µs per call, ≈ 100-150 µs of wall time per packet;
  queue push ≈ 10 µs per call. Recorder `record` 135-148 µs per drawn draw.
- `cpu` (100 % = one core, last reports): render thread id 16 **55-58 %**, id 12 37-38 %, id 17 34 %, host id 2 33 %, **recorder 32-34 %** (its own row, first time); sum ≈ 226 % of 300 %.
  The render thread is inside `SubmitTitleCommand` ~85-94 % of the wall time but on a CPU only ~57 % of it: **the wall time of the front end is partly not CPU** (preemption among the
  0x3B threads, 10-ms rotation, or waits): to be split by the sampled CPU ticks of run 45.
- VERIFICATO in the run-44 ELF (`__syscall_thread_create`): a plain `std::thread` (the recorder) is created at **priority 0x3B with core -2, then `svcSetThreadCoreMask(-1, process
  core mask)`**: the recorder floats over cores 0-2 at the guests' priority; it is **not** pinned to core 0 (the hypothesis I had to check first: refuted).
- The GPU numbers are upper bounds (a stamp serialises the GPU at each pass boundary), and the debug library checks every call.

### P4. What run 44 decides

1. **The depth hand-off is necessary and it works** (VERIFICATO: 455/455, the volumes on its surface, the dark state's black quads gone). It stays.
2. **Play is limited by the title's thread inside the front end and the queue**, not by the GPU and not by waits for the recorder. The first CPU intervention is on that path:
   fewer calls, a cheaper diff, a cheaper push (Q).
3. **The cut-scene stall is the GPU** (one draw of ~0.8 s): next measure = the shaders of the slow one-draw passes (Q adds them to the `slow work` line).
4. **Rendering, still open, in this order of evidence:** the bright state (wedges, overexposure, blue; the hand-off did not touch it; candidates: `kTextureLock` never reads GPU
   results back to guest memory, so whatever the title reads on the CPU (luminance/exposure) is stale (`ExecuteTitleCommand`, B4); the HDR resolve exponent (C2); stencil residue);
   the shattered geometry (reason-13 skips, vertex inputs, skinning constants); the glyph holes.

## 2026-11-02: run 45 (Q): the title's submission path made cheaper (fewer calls, a 64-byte constant diff), plus the measures that split its wall time into CPU and waiting

One intervention, aimed at P3's finding (play is bound by the title's thread inside `SubmitTitleCommand`), plus read-only measures. No rendering change.
Most of the code was written on 2026-10-29 at 21:54 by a session that left no entry (P); this session audited it, completed it, fixed one defect that made half
of it inert, and built it.

### Q1. The changes (uncommitted, tree `17f3a18-dirty`)

| # | Change | Files | Before | After | Off switch |
|---|---|---|---|---|---|
| 1 | **The hooks do not send the four state commands** (`SetRenderState`, `SetTexture`, `SetRenderTarget`, `SetDepthStencil`) when deko3d is the native renderer | `gta4-recomp/src/gta4_native_hooks.cpp` (`SubmitNativeCommand`) **and the copy the console compiles, `out/gta4-native-eu/src/gta4_native_hooks.cpp`**; `dk_graphics_system.cpp` sets the weak flag `g_theft4_dk_skip_state_commands` | ~80 % of the 150-260k calls per 10 s were these; each took the front-end lock, the timers and a `switch` that does `return true` (`native_front_end.cpp`: the front end reads the guest device at each draw and never used them) | not captured, not traced, not sent | **`nx_dk_skip_state_commands = false`** (new cvar, this session) |
| 2 | Constant diff on raw 64-byte groups | `native_constant_diff.h` (new), `native_front_end.cpp/.h` | 480 registers per draw byte-swapped and compared one by one; float shadow | shadow = the guest's big-endian bytes, `memcmp` of 64 B (4 registers), swaps only the registers that changed; 8 runs / 128 floats reserved on the first change | none (same output: Q3) |
| 3 | Timers on 1 call in 16 (x16), with the thread's CPU ticks | `dk_graphics_system.cpp/.h` | 4 clock reads + 5 atomic adds on every call (run 44's measure cost about what the cheap calls cost) | 1/16 of the calls; **new: CPU time of the sampled calls** (`ThreadCpuTicksNow`) beside their wall time | – (measure) |
| 4 | Recorder woken per batch | `dk_graphics_system.cpp` | `notify_one` per packet (a kernel wake-up each, 40-80 µs on a saturated CPU per the 21:54 comment: STIMA, not measured here) | woken at an event (anything but a draw) or 16 queued draws; it waits at most 2 ms; takes up to 64 packets per lock | – |
| 5 | **CPU by core** (read-only, this session) | `tools/switch-game/main.cpp` `LogCpu` | the `cpu` line sums each thread over all cores | new line `cpu by core`: per core 0-3 the load of the listed threads, and for the six busiest their ideal core, core mask and split by core | – |
| 6 | **Shaders of the slow passes** (read-only, this session) | `dk_recorder.cpp/.h` | `slow work` named targets only | each pass keeps its first draw's vertex/pixel shader hashes; the `slow work` line prints them (answers P3's one-draw 770 ms passes) | – |

**Defect found and fixed in this session (VERIFICATO):** the console build compiles the hooks from `out/gta4-native-eu/src` (`tools/switch-game/CMakeLists.txt` 249-254), the
European-address copy made by `tools/gta4_native_eu_translate.py`, not from `gta4-recomp/src`. The 21:54 edit was only in the latter: a first build of this session
linked a `gta4_native_hooks.o` of 2026-10-21 that does not reference the flag (`nm`): **change 1 was inert**. Fixed by applying the same two hunks to the EU copy by hand.
I did **not** regenerate the copy: the translator now writes different European addresses for unrelated hooks than the copy used by runs 37-44 (fusion timecycle hooks
`sub_822F5DE8/63A0/14B0/16A8/2478` → `sub_822CC388/C940/C7A08/C7C00/C89D0`, the skinned-submodel hook `sub_82855890` → `sub_828C4578`, the shadow-target globals, and
`minute_override` `0x82CFA27C` → `0x82DF3920` while `hour_override` stays `0x82CFA284`, which looks wrong). The census changed after 2026-10-20; regenerating would bring
untested hooks into this run. **Open item (DA VERIFICARE):** reconcile the census before the next regeneration; until then edits to the hooks go into both copies.

### Q2. Why this first (the evidence, not the theory)

P3: in play the title's threads were inside `SubmitTitleCommand` 8.3-9.4 s of 10 s, the GPU busy 9-27 %, the recorder idle 27-40 %, the Present wait 0. The path the title
pays for is the only one that can move the frame rate now; GPU work, the recorder's 140 µs per draw and rendering fixes cannot, until the title produces frames faster.
The run also has to say **how much of that wall time is CPU**: the render thread was on a CPU only ~57 % of the time while being inside the call 85-94 %.

### Q3. Offline tests (VERIFICATO)

- `python tests/deko3d/test_constant_diff.py`: **OK**: 8,000 sequenced draws on both banks (no change, one byte, a register, a block of four, 20 bytes, the whole bank, first
  draw of a device): runs, values and register counts equal the old loop for every draw; the shadow equals the guest bytes (`differences 0 shadow_wrong 0`, 303,782 registers, 24,755 runs).
- All other tests of the folder re-run: `test_depth_alias`, `test_depth_clamp`, `test_depth_handoff`, `test_dksh_scratch`, `test_resolve_clear`, `test_runtime_package`,
  `test_view_swizzle`: **OK** (exit 0).
- Packet sizes, host clang (LP64, same layout as AArch64): `RecorderDraw` 1,832 B, the `FrontEndPacket` variant 1,840 B, `RecorderClear` 496 B: input for R2 of the roadmap.
- **Not tested offline:** that skipping the four commands changes nothing on screen (the front end returned `true` for them without reading them: VERIFICATO in code; the
  captured command lists and traces are diagnostic paths not enabled in this configuration: see the investigator's report in 17-optimization-roadmap.md §3), the frame rate.

### Q4. Builds (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run45-build dk-debug dk`, `-j3`, one build at a time (no ninja/cc1plus before), exit 0, **0 compiler warnings**, tree `17f3a18-dirty`.
Checked in the outputs: `gta4_native_hooks.o` (EU copy) now references the weak `g_theft4_dk_skip_state_commands` (`nm`: `w`); the new strings (`nx_dk_skip_state_commands`, `cpu by core`, `first: vertex shader`) are in the ELF.

| File (`out/console-runs/run45-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` (**launches A and B**) | 46,921,925 | `feab5cd7350e923ea9ea9681416946631dc4ea1ed3f77cfa1a1c35d9adbab59d` |
| `theft4-dk.nro` (release library, optional launch C, times only) | 46,889,157 | `09e2b5225adade3f6b880140c9b40799f29f493a0feb1726e29f4f83a33fb54a` |
| `game-stage3.toml` | – | byte-equal to run 44's |
| `game-stage3-nostateskip.toml` | – | the same + `nx_dk_skip_state_commands = false` |

The first build of this session (SHA `42c2b3ac…`/`bb1630df…`, with the inert hook change) was overwritten and is not for a run (`build.log`; `build2.log` is the build of the files above).

### Q5. Run 45: what to do, what confirms, what refutes

- **Launch A** (the measure): `theft4-dk-debug.nro` + `game-stage3.toml` (byte-equal to run 44's) as `game.toml`; same `deko3d/` files; empty `captures/`; SHA-256 of the NRO on the card
  compared first; ~4 minutes after the mission screen, `+` held 1 s. Copy back `game.log`, `game-crash.log`, `game-exit.log`, `captures/` to `out/console-runs/45-logs/A/`.
- **Launch B** (optional, the off switch, one line): the same NRO with `game-stage3-nostateskip.toml` (= A + `nx_dk_skip_state_commands = false`) → `45-logs/B/`. B against A
  isolates change 1; A against run 44 measures the whole package (changes 2-4 included).
- **It ran:** the start-up line `the four state commands are not sent by the hooks`; `threads in the last ...` shows **~4-5x fewer calls** (≈ 30-60k per 10 s in play instead of 150-260k); `cpu by core` lines; `slow work` lines with shader hashes.
- **Confirms (STIMA):** play above run 44's 1.1-2.0 fps at the same scene with GPU still < 50 % busy; front end + queue per 10 s lower.
- **Refutes:** same fps and the sampled CPU ≈ wall inside the calls: then the front end's own CPU work per draw is the cost and the next step is the front end's draw path (profile);
  same fps and CPU << wall: the time is waiting (allocator lock, preemption): the next step is R2 (allocation-free packet ring) and core placement (read `cpu by core`).
- **Fails:** anything visibly different from run 44 in the same scene (missing or wrong state would show at once), a rejected-command count that rises, a crash.

## 2026-11-02: run 45 read (R): the skip works, play got slower and both hot threads wait off a CPU; run 46 = the packet path without allocations

*Campione unico: nessuna differenza è dimostrata.* Run 45 has launch A only (B and C not run). The logs are in `out/console-runs/45-logs/` (not `A/`):
`game.log` 1,777,491 B, SHA-256 `26B33D60…684F6CC`, 274 s, 17 PNGs, frames to 4197. Start-up: `17f3a18-dirty`, debug library, `game.toml` = run 44's,
clocks read = wanted all along (CPU 1020, GPU 460.8, memory 1331.2 MHz), 0 faults, `libdeko3dd errors 0 warnings 0`, exit in 9 steps. The scene is the same as run 44's
(Roman's taxi at the docks, the same camera), so the play windows compare.

### R1. What the run says (VERIFICATO unless marked)

- **The skip ran.** Start-up line `the four state commands are not sent by the hook`. Front-end totals at the end: run 44 3,334,820 commands for 877,436 draws
  (3.80 per draw), run 45 1,165,490 for 543,817 (**2.14 per draw, −44 %**). Fewer than the 80 % the roadmap estimated: the other commands (constants, shaders,
  streams, the draws themselves) remain.
- **Pictures: nothing changed, as expected (no rendering change in run 45).** Same states as run 44 in the same places: dark state close to right (frame 4194, luma 30,
  10.5 % dark), bright states overexposed blue with the wedges near the crane (4182, 4189, 4192, 4197: luma 104-195, up to 24 % saturated), shattered geometry,
  radar half black. Menu/loading frames 306-3743 equal run 44's (luma, dark share, B/R within 1).
- **Play got slower: 0.2-0.7 fps against 1.1-2.0** (windows 170-245 s against run 44's 150-260 s), at the same 2,400-4,100 draws per frame:

| play, per 10 s | run 44 | run 45 |
|---|---|---|
| frames/s | 0.6-2.0 | 0.2-0.7 |
| packets to the recorder | 17-60 k | 5.6-20.6 k |
| title: front end per packet | 91-200 µs | 276-1,262 µs |
| title: queue push per packet | 40-54 µs | 148-474 µs |
| title: waiting at a Present | 0 | 0.13-0.97 s |
| sampled calls on a CPU (new) | – | **35-45 % of their wall** |
| recorder: waiting for a packet | 2.7-4.1 s | 0.4-1.6 s |
| recorder: recording | 4.8-6.1 s | 6.5-7.3 s |
| recorder: "the rest" per draw | 36-160 µs | **193-764 µs, rising window after window** |
| recorder: textures + vertices + indices + constants per draw | 30-38 µs | 30-36 µs (unchanged) |
| recorder CPU (heartbeat) | 31-37 % | 5-28 % |
| process CPU (guest + host + ui, of 300 %) | 150-235 % | 92-200 %, ~105-135 % late in play |
| queue depth max | 150-3,500 | 6,300-6,700 |
| GPU busy | 0.9-2.7 s | 0.26-0.74 s |
| slot-fence waits | 0 | 0 |

- **Reading:** the recorder is now the slowest stage (the queue stays full, the title waits in its push and at Presents), but it is not slow on a CPU: it records
  6.5-7.3 s per 10 s while using 5-28 % of a core, and the part of its time per draw that grew is the unmeasured rest, not the copies. The title's thread is the
  same: its sampled calls are on a CPU 35-45 % of their wall. With the process at ~105-135 % of 300 %, both threads are **blocked**, not starved of cores.
- **Where (DA VERIFICARE, the best-supported hypothesis):** the only things the two threads share on that path are the queue lock and newlib's malloc lock. Every packet
  cost a deque node (1,840 B: one malloc on the title's thread, one free on the recorder's) and up to four constant vectors (run 45 made them two `reserve` calls per
  bank), and the recorder's own `Draw` allocates its stream list and fix lists per draw. Run 45 added the batch pop: up to 64 node frees **under the queue lock**,
  which the title's push takes for each packet. Push cost ×3-9 and the rest ×3-5 fit lock convoys on those two locks; the rise window after window fits a heap that
  fragments (`heap arena in use` grew 1,682 → 1,739 MiB in play). Not measured: which lock. Not excluded: another process taking CPU time on the cores (the process
  cannot see it).
- **Is the skip the cause?** Not plausibly: it removes calls at the hook before any lock or allocation. The batch pop and the wake batching changed the same path in the
  same build; launch B (skip off) would not separate them. **Launch B is not needed:** the skip's effect is already measured (−44 % commands per draw), and B cannot
  answer the open question. The next build changes the shared path instead (R2 below), and is compared with launch A in the same scene.
- **`cpu by core` is unusable:** all zeros in every window. `svcGetInfo(ThreadTickCount, thread, core)` answers only for the calling thread on its current core (STIMA from
  the kernel's documented behaviour); the line stays but says nothing. The per-thread `cpu` line is the measure.
- **Cut-scene stall: the shaders are named now** (the new `slow work` field). Every slow work (1.5-3.3 s) has the same one-draw passes on `targets 40076EB0 400007C0 1280x720`:
  vertex shader `0872615BD9E7930D` + pixel shader `2673E2AF61956ECD` **760-910 ms**; vertex shader `22F0AB2B742F2053` without a pixel shader (depth only) 140-700 ms;
  `636FBE61D31C549A` + `83DDF2237C3A51B6`/`E500863CDDB98A9F` 70-200 ms; `D6F569E53F3F319C` + `97AC30BA76D5515F` 70-143 ms. At 140-165 s two of each fell in one work: 3.0-3.25 s and
  9 warnings `the GPU has not finished slot N after 2 s` (run 44: one per work, no warning). A depth-only draw of 700 ms points at the vertex side (vertex or index
  count, or a loop in the vertex shader), not at fill: input for G4 (look these four hashes up in the package; the draw's counts are not logged yet).
- **Exit:** step 2 → 3 took 10.9 s (run 44: 4.3 s): the recorder drained a full queue of ~6,500 packets at ~1 ms each before the file gate (STIMA from the numbers).

### R2. Run 46: one change, the packets' way from the title to the recorder without an allocation per packet

Chosen because run 45 says the cost is waiting on that path, and R1 of the roadmap removes the waits that path can cause, on both threads at once:

1. **A ring of slots made once** (`native_packet_ring.h`, new) instead of `std::deque` (`dk_graphics_system.cpp/.h`): 4,096 slots of 2,512 B (10 MB). The producer (the
   title's thread, serialised by the front-end lock as before) moves a packet into a free slot and publishes the tail (atomic); the recorder reads up to 64 slots **in
   place** and gives them back (atomic head). No lock on that path; the mutex is taken only to sleep and to wake a sleeper (the same rule as run 45: an event or 16
   waiting packets wake the recorder; otherwise its 2 ms timeout). A full ring makes the title wait in its push (counted: `ring of 4096, full N times since start` on the
   `threads in the last` line). The recorder drops what a slot shares (DrawPrimitiveUp vertices, the declaration) after recording it, as the deque's destruction did.
2. **Constant deltas inside the packet** (`native_inline_vector.h`, new; `ConstantDelta` in `native_recorder.h`): up to 16 registers in 8 runs per bank travel in the packet;
   more (the first draw of a device, bone palettes) spill to the heap as before. Run 45 averaged 3.2 registers per draw. `DiffRegisters` no longer reserves.
3. **The recorder's per-draw lists reused:** the stream list of `Draw` is one thread-local vector emptied per draw, its fix lists are inline; `ApplyVertexFixes` takes a span.

Not changed: the front end's work, the recorder's work, the Present back-pressure (2 frames), the skip, the measures. No cvar: the change is in the packet type; the
comparison is run 46 A against run 45 A (same NRO settings, same scene).

### R3. Offline tests (VERIFICATO)

- `python tests/deko3d/test_packet_ring.py` (new; `dk_run46_ring_probe.cpp`, host clang): **OK**. Inline vector against `std::vector` over 2,000 rounds (spills, copy,
  move, clear): 0 errors. 2,000,000 `RecorderDraw` packets with real `DiffRegisters` deltas (1-12 registers, an 80-register palette every 97, nothing every 97) and a
  Present every 1,000 through a ring of 64 between two threads: order kept, constants checksum equal, the ring filled (120-200 waits per run). **Allocations per packet in
  the steady state, counted by a replaced `operator new` on each thread: producer 0, consumer 0.** A stopped ring refuses a waiting push. Packet 2,512 B (was 1,840).
- All other tests re-run, **OK**: `test_constant_diff` (now with the inline vectors: 8,000 draws, 0 differences, 303,782 registers, 24,755 runs, as before),
  `test_depth_alias`, `test_depth_clamp`, `test_depth_handoff`, `test_dksh_scratch`, `test_resolve_clear` (runs `ApplyVertexFixes` through the span), `test_runtime_package`,
  `test_view_swizzle`.
- **Not tested offline:** the Switch's malloc and scheduler, i.e. whether the waits were these locks (that is the run).

### R4. Build (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run46-build dk-debug`, `-j3`, one build (none running before), exit 0, **0 warnings**; rebuilt `native_front_end`,
`dk_graphics_system`, `dk_recorder`, `main`; the new report text and `PacketRing` symbols are in the ELF. Tree `17f3a18-dirty`. Only the debug-library variant (the one
run 45 A used); no release build.

| File (`out/console-runs/run46-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 46,921,925 | `1dcc7d7162239443eb6d9a5c14a0ae4646629adad812ca94b88664241940c976` |
| `game-stage3.toml` | – | byte-equal to run 45's (and 44's) |

(The NRO's size equals run 45's by page alignment; the hash differs.)

### R5. Run 46: what to do, what confirms, what refutes

- **One launch, A:** `theft4-dk-debug.nro` + `game-stage3.toml` as `game.toml`; same `deko3d/` files; empty `captures/`; SHA-256 of the NRO on the card first; the same route as
  run 45 (mission screen, the taxi), ~4 minutes of play, `+` held 1 s. Logs to `out/console-runs/46-logs/`.
- **It ran:** `queue depth max N (ring of 4096, full N times since start)` on the `threads in the last` lines.
- **Confirms (STIMA):** in play, queue push per packet back to ≤ 50 µs, the recorder's rest per draw back to ≤ 160 µs and flat across windows, sampled calls on a CPU well
  above 45 %, process CPU back near run 44's ~220 %, fps at least run 44's 1.1-2.0.
- **Refutes:** same push and rest costs with the CPU share still low: the waits are not these locks; then the next measure is a lock-wait counter on newlib's malloc lock and
  the queue lock (or a check that no other process runs: an overlay, a sysmodule, Sphaira's FTP), before any other optimisation.
- **Fails:** a hang at a full ring (the title stops, the log's `full N times` grows and frames stop), any picture different from run 45's in the same place, a crash at exit.

## 2026-11-03: run 46 read (S): the ring works; the freeze and the cut-scene stall are XenosRecomp's state-machine vertex shaders; run 47 = those shaders structured

*Campione unico: nessuna differenza è dimostrata.* Logs in `out/console-runs/46-logs/`: `game.log` 2,795,894 B, 27 PNGs, 0 faults, `libdeko3dd errors 0`, clocks as wanted.

### S1. The packet ring (R1) (VERIFICATO)

| play, per 10 s | run 44 | run 45 | run 46 |
|---|---|---|---|
| frames/s | 1.1-2.0 | 0.2-0.7 | **3.2-5.3** (outside the freeze) |
| packets to the recorder | 17-60 k | 5.6-20.6 k | **100-167 k** |
| recorder "the rest" per draw | 36-160 µs | 193-764 µs | **6.5 µs** (window 150 s: 47.9 µs in all) |
| recorder CPU | 31-37 % | 5-28 % | 28-60 % |
| process CPU (of 300 %) | 150-235 % | 92-200 % | 205-235 % |

The allocations and the queue lock were the waits of run 45 (the hypothesis of §R1 confirmed by the outcome: same scene, only R1 changed).
The ring filled 35 times by 150 s. Play is now limited by the title's thread again (front end 2.7-5.8 s per 10 s) and by the recorder (recording 7-9 s).

### S2. The freeze (VERIFICATO): the same draws as the cut-scene stall

From 175 to 240 s of play: 0.2-0.3 fps, GPU busy 9.1-14 s per 10 s, works of 3.2-4.8 s, 20 warnings `the GPU has not finished slot N after 2 s`.
Every slow work holds one-draw passes on `targets 40076EB0 400007C0 1280x720` with the same shaders as the cut-scene of runs 44-46:

- vertex shader `0872615BD9E7930D` with pixel shader `2673E2AF61956ECD` or `9A1AA9877ACD2E42`: 810-850 ms each, 2-3 per work;
- the depth-only vertex shader `22F0AB2B742F2053`: 430-700 ms, 1-2 per work.

Over the whole run (40 slow-work lines): 49 + 16 passes of `0872…`, 44 of `22F0…`, 39 + 2 of `636FBE61D31C549A`. These are the deferred light-volume vertex shaders (`gDeferredLight*`). When the scene has those lights, a frame costs 3-5 s.

### S3. Why: the state machine (VERIFICATO on the PC, same chain as the console's DKSH)

XenosRecomp writes a shader that has jumps or loops as `uint pc = 0; while (true) { switch (pc) { case 0: … } }`: each case falls into the next, and a jump is `pc = N; continue;`.

- DXC makes SSA of that: all 32 float4 registers are live across every case of the loop.
- uam then gives the vertex shader 255 GPRs and spills to local memory.
- The PC chain run on the original HLSL reproduces the shipped DKSH exactly: `0872…` code 744,960 B, 255 GPRs, 92,672 B scratch per warp. The package's median vertex shader is 10 KB.
- 54 vertex shaders declare more than 2 KB of scratch. The two worst draws above use the largest (92,672 B) and the third largest (77,312 B).
- The loop is not runaway: `i15 = int4(12, 0, 1, 0)` is a compile-time constant, so 12 rounds over `vol_offsets`.

**The same form is behind the 60 vertex shaders uam could not compile at all** (M1, §M: crashes and `-4`): all 60 are state machines. Of the 183 state machines in the package, the 60 are skinned (`gBoneMtx`): characters and vehicles. Their draws were skipped (no DKSH) or relied on a cache entry of unknown origin. DA VERIFICARE: whether the shattered vehicles and faces are these draws.

The state machine is only a written-out if/else and do-while program:

- a conditional forward jump from case k to T is an `if` over k+1 .. T-1;
- with an unconditional jump at T-1 to E, it is an if/else whose else is T .. E-1;
- the one backward jump (a conditional latch) is a do-while.

### S4. Run 47: the state machines structured (one change, in the shader package; the NRO is run 46's)

- `tools/deko3d/hlsl_structure.py` (new) rebuilds that program from the cases, copying the bodies verbatim and in order. Any other shape is left as it is: a jump out of or into a loop, an unconditional backward jump, `pc`/`continue`/`break` inside a body.
- `tools/deko3d/gta4_deko3d_structure.py` (new) copies XenosRecomp's deko3d SPIR-V folder, structures every state machine and compiles it again with XenosRecomp's DXC and its deko3d arguments (`-DGTA4_DEKO3D`, no `-fvk-invert-y`). The result is `<private>/eu-private/deko3d-spirv-s47`: **183 of 183 structured, 0 DXC failures**.
- `gta4_deko3d_shaders.py --reuse` then runs into a copy, `deko3d-out-s47`, so run 46's package stays as it was.

Per shader, the same chain (DXC, SPIRV-Cross with the fixups, banjo-nx's passes, uam; VERIFICATO):

| shader | code | GPRs | scratch per warp |
|---|---|---|---|
| VS `0872615BD9E7930D` | 744,960 → **22,016** B | 255 → **76** | 92,672 → **0** B |
| VS `22F0AB2B742F2053` | 608,256 → **21,248** B | 255 → **70** | 77,312 → **0** B |
| VS `636FBE61D31C549A` | 37,632 → **10,240** B | 212 → **27** | 0 → 0 |
| 5 of the 60 that did not compile | **compile** | 52-63 | **0** |

Whole package (`gta4_deko3d_shaders.py --reuse` into `deko3d-out-s47`, VERIFICATO):
- **12,120 of 12,120 variants compiled, 0 failed in uam.** Run 46's package had 60 failed, and 18 of them had no DKSH on the console.
- **No DKSH declares more than 2,048 B of scratch.** Before, 54 did, with a largest of 92,672 B.
- The cache shrinks from 99.1 to 92.5 MB.
- banjo-nx's passes ran on the 183 new texts (optimised 183, reused 1,729).

Package files for the card:

| file (`<private>/eu-private/deko3d-out-s47/`) | bytes | SHA-256 |
|---|---|---|
| `gta4_deko3d_runtime.bin` (validated by `gta4_deko3d_runtime_check`, 0 variants without DKSH) | 92,679,424 | `1de5dd6a19b802743df64e0cbd2f2a8b523cdbc77f859ca5ce416da0c63863ce` |
| `gta4_deko3d_glsl.bin` | 41,365,921 | `02ef7d59373746bea6e45f6b13f6ee753c0b5e748673363a4cf879619a919ea9` |

### S5. Offline tests (VERIFICATO)

- `python tests/deko3d/test_hlsl_structure.py <shader.hlsl …>` (new), **OK**:
  - synthetic machines (if, if/else, the run-46 nesting, a do-while, an if/else inside a loop): the structured program executes the same sequence of case bodies as the machine over 2,000 random condition streams each;
  - the three shapes that must be refused are left unchanged;
  - the five real shaders (`0872…`, `22F0…`, `09F5…`, `636F…`, `B8BC…`) are trace-equivalent and keep the text outside the machine.
- All other tests re-run, **OK**: `test_constant_diff`, `test_depth_alias`, `test_depth_clamp`, `test_depth_handoff`, `test_dksh_scratch`,
  `test_packet_ring`, `test_resolve_clear`, `test_runtime_package`, `test_view_swizzle`.
- `test_dksh_scratch.py --real deko3d-out-s47`: 6,295 blobs parsed, 0 over 2,048 B, maximum 0.
- **Not tested offline:** what the GPU makes of the new code (time per draw, picture). That is run 47.
- reNut / renut-nx: the user reports permission from its author (Chansey) to copy and reuse its code. Nothing in renut-nx addresses this shader form:
  its worst full-screen pass was 1.94 ms, and its GLSL passes and uam patches are already in this chain. Its blend, sRGB and viewport fixes are the
  candidates for G3 (roadmap), with the source cited.

### S6. Run 47: what to do, what confirms, what refutes

- **One launch with run 46's NRO.** Replace only `sdmc:/switch/theft4/deko3d/gta4_deko3d_runtime.bin` and `gta4_deko3d_glsl.bin` with the files of `deko3d-out-s47` (SHA-256 below). Run 46's stay in `deko3d-out`. Follow the same route as run 46: the cut-scene, then ~4 minutes of play past the place of the freeze. Logs to `out/console-runs/47-logs/`.
- **Confirms:**
  - no `slow work` line and no `has not finished slot`;
  - the cut-scene at the speed of the rest of the game and no freeze in play;
  - `shaders: … 0 compiling` and no skip reason 14.
  - DA VERIFICARE: whole vehicles and faces.
- **Refutes:** the same 400-900 ms single draws with the new shaders. Then the time is in the draw's vertex count, which the next run must log.
- **Fails:** wrong lights, or other new wrong pictures.
  - The structured program is trace-equivalent, but DXC and uam compile it differently, so a uam miscompile would show here.
  - Rollback: put back run 46's two files.

## 2026-11-03: run 47 read (T): the structured shaders removed the freeze and the stall and changed no picture, as they could not; run 48 = four renderer corrections, the crash's missing function, the deko3d files beside the NRO

*Campione unico: nessuna differenza è dimostrata.* Logs in `out/console-runs/47-logs/` (left as they are): `game.log` 26,612 lines, 46 PNGs, `game-crash.log`.
Read by six read-only investigators and three adversarial reviewers (workflow `wf_3b6d60a9-481`; their reports and scripts are in this session's scratchpad: `wf/`,
`logcmp/`, `pics/`, `crash-0x82596BC0/`, `blend/`). Run 47 = run 46's NRO + the package `deko3d-out-s47` (S4); the log confirms the package (`92679424 bytes`, `0 without DKSH`).

### T1. What run 47 measured (VERIFICATO from the log unless marked)

| | run 46 | run 47 |
|---|---|---|
| `slow work` / `has not finished slot` | 40 / 25 | **0 / 0** |
| longest GPU work | 1.6-4.8 s | **≤ 133 ms** |
| cut-scene | 0.6-0.7 fps (then skipped by a button at ~110 s) | **11-16 fps**, GPU-bound (busy 9.4-10.0 s of 10); played to the end (frames ~3707-6907), no button until 337 s |
| play | 3.2-5.3 fps, freeze 175-240 s at 0.2-0.3 fps | **5.5-7.3 fps** at 2,000-3,500 draws/frame, no freeze; draws/s 14.3-20.5 k against 9.8-16.6 k |
| draws skipped | reason 13: 2,109 | **none** |
| light pass `40076EB0 400007C0`, per draw | 37.6 ms | 0.23-1.5 ms |
| end | `+` at 363 s, frame ~4972 | **crash at 481 s, frame 8098** (T3) |

Play is title-bound again (render thread 74-77 % of a core, Present wait 0, recorder 41-65 %, GPU 5.6-7.4 s of 10).

**Why the picture did not change:** the 183 structured shaders are trace-equivalent to the machines (S5): every draw of run 46 gives the same result, faster. The only draws
run 47 adds are run 46's 2,109 reason-13 skips, **0.09 %** of 2.37 M draws. So the wedges, the bright state and the torn characters are **not** these shaders (inference from the counts; run 47 was
built to remove the stall, S6, and did).

### T2. The pictures (VERIFICATO by eye on 24 run-47 frames and 6 earlier ones; numbers on all, `pics/pngstats.py`)

- **Vehicles are whole** in run 47 (the taxi's rear at the same place and camera: 46-4345 torn, 47-7231 intact; 7639-8062 whole). Characters and faces are **not** better (4311, 4802, 5604, 6512: heads without torsos, torn skin, shards).
- **Bright state** unchanged in share (run 46: 4 full + 1 half of 16 play frames; run 47: 2 + 1 split + 1 medium of 14). **New clue, 47-7567:** a straight full-width edge at **y = 360 exactly** (97 % of columns jump there), geometry continuous across it, bright above and dark below. No other frame of runs 44-47 has it (DA VERIFICARE: a full-screen pass in two 1280x360 halves, or a viewport/scissor of height 360).
- **Light volumes:** hard-edged wedges and fans (7231, 7297, 7639, 7998, 8062), black slabs (7939).
- **2D:** glyphs missing or black (7360, 7639, 7825), button icons missing (7998), the logo's "IV" with one diagonal half missing (6918), radar quadrant black in some frames.

### T3. The crash (VERIFICATO; the caller is an inference)

`game.log:26609`: `[FATAL] Call to invalid or unregistered function at guest address 0x82596BC0` on the main thread (`c72`), then `abort(0)`. **Not memory:** 0 faults, 1,329 MiB of heap headroom; `process 3185 of 3189 MiB` is the fixed process size (run 46, which exited normally, shows the same line).
- `0x82596BC0` is in a gap: `config-eu-base/gta4_eu_base_gaps_bulk.toml:267` declares `0x82596BB0` for 32 bytes, the generator stopped at `sub_82596BB0`'s tail `b` after 16, and the second 16-byte function was never emitted (`gta4_register.cpp` registers BB0 then BD0).
- It is a script command handler: `sub_82596BD0` passes r4 = `0x82596BC0`, r3 = `0x82037290` (its name) to RAGE's register-command routine `sub_827D6C40` at `0x82597434` (`gta4_recomp.38.cpp:65059-65069`). Its body: `lis r11,-32167; mr r4,r3; addi r3,r11,19312; b 0x82564B00` (the generic script-call adapter; both targets registered). Inference: a mission script called the command for the first time; no earlier run got that far.
- A regeneration with the entry changes exactly these 16 lines (rexglue 58 s), **but** re-running `direct_calls.py` today changes all 80 files (105,134 direct calls against 116,786 in the tree): a full regeneration is not safe now (the census drift of Q1).
- **Same defect elsewhere (scan, not exhaustive):** of 2,725 command registrations through `sub_827D6C40`, 7 handlers are unregistered, all in gaps: `82548078`, `82548098`, `82560930`, `82572EB0`, `82588EF0`, `82590190`, `82596BC0`. Declaring all 7 moves code across files 36-40 (931 lines) and turns two registered starts into labels: left for a separate change; the next one of them would crash the same way, now with its caller (T5).

### T4. The audio crackle (VERIFICATO as numbers; the cause of the cost is not known)

The `audio:` line (delivered frames against the 187.5/s of 48 kHz in 256-sample frames): menus 100 %, 0 dry-outs; **play 34-49 %, 160-460 dry-outs per 10 s**, waiting for a free buffer 0-1 ms, frames dropped 0. The output never waits: **the guest's mixer is late** (runs 44-46 the same, 9-78 %). The mixer (guest id 16, entry `82169400`, priority 0x2B) uses 74-85 % of a core for 34-49 % of real time: **6.7-11 ms of CPU per 5.33 ms packet**.
Three thread samples are in its own DSP code (`sub_8216D7E0`'s voice loop, `sub_828C2D28`), none in a spin or a yield: the priority-inversion idea (UnleashedRecomp-NX's spin-lock sleep) is not supported, and scheduling changes are capped at ~60-75 % by run 46's quiet window (the mixer alone on its core, 60 %). The export tracing (`tracing on`, ~85,700 IRQL/spin-lock export calls per second on that path) is a cost on the mixer's path not yet measured. **No audio code changed in run 48**; the run measures it: `nx_profile = true` (where the mixer's time goes, per function) and `nx_trace_exports = false`.

### T5. Run 48: the changes (uncommitted, tree `17f3a18-dirty`)

| # | Change | Files | Evidence | Off switch |
|---|---|---|---|---|
| 1 | **deko3d 0.5.0's alpha destination factor written again** after each blend bind (2 words, method `0x786 + 8·target`) | `deko3d/dk_blend_alpha.h` (new), `dk_recorder.cpp` | VERIFICATO in the disassembly of **both** `libdeko3d.a` and `libdeko3dd.a`: `dkCmdBufBindBlendStates` reads the DkBlendState at bits 0, 3, 9, 15, 18 and writes `dstColor` as the sixth register; `dstAlphaBlendFactor` (bits 24-29) is never read. Every blend whose alpha destination differs from the colour one wrote a wrong alpha. Same fix as banjo-nx's plume back end (`thirdparty/plume-deko3d/plume_deko3d.cpp` `blendWords`, MIT, ChanseyIsTheBest; found by SwitchWakerHD and Tenkaichi-3-NX). How many GTA IV draws: the new counter | `nx_dk_blend_alpha_fix = false` |
| 2 | **Image descriptors 8,192 → 32,768**, and below 2,048 free ones unused textures are released (as for the byte limit) | `dk_recorder.cpp` | run 47 `game.log:22597` `image descriptor table full` at ~415 s with 195 MB of textures (under the 256 MB limit, so none released); after it every new view got descriptor 0 (black) | – |
| 3 | **A colour fetch found only by the address of a resolved depth reads guest memory** (not the depth viewed RRRR) | `dk_recorder.cpp` (`PrepareTexture`) | run 47: 63,886 of 63,887 by-address lookups were the wrong type, all one handle, `40079DA0` (32x32 A8R8G8B8 at the depth's base `1F1B9000`), ~15.6 per deferred frame; on the PC the title fills it from the CPU (`out/pc-runs/m0-lra-720/pc.log:180` `unlock handle=40079DA0 access=1 range=0+4096`) and LRA looks textures up by handle only. The registered alias (`DA1A6420`) is untouched. **Conflicts with K8's reading** (a second packed alias): only the console decides; the visual effect is a HYPOTHESIS (a reviewer: "at most a partial change to shadowed lights") | `nx_dk_mixup_from_memory = false` |
| 4 | **`sub_82596BC0` registered** (by hand, the body rexglue emits) | `config-eu-base/gta4_eu_base_gaps.toml` (entry), `generated/eu-base/gta4_recomp.38.cpp`, `gta4_init.cpp`, `gta4_register.cpp` (declaration local to the three, so only they rebuild; the next regeneration moves it to `gta4_init.h`) | T3 | – |
| 5 | The FATAL names the caller: `(guest lr 0x…)` | `src/system/function_dispatcher.cpp` | run 47 could not | – |
| 6 | Read-only: line `deko3d: run 48 (totals)` (blends bound, with a distinct alpha factor, fetches read from memory, 3D texture fetches, descriptors handed out/free, textures released for descriptors); the first 32 vertex-shader inputs without an element with the declaration's elements (run 47: 21,892, rising with the torn characters; candidate for them, DA VERIFICARE) | `dk_recorder.cpp/.h` | – | – |
| 7 | **The build puts the deko3d package beside the NRO**: `<build>/deko3d/gta4_deko3d_runtime.bin`, `gta4_deko3d_glsl.bin`, `PACKAGE` (the source folder); `THEFT4_DK_PACKAGE` chooses (default `deko3d-out-s47`) | `tools/switch-game/build_variants.sh` | the user's request | – |

Not changed, with the reason: the resolve exponent and the colour-output step of LRA (T6 2), the 6 other missing handlers (T3), audio code (T4), the y = 360 seam (no code lead yet).

### T6. Considered and not done in run 48 (from the investigators; DA VERIFICARE)

1. **Shattered characters:** not the skipped shaders (T1). Candidates: the inputs without an element (now logged), bone-palette constants (the spill path past the 16 inline registers), blend-index/weight formats, the skinned-submodel hook whose EU address differs between the two hook copies (Q1).
2. **Bright state:** LRA multiplies colour resolves with a non-zero exponent by 2^exp and scrubs NaN/±Inf in a conversion pass, and adds a colour-output step to every pixel shader (`native_color_output.h`); we copy raw (10,114 resolves with exponent 1, `not applied`). A reviewer refuted it as the cause of the bright state (a 1x1 exposure value cannot make 47-7567's seam); it stays a real gap, second in line.
3. **renut-nx.** `C:/dev/refs/unleashed-chansey` (UnleashedRecomp-NX, 1f4803d) draws through plume on **Vulkan/NVK**, not deko3d: none of its renderer drops into the recorder. Its deko3d knowledge reached us through banjo-nx's plume back end, already in `thirdparty/plume-deko3d/` (MIT): change 1 is from there. Licences: UnleashedRecomp-NX GPL-3.0 (ours is GPL-3.0: compatible, keep notices); the reNut core carried by sa2-nx has no licence file and its author is masterspike52: **Chansey's permission covers Chansey's code; core reNut files need masterspike52's or Chansey's confirmation** (DA VERIFICARE before copying any). Candidates for later: colour targets with `DkImageFlags_HwCompression` (bandwidth; the cut-scene is GPU-bound), the shader-audit tools (`tools/switch-shader-audit/`, GPL-3.0) to bisect banjo-nx's GLSL passes (sa2's `alpha` pass gave flat colours; ours runs it), sa2-nx's audio pump (BSD-3).
4. Display gamma ramp at present (the title calls `VdGetCurrentDisplayGamma`): global brightness only.

### T7. Offline tests (VERIFICATO)

- `python tests/deko3d/test_blend_alpha.py` (new; `dk_run48_blend_probe.cpp`): **OK**. The disassembly check reads fields at bits `[0, 3, 9, 15, 18]` in `libdeko3d.a` and `libdeko3dd.a`; over 8 targets × 19 × 19 destination factors × 4 variations (11,552 cases) the library's registers followed by the two words equal plume's six registers in every case; the library alone differs in 10,944, exactly the cases with two different destination factors.
- All other tests re-run, **OK**: `test_constant_diff`, `test_depth_alias`, `test_depth_clamp`, `test_depth_handoff`, `test_dksh_scratch`, `test_packet_ring`, `test_resolve_clear`, `test_runtime_package`, `test_view_swizzle`.
- **Not tested offline:** changes 2-4 on the console (descriptor eviction, the fetch from memory, the new function: the build compiles them), what the pictures do.
### T8. Build (VERIFICATO) and run 48

`tools/switch-game/build_variants.sh out/console-runs/run48-build dk-debug`, `-j3`, one build, exit 0, **0 new warnings** (only the SDK's known `live_compatibility.h` ones). Rebuilt: `function_dispatcher`, `dk_graphics_system`, `dk_recorder`, `main`, `gta4_init`, `gta4_recomp.38`, `gta4_register`. In the ELF (`nm`, strings): `sub_82596BC0` (2 symbols), `run 48 (totals)`, `guest lr 0x`, `nx_dk_blend_alpha_fix`, `nx_dk_mixup_from_memory`, `has no element; the`.

| File (`out/console-runs/run48-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 46,926,021 | `f60fbd87337fb7a6b48e3fa89f5eb085c840334d690719cb6a0e74f074805ec2` |
| `deko3d/gta4_deko3d_runtime.bin` (= `deko3d-out-s47`, run 47's) | 92,679,424 | `1de5dd6a19b802743df64e0cbd2f2a8b523cdbc77f859ca5ce416da0c63863ce` |
| `deko3d/gta4_deko3d_glsl.bin` (= run 47's) | 41,365,921 | `02ef7d59373746bea6e45f6b13f6ee753c0b5e748673363a4cf879619a919ea9` |
| `game.toml` | – | stage 3 as runs 44-47 + `nx_profile = true`, `nx_trace_exports = false` |

**Run 48, one launch.** Copy `theft4-dk-debug.nro`, `game.toml` and `deko3d/` of the folder to `sdmc:/switch/theft4/` (empty `captures/` first; check the NRO's SHA-256). Same route as run 47: the whole intro cut-scene, then play until past 481 s (the place of the crash), `+` held 1 s. Logs to `out/console-runs/48-logs/`.
- **It ran:** the `deko3d: run 48 (totals)` lines; `blend states bound … alpha factor written again`; `read from guest memory (nx_dk_mixup_from_memory)` in the mix-up line; `profile` lines.
- **Confirms:** no FATAL at the place of run 47's (or, if a later one, a FATAL with `guest lr`: then one of T3's six); no `image descriptor table full`; `textures found by base address … wrong type` stops rising; the distinct-alpha blend count > 0 (the fix touched draws). Pictures (STIMA, weak): any change of the lit areas, the bright state or the 2D in the same places as 47-7231/7297/7639/7998/8062.
- **Refutes:** distinct-alpha count 0 (the blend fix is inert for this title) and pictures equal to run 47's: then T6 1-2 are next.
- **Fails:** new black or wrong textures (the descriptor release: a texture released while still in use would come back as a re-upload, counted), a hang, a crash at exit. Rollback: `nx_dk_blend_alpha_fix = false`, `nx_dk_mixup_from_memory = false` in `game.toml`, or run 47's files.
- **Audio:** the `audio: … (P% of real time)` line against run 47's 34-49 % with tracing off, and the `profile id 16` lines (which functions of the mixer take its time) for the next change.

## 2026-11-04: run 48 read (U): stable to 350 s, the blend fix inert, characters still in pieces, the mixer blocked on the XMA kick

*Campione unico: nessuna differenza è dimostrata.* Logs in `out/console-runs/48-logs/` (left as they are): `game.log` 29,798 lines, 37 PNGs, `game-crash.log`/`game-exit.log` (exit, not a crash).
Read by five read-only investigators and one adversarial reviewer per claim (workflow `wf_b4e5508a-f85`; reports in this session's scratchpad `inv.json`, scripts in `scratchpad/run49/`).

### U1. What run 48 measured (VERIFICATO from the log)

| | run 47 | run 48 |
|---|---|---|
| end | FATAL `0x82596BC0` at 481 s | **`+` at 350 s, exit mode D, 0 faults, 0 FATAL** (it did not reach 481 s: the crash fix is not tested) |
| cut-scene | 11-16 fps, GPU-bound | 11.6-15.8 fps, GPU busy 9.7-9.95 s of 10 (skipped with a button at ~142 s) |
| play | 5.5-7.3 fps, 14-20.5 k draws/s | **4.1-7.6 fps at 1,965-6,172 draws/frame = 15-27 k draws/s** (more draws per frame in the street; throughput did not regress) |
| recorder in late play | – | the limit: idle 0-0.5 s of 10, ring of 4,096 full 958 → 2,708 times, 31-46 µs per drawn draw, of which **vertices 15.6-26.3 µs**, indices 3.5-6.2, scan 1.9, textures 2.1-4.7, constants 2.2-2.8; 3.0-3.3 GB uploaded per 10 s |
| `image descriptor table full` | at ~415 s | **none** (11,234 descriptors at 330 s of 32,768) |
| textures | – | live 3,787 (69 MB) → 11,185 (215 MB), none released (under the 256 MB limit: a cache, not a leak; heap headroom 1,462 → 1,330 MiB, +0.7 MiB/s) |
| blend states with a distinct alpha factor | – | **0 of 171,160**: the deko3d 0.5.0 fix is inert for this title |
| colour fetches at a resolved depth read from memory | – | 54,444 (the run-48 change ran; no visible change found) |
| inputs without an element | 21,892 | 16,032 by 330 s, **only two shaders** (`AD3E412AF420299A`, `5E7C32C4EF6771C0`, skinned shadow shaders) drawn with a declaration without BLENDWEIGHT/BLENDINDICES |
| audio in play | 34-49 % | **38-45 %**, 250-415 dry-outs per 10 s (export tracing off: +1 point, noise) |

### U2. The pictures (VERIFICATO by eye on 12 run-48 frames, compared with run 47)

- Unchanged from run 47: bright state, light-volume wedges, missing/black glyphs and radar tiles.
- **Characters and fragment objects:** whole pieces placed wrongly, not random spikes and not triangles pulled to the origin. 48-6027: Niko's head, hands and shoes where they belong, the torso missing; 48-4955 and 47-7231: a crane drawn as planar strips fanned around one pivot; 48-6042: an open car door as its frame only. Buildings in the same frames are whole.

### U3. Where the time of the audio mixer goes (VERIFICATO: 27,063 profile samples of thread id 16 in play, symbolized with the run-48 ELF)

The profiler samples wall time (it pauses the thread and reads its context, `tools/switch-game/nx_profiler.cpp:152-179`), so blocked time is in it.

| share of the mixer's wall time | where |
|---|---|
| **19.4 %** (21-24 % in the 38-40 % windows) | blocked in `XmaDecoder::WriteRegister` (`xma_decoder.cpp:354`, `WaitForWorkDone`): every context kick waits for the XMA worker's whole decode |
| 3.9 % | `sched_yield` (svcSleepThread) in `xeKeKfAcquireSpinLock` from `sub_8216D7E0` |
| 3.1 % | `KeWaitForSingleObject` at the top of the mixer loop |
| ~72 % | the guest's own DSP code (`sub_8216DBB0` 7.4 %, `sub_828C6740` 4.2 %, `sub_82188C28` 1.8 %, 58 % not attributable from the top-14 lists) |

The XMA wait equals the XMA Decoder thread's own CPU (12-24 %) to 1-2 points: it is decode time, not a scheduling delay, so `THEFT4_XMA_INLINE=1` would only move it onto the mixer (0-2 %). The generated code is already `-O3 -mcpu=cortex-a57`: no compiler-flag gain. **Inference:** with zero waits the mixer still needs ~72 % of a core for 38-45 % of real time, so at 1020 MHz continuous audio needs the DSP itself cheaper (or a faster clock: run 29 at 1785 MHz measured 60-75 %).

### U4. The rendering leads, checked and refuted (VERIFICATO unless marked)

- **Bone palette transport** (c64-c255 from device+0x780, the front end's diff, the ring's spill past 16 registers, `PushConstants`, the GLSL binding): checked end to end, **no defect**; the PC path reads the same bank. The cached command-list replay (`ScopedReplayDrawState`) writes its build-time constants into the device before the draw is captured, so the front end sees them.
- **Hooks:** the skinned-submodel hook `sub_82855890` is the right EU function (83 of 83 instructions) and changes nothing on the GPU. No hook delivers bones. (The EU translator passes addresses missing from the old census unchanged: a regeneration hazard, not this bug.)
- **Vertex formats:** `VertexAttribute` equals the PC's `GetCompatibleVertexFormat` for every type the title uses; all 636 vertex shaders have float inputs, at most 10, uam locations ≤ 9; the skinned shaders' weights and indices are both D3DCOLOR and paired by the same swizzle. The `g_Swapped*` masks are 0 on the PC too.
- **The internal passes** (resolve, hand-off, alias, output) all reset the emitted state (`ForgetState`): no binding leaks into the next draw.
- **Found, real, but not shown to be the cause** (fixed in run 49, V):
  1. vertex and index bytes are read from live guest memory on the recorder's thread up to a full ring (1.3-1.5 frames) after submit; the PC copies at submit (`CaptureBufferResource`) and keeps a sampled check "because guest bytes can change without an unlock". The completed-frame word is only signalled after the recorder's Present, so a title that waits on it is safe; one that rewrites a buffer without that wait is not. Not measured (no counter existed): run 49 measures it.
  2. 32-bit indices not masked to 24 bits; restart applied to every primitive, and for 16-bit draws with the reset value cut to 0xFFFF (the PC: strips/fans only, 16-bit only when the value fits).
  3. inputs without an element bound as deko3d fixed attributes (value not documented) where the PC binds a zero record.

## 2026-11-04: run 49 (V): the draw's bytes as submitted, the Vulkan path's index rules, a zero record, the skinning measured, the XMA kick asynchronous

### V1. The changes (uncommitted, tree `17f3a18-dirty`)

| # | Change | Files | Evidence | Off switch |
|---|---|---|---|---|
| 1 | **The bytes a draw is recorded with are the bytes the title submitted.** The front end copies, at submit, the draw's vertex range and indices for buffers the title rewrites: still locked at the draw, written again after their first fill (a second guest-write unlock), or caught changing by the recorder. 1 draw in 32 of the others is hashed at submit and hashed again when recorded; a difference is counted, logged (first 16) and its buffers are copied at submit from then on. | `native_draw_snapshot.h` (new), `native_front_end.cpp/.h` (`CaptureDrawData`, `PromoteDynamicBuffer`), `native_recorder.h` (`RecorderDraw::data`, `submit_hash`), `dk_recorder.cpp`, `dk_graphics_system.cpp/.h` | U4 1; the PC reference does the same at submit (graphics_system.cpp:5186-5360) | – (inert when nothing changes: only counts) |
| 2 | **Xenos's index rules, as the Vulkan path:** 24-bit indices, restart only on line strips/triangle strips/fans and for 16-bit indices only when the reset value fits, the reset value turned into all ones; the range scan and the conversion are one pass (run 48: two) | `native_index_policy.h` (new), `dk_recorder.cpp` | U4 2; graphics_system.cpp:2692-2704, 24226-24233, 24480-24490 | – |
| 3 | **Inputs without an element read a zero record** (16 bytes, per instance), as the Vulkan path's default binding | `dk_recorder.cpp` | U4 3; graphics_system.cpp:22106-22121, 22195-22205 | – |
| 4 | **Vertex streams without a fix-up are swapped straight into the upload ring** (one write-only pass instead of scratch + copy; same bytes) | `dk_recorder.cpp` | U1: vertices are half of the recorder's time per draw and the recorder is the limit in late play | – |
| 5 | Read-only: **the skinning inputs of 1 skinned draw in 64** (weight sum of the 4 unorm bytes, bone index < 64), first 24 combinations logged with types and bytes; line `deko3d: run 49 (totals)`, line `front end, run 49 (totals)` | `dk_recorder.cpp/.h` | U2: says whether the inputs or the bones are wrong | – |
| 6 | **The XMA kick returns at once** (cvar `xma_async_kick`, default on); a context the title locks before the worker reached it is decoded on the locking thread first, so no kick is lost | `src/audio/xma_decoder.cpp` | U3: 19-24 % of the mixer blocked there; the hardware decodes asynchronously (sa2-nx/nfsmw-nx `audio_xma_in_worker`, BSD-3, the idea only, no code copied) | `xma_async_kick = false` |
| 7 | **Guest spin lock:** test and test-and-set, 64 spins on the CPU's `yield` hint before `sched_yield` | `src/system/guest_spinlock.cpp` | U3: 3.9 % of the mixer in `sched_yield` on that lock | – |

Not done, with the reason: the 6 other unregistered script handlers (T3: no FATAL in run 48 to point at one), a persistent vertex buffer cache (the next performance step: 3 GB uploaded per 10 s; needs change 1's dynamic-buffer knowledge first), the bright state/wedges (no new lead), the DSP cost of the mixer (guest code at -O3; next: per-function rollup of the profile).

### V2. Why these (impact against risk)

- Characters in pieces (U2) have no proven cause: every path checked in U4 matches the PC. Changes 1-3 remove the three verified differences from the PC that can move or drop geometry; change 5 and the counters of 1 decide between "bytes changed under the draw", "bad skinning inputs" and "neither" (then: the bones, i.e. the constants at the draw, need a per-draw palette hash next).
- Audio: change 6 removes the one wait that the profile names (~20 % of the mixer's wall time) without moving the decode onto the mixer.
- Performance: change 4 and the one-pass index conversion cut the recorder's per-draw work, which bounds late play; change 1 adds work on the title thread only for the draws it copies (counted) and 1/32 hashes.

### V3. Offline tests (VERIFICATO)

- `python tests/deko3d/test_run49.py` (new; `dk_run49_probe.cpp`): **OK.**
  - Index policy: 20,000 random draws (16/32-bit, 8 primitive types, 4 reset values, top bytes): **0 differences** from a reference written from the Vulkan path's rules; the front end's scan gives the recorder's range in all; restart on in 2,804 draws, 184,051 indices masked; **run 48's rules gave another vertex range in 10,435 of the 20,000 random draws** (random data, not the title's: says the rules differ, not how often in the game).
  - Snapshot: 10,000 random draws over fake guest buffers (1-3 streams, strides 4-48, base vertex, 16/32-bit): **0 false alarms** with nothing changed, **0 changes missed** inside the range or in the indices, **0 alarms** for a change outside the range, **0 copies** different from the submitted bytes.
- All earlier tests re-run, **OK**: `test_packet_ring` (`RecorderDraw` 2,544 B, still 0 allocations per packet in the steady state), `test_constant_diff`, `test_depth_alias`, `test_depth_clamp`, `test_depth_handoff`, `test_dksh_scratch`, `test_resolve_clear`, `test_runtime_package`, `test_view_swizzle`, `test_blend_alpha`, `test_hlsl_structure`.
- **Not tested offline:** the async XMA kick (needs the title's audio), the zero record and the direct copy on the GPU, what the pictures do.

### V4. Build (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run49-build dk-debug`, `-j3`, one build, exit 0, **0 new warnings** (log `out/console-runs/run49-build.log`). Rebuilt: `native_front_end`, `dk_graphics_system`, `dk_recorder`, `guest_spinlock`, `xma_decoder`, `main`. In the ELF: `NativeFrontEnd::CaptureDrawData`, `PromoteDynamicBuffer` (nm); strings `run 49 (totals)`, `front end, run 49`, `xma_async_kick`, `skinned draw`, `changed between submit and record`; `xeKeKfAcquireSpinLock` has the `yield` loop (objdump). The package beside the NRO passes `gta4_deko3d_runtime_check.py` (1,912 shaders, 12,120 variants, 0 without DKSH).

| File (`out/console-runs/run49-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 46,950,597 | `c121c75e4d73ddb49fa67a6cfb3fd018bacd3f13a366548cacc9594efc93cb5c` |
| `deko3d/gta4_deko3d_runtime.bin` (= `deko3d-out-s47`, runs 47-48) | 92,679,424 | `1de5dd6a19b802743df64e0cbd2f2a8b523cdbc77f859ca5ce416da0c63863ce` |
| `deko3d/gta4_deko3d_glsl.bin` (= runs 47-48) | 41,365,921 | `02ef7d59373746bea6e45f6b13f6ee753c0b5e748673363a4cf879619a919ea9` |
| `deko3d/PACKAGE` (the source folder) | 40 | `313ab911545ef5b2a524f2c3b064b97d0726123c72223ace647bdec2dd0140ab` |
| `game.toml` (= run 48's settings) | 678 | `dcee11572d6c3da9a3a7266df1b49a15466e517273ba57248e497b5c1dac405f` |
| `switch-game-dk-debug.elf` (symbols, not for the card) | 315,978,168 | `3bb40faa4326c7f565258e604dd3a6f517a9bf2b70fddbbc92ff9a06f516ad17` |

### V5. Run 49: what to do, what confirms, what refutes

**One launch.** Copy `theft4-dk-debug.nro`, `game.toml` and `deko3d/` of the folder to `sdmc:/switch/theft4/` (empty `captures/` first; check the NRO's SHA-256). Same route as run 48: the intro cut-scene to its end (no button), then play on foot and by car **past 481 s** (run 47's crash point, not reached by run 48), `+` held 1 s. Logs and `captures/` to `out/console-runs/49-logs/`.
- **It ran:** `XMA decode scheduling: dedicated worker, kicks not waited for (xma_async_kick)`; the lines `deko3d: run 49 (totals)` and `front end, run 49 (totals)`.
- **Read first:** `changed since submit` (> 0: the title rewrites buffers under waiting draws, the copies now hide nothing and fix it; 0: the bytes were never the cause); `skinned draws checked … weight sum off / bone index past the palette` (> 0 with `skinned draw … OUT`/`PAST` lines: the skinning inputs are wrong; both 0: the bones themselves, next run hashes the palette per draw); `restart … refused`; `32-bit indices cut`.
- **Confirms (STIMA):** characters and fragment objects whole or less broken where `changed since submit` or `restart refused`/`masked` are large; audio above run 48's 38-45 % (expected ~47-56 %: the ~20 % wait gone, the DSP cost unchanged, so still not continuous); play fps at least run 48's 4.1-7.6 at the same draws per frame; `vertex streams copied straight into the ring` > 0 and vertices per draw below 15.6-26.3 µs.
- **Refutes:** all run-49 counters 0 and pictures as run 48: then the three PC differences were not the cause, and the bones (the constants at the draw) are next.
- **Fails:** audio silent, stuck or looping (the async kick: rollback `xma_async_kick = false` in `game.toml`); title-thread time up a lot with `draws copied at submit` very large (the copy is too wide); new wrong geometry (the index rules); a hang or crash. Rollback for the renderer: run 48's NRO.

## 2026-11-04: run 49 read (W): 1,355 s and a crash in the save, the counters, the copies too costly, audio 53-61 %

*Campione unico.* Logs in `out/console-runs/run49-logs/` (the user's folder name; left as they are): `game.log` 54,576 lines, 131 PNGs, `game-crash.log`, `game-exit.log` (empty). The user: "no graphical or performance improvement; further into the first mission; crashed again; the audio still crackles".

### W1. The end (VERIFICATO from the log)

- **Ran 1,355 s, past run 47's 481 s**: the hand-registered `sub_82596BC0` (run 48) holds; no `[FATAL]`, 0 faults, heap headroom 1,245 MiB.
- **Crash = the save.** `game.log:54499` `file open #626: NtCreateFile 'save0:\SGTA412' disposition 5 … access 0x40100080` (the title creates its save for writing at a mission checkpoint), then `ERROR TERMINATION: std::terminate (uncaught … filesystem_error: status: I/O error [sdmc:/switch/theft4/saves/…/SGTA412/SGTA412])`, `abort(0)`.
  - Where: the title opened the save again; `VirtualFileSystem::OpenFile` checks that a cached entry still exists on the host with `std::filesystem::exists(file_path)` **without an `error_code`** (`virtual_file_system.cpp:262`).
  - Why EIO: Horizon locks a file that is open for writing; until that handle closes, a second open fails, and so does `stat()` (libnx's stat opens the file for its size). libnx reports both as EIO; `exists()` throws on any error but "not found".
  - Not the run-47 kind (an unregistered function): a host-side exception on the title's thread.
- **The other 6 unregistered script handlers (T3)** did not fire in this run; a re-scan of the tree (`crash-0x82596BC0/scan.py`): 6 of 2,725 command registrations still name a gap.

### W2. The run-49 counters (VERIFICATO; totals at 1,340 s)

| Counter | Value | Reading |
|---|---|---|
| sampled draws checked again / **changed since submit** | 112,737 / **9,970 (8.8 %)** | the title does rewrite buffers under draws still waiting in the ring: real |
| draws copied at submit | 8,388,798 of 13.9 M (**60 %**, 112 GB) | 19,786 buffers promoted, never demoted |
| skinned draws checked / weight sum off / bone index past the palette | 8,932 / **0 / 0** | the skinning inputs are right |
| 32-bit indices cut to 24 bits / restart on / refused | **0 / 0 / 0** | the index rules never applied to this title |
| inputs from the zero record | 83,316 | the two skinned shadow shaders (U1) |
| front end: rejected | **0** | no draw lost after its constants were captured |

**The pictures did not change** (W3): so the stale bytes, while real, were not what breaks the characters, and the index rules and the zero record do not matter for this title.

### W3. The pictures (VERIFICATO by eye on 8 run-49 frames)

- Streets and buildings mostly whole (`dk-f007468`), with some slabs out of place and dark wedges by the kerb.
- **Characters: stretched shards converging on points, most of a body missing** (`dk-f011543`, `dk-f013643`), faces mostly right with dark irregular patches (`dk-f010059`, `dk-f006012`); floating flat shards above the docks (`dk-f006012`). The PC (`out/pc-runs/m0-side/side-t0120.png`, same front-end design, Vulkan) draws the same characters whole.
- Shards converging on points are what vertices transformed by a zero or foreign bone matrix give (a vertex collapses on the model's origin).

### W4. Leads checked in run 50 and excluded (VERIFICATO unless marked)

- **Skinning inputs**: the logged bytes decode consistently with D3DCOLOR for both elements, e.g. weights `00 97 68 00` with indices `00 00 01 00` = 0.59 on bone 0 + 0.41 on bone 1; the GLSL (`vs_AD3E412AF420299A`) pairs weight component k with index component k (indices `.xzwy`, weights used as `.w↔.z`, `.z↔.y`, `.y↔.w`, `.x↔.x` after the swizzle), index × 765.0059 → 3 registers per bone, read as `c[64 + min(i, 191)]` zeroed past 192. `swapped_blend_weights` (`g_DkShared[34].y`) is never written (0), on the PC too.
- **The shader compiler's input slots**: a test shader with locations 0, 3, 7 compiled by uam (`-r`): the `ALD` attribute addresses are 0xB0, 0xF0, 0x80 = 0x80 + 16 × location, so uam keeps locations (TGSI's compacted `IN[]` do not reach the hardware).
- **Lost constant deltas**: `rejected 0`; the PC reads the same live device bank at the draw (`graphics_system.cpp:4797`, dirty words picking ranges of the same bank), so our full diff is a superset.
- **Remaining difference with the PC in how constants reach the GPU**: the PC gives each draw its own version of the banks (a constant arena, `NativeConstantArenaAlignment`); the deko3d recorder kept one block per work and updated it in place with `dkCmdBufPushConstants`, and in `PushConstants` the push came **before** the bind, so after an internal pass had bound its own block at vertex/fragment slot 0 the update went to a block not bound at that moment, which draws recorded earlier may still read. Not proven on the GPU (no way to observe it offline): run 50 removes the mechanism (X1 3) and measures the transport (X1 4).
- HYPOTHESIS, not acted on: the dark patches on faces look like depth fighting between two passes of one mesh; the GLSL marks only x/y/w of the position `precise`, not z, and no shader declares `invariant gl_Position`. Changing it needs the shader package rebuilt (12,120 variants): next.

### W5. Performance: run 49 was slower than run 48 (VERIFICATO)

| | run 48 | run 49 |
|---|---|---|
| play | 4.1-7.6 fps, 15-27 k draws/s | **1.5-4.8 fps at 2,800-5,100 draws/frame = 13-15 k draws/s** |
| title thread in the front end | 3.0-3.3 s of 10 (5.6 µs per call) | **6.5-6.8 s of 10 (12.5 µs per call)** |
| recorder per drawn draw | 31-46 µs (scan 1.9) | **44-54 µs (index conversion 6.4-8.2)** |
| GPU busy | – | 5.0-5.5 s of 10 |

Causes: the copies at submit (60 % of the draws, ~16 KB each, one `make_shared` plus a vector per draw on the title's thread, freed on the recorder's through newlib's one malloc lock), and run 49's one-pass index conversion, whose restart test kept the compiler from vectorizing it (the old scan alone was 1.9 µs).

### W6. Audio (VERIFICATO)

- `XMA decode scheduling: dedicated worker, kicks not waited for (xma_async_kick)`: **53-61 % of real time in play** (run 48: 38-45 %), 11-55 dry-outs per window: the asynchronous kick gave the ~15 points it was meant to, inside V's estimate (47-56 %).
- The mixer (id 16) still runs **95 % of a core** at 1,020 MHz. 107,970 profile samples after 200 s (`scratchpad/run50/mixer_profile.py`, symbolized with the run-49 ELF): 12.4 % on 128-bit guest loads (`lvx128` → `vld1q_s64`) and 5.2 % on stores in `sub_8216DBB0` (the voice loop's multiply-add over buffers), `sub_828C6740`, `sub_82188C28`; 4.4 % waiting in `KeWaitForSingleObject`. The FPCR writes are conditional (`context.h enableFlushMode`), not in the loop. **Memory latency in the guest's own DSP**: no host-side lock or wait left to remove; what is left is speed (clock) or rewriting guest code.

## 2026-11-04: run 50 (X): the save, the 6 handlers, constant versions, copies recycled, the clock

### X1. The changes (uncommitted, tree `17f3a18-dirty`)

| # | Change | Files | Evidence | Off switch |
|---|---|---|---|---|
| 1 | **The save crash.** The VFS drops a cached entry only when the host says the file is not there (`HostFileIsGone`: no error, ENOENT, ENOTDIR), never on EIO. On NX the descriptors are kept per path (`SharedHostFdTable`): a second open of a file our own handle keeps locked shares that descriptor (reads and writes are `pread`/`pwrite` at the handle's own offsets), `stat` of it is answered by `fstat`, and the descriptor closes with the last handle; a handle that only writes is opened read-write so it can serve a reader. Lines `host open of a locked file shares …`, `host stat of a locked file answered …` (first 16) | `src/core/host_file_share.h` (new), `src/core/filesystem_posix.cpp`, `src/filesystem/virtual_file_system.cpp` | W1 | – |
| 2 | **The 6 other script command handlers registered** (`82548078`, `82548098`, `82560930`, `82572EB0`, `82588EF0`, `82590190`): bodies from a scratch regeneration with the 6 entries (rexglue 52.6 s, `scratchpad/run50/handlers/`), added by hand as run 48 did for `82596BC0` (declarations local to the three files). The regeneration turns `sub_82548108` and `sub_825609A0` into labels inside the new bodies; the tree keeps them as registered functions (no direct call to either in the tree; kept so that nothing registered disappears) | `config-eu-base/gta4_eu_base_gaps.toml` (6 entries), `generated/eu-base/gta4_recomp.36/37/38.cpp`, `gta4_init.cpp`, `gta4_register.cpp` | T3, W1; re-scan: **0 of 2,725 registrations unregistered** | – |
| 3 | **Constant versions**: a draw whose vertex or pixel constants changed gets its own block, filled by the CPU and never written again, as the PC's constant arena; no inline update. Upload ring 32 → 64 MB per slot (the draw already reserved room for both blocks; up to 8 KB more per draw) | `deko3d/dk_recorder.cpp/.h`, `dk_graphics_system.cpp/.h`, `tools/switch-game/main.cpp` | W4 | `nx_dk_constant_versions = false` |
| 4 | Read-only: **the transport of the constants checked**: 1 draw in 32 (half a period from the data hash) carries the front end's banks hashed after its deltas (vertex c0-c63, the bone palette c64-c255, pixel); the recorder hashes its own banks when it records the draw and counts differences per region (first 16 logged). Line `deko3d: run 50 (totals)` | `native_constant_diff.h` (`ConstantBankHash`, `ApplyConstantDelta`, now shared by the recorder), `native_recorder.h`, `native_front_end.cpp`, `dk_recorder.cpp` | W4 | – |
| 5 | **Index conversion and scan without branches** when restart is off (every draw of this title): vectorizable | `native_index_policy.h` | W5 | – |
| 6 | **The copies at submit recycled** (`DrawDataPool`): no allocation per copied draw, at most 1,024 waiting copies of at most 64 KB kept; a recycled copy is not zeroed again. Line `front end, run 50 (totals)` | `native_draw_snapshot.h`, `native_front_end.cpp`, `dk_graphics_system.cpp` | W5 | – |
| 7 | **Clocks in `game.toml`**: CPU 1,785 MHz (Horizon's boost-mode clock) and memory 1,600 MHz (the docked clock), GPU unchanged (460.8). Code unchanged (run 29's `nx_cpu_mhz`/`nx_emc_mhz`) | `run50-build/game.toml` | W6: the mixer is CPU-bound in guest code at 95 % of a core; run 29 at these clocks | delete the two lines |

Not done, with the reason: `invariant gl_Position` (needs the package rebuilt, W4 hypothesis), a persistent vertex buffer cache (the copies' cost is now lower; next performance step), the bright state and light wedges (no new lead), rewriting the mixer's DSP (guest code).

### X2. Why these

- The crash: a defect proven to the line, fixed at the cause (the check and the locking), not by catching the exception.
- The handlers: the same defect as run 47's crash, now removed for all of them.
- The characters: every input-side lead is excluded (W4); constant versions remove the one remaining difference with the PC in how the bones reach the GPU, and change 4 says whether the CPU side is exact. If the pictures stay as they are and change 4 counts 0, the bones are right up to the GPU and the shader side is next (`invariant`/precision, then a GPU readback of one skinned draw's positions).
- Performance: undo run 49's regression (5, 6) without giving up the copies (8.8 % of the sampled draws do change).
- Audio: the code-side wait is gone (W6); the clock is the lever left that does not touch guest code.

### X3. Offline tests (VERIFICATO)

- `python tests/deko3d/test_run50.py` (new; `dk_run50_probe.cpp`): **OK.**
  - FILE: a model of Horizon's locking reproduces run 49's sequence (the old `exists()` throws), the new check keeps the entry, the second handle shares the descriptor, which is closed once with the last handle; ENOENT/ENOTDIR drop the entry; a read-only descriptor never serves a write; the table stays bounded. **0 errors.**
  - CONST: 12,800 random draws through `DiffRegisters` (front end) and `ApplyConstantDelta` (recorder): **0 false alarms**; 101 deltas dropped on purpose, **101 caught, all in the right region**; every version, read after its work was recorded, holds the bank of its draw (0 wrong). The single in-place block read the same way gives 12,449 of 12,800 draws a later bank: an illustration of what an unordered update would do, not a measurement of the GPU.
  - INDEX: 20,000 random draws, the branch-free paths against a reference: **0 errors**.
  - POOL: 20,000 copies through the pool, each equal field by field to a fresh copy: **0 errors**; 48 created, 19,952 recycled.
  - Re-run inside it: `test_run49.py` OK, `test_constant_diff.py` OK, `test_packet_ring.py` OK.
- Handlers: `crash-0x82596BC0/scan.py` on the tree: **0 unregistered of 2,725**.
- **Not tested offline:** the shared descriptors on Horizon itself, the constant versions on the GPU, what the pictures do, the clocks' effect.

### X4. Build (VERIFICATO)

`tools/switch-game/build_variants.sh out/console-runs/run50-build dk-debug`, `-j3`, one build at a time, two passes (the second after header edits made during the first: `native_front_end`, `dk_graphics_system`, `dk_recorder`, `main` rebuilt), exit 0, no new warnings (the warnings in `filesystem_posix.cpp`/`virtual_file_system.cpp` are on untouched lines). Logs `out/console-runs/run50-build-pass1.log`, `run50-build.log`. In the ELF: the six `sub_825…` handlers (nm), strings `run 50 (totals)`, `front end, run 50`, `nx_dk_constant_versions`, `host open of a locked file`, `host stat of a locked file`. The package beside the NRO passes `gta4_deko3d_runtime_check.py` (1,912 shaders, 12,120 variants, 0 without DKSH). `SHA256SUMS.txt` in the folder.

| File (`out/console-runs/run50-build/`) | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 46,975,173 | `c7646d95cf989ae29ab12e0f9847846ac457ea45f50f25055c5795cd1cd3e2dc` |
| `deko3d/gta4_deko3d_runtime.bin` (= runs 47-49) | 92,679,424 | `1de5dd6a19b802743df64e0cbd2f2a8b523cdbc77f859ca5ce416da0c63863ce` |
| `deko3d/gta4_deko3d_glsl.bin` (= runs 47-49) | 41,365,921 | `02ef7d59373746bea6e45f6b13f6ee753c0b5e748673363a4cf879619a919ea9` |
| `deko3d/PACKAGE` (`deko3d-out-s47`) | 40 | `313ab911545ef5b2a524f2c3b064b97d0726123c72223ace647bdec2dd0140ab` |
| `game.toml` (run 49 + clocks) | 1,104 | `3e0a755b771288b1c0e65fc64ac6eba206d03314d2abe19def40e08e4cd79186` |
| `switch-game-dk-debug.elf` (symbols, not for the card) | 316,402,472 | `1377e49c5b1caead23818edd104c82c34a55b80fa2b25925af7e33cc3b0c69cc` |
| `COMMIT` (`17f3a18-dirty`) | 14 | `e45ad8b5340ea682cdaea63e2e60a939d9bc34a95421a5266693a6d9feb4801b` |

### X5. Run 50: what to do, what confirms, what refutes

**One launch.** Empty `sdmc:/switch/theft4/captures/`; copy `theft4-dk-debug.nro`, `game.toml` and `deko3d/` to `sdmc:/switch/theft4/` (check the NRO's SHA-256). The intro cut-scene to its end without a button, then the first mission **past run 49's save point (1,355 s)**, letting the game save; `+` held 1 s. Logs and `captures/` to `out/console-runs/run50-logs/`.
- **It ran:** `clocks read: CPU 1785…`, the lines `deko3d: run 50 (totals)` and `front end, run 50 (totals)`.
- **Read first:** `different: … bone palette c64-c255` (> 0: the CPU transport of the bones is wrong, the logged draws say which shaders; 0 with characters still broken: the bones reach the GPU right, the shader side is next); `host open of a locked file shares …` (the save's second open, now served); the `audio:` lines; the front end's ms per 10 s and the recorder's µs per draw against W5.
- **Confirms (STIMA):** the save passes without a crash; characters whole or less broken (if the in-place update was the cause); front end back near 3 s per 10 s and recorder per draw under run 49's 44-54 µs at 1,020 MHz-equivalent; with the clocks, audio towards 85-100 % of real time and play fps above run 48's.
- **Refutes:** pictures as run 49 with 0 differences: the constant mechanism was not the cause.
- **Fails:** a crash at the save again (read `game-crash.log`); new missing objects (versions: rollback `nx_dk_constant_versions = false`); the console too hot or clocks read != wanted (delete `nx_cpu_mhz`/`nx_emc_mhz`).

## 2026-11-04: run 50 read and run 51 (Y): the other ports' work, Liberty Recompiled's renderer on NVK, the clocks through sys-clk, the mixer in NEON

The user's report of run 50: the picture as broken as before, crackling sound, poor speed; no improvement felt over the
earlier runs. New instruction: stop re-deriving what other ports already have; the authors of the ports (kabba536's GTA IV
port among them) gave permission to reuse their code whatever the licence differences.

### Y1. Run 50 read (`out/console-runs/run50-logs/`, two sessions)

| | |
|---|---|
| Session 1 (`game.prev.log`) | **unhandled fault at 35 s** while the title loads its `.img` archives (main thread): `pc` in `__dynamic_cast` called from `XFile::IsIoTraceEnabled` (`xfile.cpp:61`), `far 0xd01ec`: the `device` pointer reached through `file_ -> entry -> device` is garbage. **New**: no earlier run has it. Caller unknown (the crash record had pc and lr only, and lr was inside the faulting function). The VFS never frees entries of the read-only `game:` device (`Entry::Delete` refuses), so the entry is not freed there; the cause is NOT found. |
| Session 2 (`game.log`) | 297 s, ended with `+` (exit mode D, clean). The save point of run 49 (1,355 s) **not reached: the save fix is still unverified** (0 `host open of a locked file` lines). |
| Clocks | **Not applied**: `read back DIFFERENT`, CPU 1020 MHz / memory 1331 MHz for the whole run; `clock tool: sys-clk/Horizon OC ... RUNNING`. clkrst set them, the sysmodule put its own back within a second, every 5 s ("raising again"). Run 50's audio and speed numbers are therefore at the old clocks. |
| Constants | `constant banks checked against the front end's 116,928, different: vertex 0, bone palette 0, pixel 0`; versioned blocks 978,702 / 912,078, inline pushes 0. **The constants reach the GPU exactly as the front end sent them**: the constant path is not what breaks the picture (refuted for good). |
| Speed | play 2.7-4.4 fps at 4,000-6,700 draws per frame; recorder 31.8-34.4 us per draw, of which **vertices 18-19 us (55-60 %)**; front-end copies recycled 1.51 M times, 18,770 allocated (the pool works). |
| Audio | 53-55 % of real time, output ran dry ~50 times per window: the same as run 49 (same clocks). |
| Pictures | street geometry mostly whole; letters missing in the help text (`Pa k  u`), black holes in walls, curved shapes in the sky (a paraboloid reflection-like picture over the scene), characters almost invisible. |

Conclusion: the deko3d recorder's design costs ~32 us of CPU per draw (vertex conversion on the CPU for every draw) and,
after fifteen runs of fixes, still draws wrong. The renderer that draws GTA IV right on this hardware already exists:
Liberty Recompiled's `gta4_native` on Vulkan, which kabba536's port runs on NVK at 15-20 fps in the city.

### Y2. What was taken from the other ports (provenance)

| What | From | Author / repository | Where it went |
|---|---|---|---|
| Switch changes to the LRA renderer (UBO alignment 256 on NVK, identity texture views = R/B fix, bigger batches, frame-complete signal 2, queued frames 3, resolves as draws, texture sub-allocation arena, texture cache cap, pipeline cache rename on Horizon, `bad_alloc` skip, buffer change streak, thread priorities, report lines) | `source/gta4nx-switch.patch`, files under `src/graphics/gta4_native/` (7 diffs, applied unchanged; `native_texture_memory_arena.h` new) | kabba536, `github.com/kabba536/Grand-Theft-Auto-IV-Native-Port-Nintendo-Switch` @ `dbd7288`, on LibertyRecompAndroid `7c97c58` (vaduur) | `glue/rexglue-sdk-main/src/graphics/gta4_native/` |
| NAK fix `FloatCmpOp::IsNum | IsNan => self` | `source/build_nvk.sh` (Mesa rebuild) | kabba536 (same repository) | **as a binary patch** of the built `libvulkan.a`: `tools/switch-game/nak_flip_patch.py` (no Rust toolchain here) |
| sys-clk / Horizon OC override (sm 65100 before `smGetService`, service names, command 0 check, command 8 SetOverride, release only what was set) | `src/ui/switch_sysclk.cpp` | kabba536 (same repository); protocol from Horizon-OC/sys-clk-plus | `tools/switch-game/nx_clocks.cpp` (`sysclk::`) |
| The guest mixer's voice loop in NEON | `gta4_native_hooks.cpp`, `sub_82199BC8` (USA TU8) | kabba536 (same repository) | `tools/switch-game/nx_audio_mixer.h/.cpp`, at the EU address `8216DBB0` (checked instruction by instruction against our generated body; multiply and add kept separate as in the generated code) |
| The renderer itself | `gta4_native` | Liberty Recompiled (OZORDI) / LibertyRecompAndroid (vaduur) @ `7c97c58` | already in our tree since M0; now linked into the Switch NRO |

Looked at and not taken this run: kabba's `switch_audio_fast.cpp` (verbatim USA recompiler output: game code of another
edition, and our generated EU code is the same thing for our edition), their threading rewrite (lock-free events, raised
guest threads with yields as sleeps: depends on their whole `threading_switch`), the far-clip and streaming hooks (USA
addresses, EU equivalents to find), reNut / plume (a D3D-level renderer for sa2-nx and mgr-nx-deko: replacing
`gta4_native` with it is a rewrite of the hooks), nfsmw-nx's renderer (its draw path needs their Mesa patch).

### Y3. The changes (uncommitted, tree `17f3a18-dirty`)

1. **`nx_gpu = lra`: Liberty Recompiled's renderer on NVK** (`tools/switch-game/CMakeLists.txt` `THEFT4_SWITCH_LRA`,
   `build_variants.sh lra`, NRO `theft4-lra.nro`): `graphics_system.cpp` and its 20 sources in a static library, the
   game-derived shader cache and the override cache (PC build) compiled in, the EU hooks (as the deko3d build),
   `nx_lra_platform.cpp` for the Switch services kabba's code calls (`native_switch_platform.h`), `main.cpp` creates
   `Gta4NativeGraphicsSystem` on the NVK window, heartbeat line `lra:` (frames published, fps). The SDK writes its
   libraries per build dir (`REXGLUE_OUTPUT_DIR`), so the deko3d and LRA builds do not overwrite each other.
2. **NAK flip fix** (`nak_flip_patch.py`): 8 jump tables, 16 bytes; the 3 used on Maxwell (sm20 FSet / FSetP, shared by
   sm50, and sm50 DSetP). Without it every GTA IV pipeline with an ordered/unordered compare fails (kabba: black screen).
3. **Clocks through sys-clk** (`nx_clocks.cpp`): when clkrst's clocks do not hold, the same clocks are asked of the
   clock sysmodule (override), and re-asked if it puts its own back; released at exit (not after a crash: then the
   overlay or a reboot clears it). Log: `clocks (...): sys-clk service '...' (API n): override ... set`.
4. **Mixer voice loop in NEON** (`nx_audio_mixer.h/.cpp`, cvar `nx_audio_neon_mixer`, default on): both callers in
   `gta4_recomp.2.cpp` call `sub_8216DBB0` (hand patch, backup in the scratchpad), count in the `audio:` line.
5. **Crash record callers** (`main.cpp`): the frame-pointer chain of the faulting thread, 32 frames, as ELF addresses.
6. **`XFile::IsIoTraceEnabled` cached** (`xfile.h/.cpp`): the `dynamic_cast` through `file_ -> entry -> device` once
   per file instead of on every read and write.

### Y4. Offline tests (VERIFICATO)

`python tests/deko3d/test_run51.py` (new; `dk_run51_probe.cpp` and the test need `git add -f`): **OK**.

| Part | Result |
|---|---|
| MIXER | the NEON voice loop (simde's NEON emulation on the PC) against a transliteration of the generated EU `sub_8216DBB0` (same `lvx128`/`VectorMaskL`, same separate multiply and add, same store order): **4,000 voices, 500 with source overlapping destination, 0 bytes different** |
| NAK | `nak_flip_patch.py` on the portlibs `libvulkan.a`: **8 tables patched (3 used by Maxwell), 0 refused, 16 bytes changed** (each table's bytes 12-13 from the panic block's offset to OrdEq's); re-read from the patched archive: `0f0f0006 020a0f0f 0e080c04 0f0f` |
| run 50 tests | `test_run50.py` OK (with `test_run49.py`, `test_constant_diff.py`, `test_packet_ring.py`) |

Not testable offline (said, not claimed): the sys-clk override (an IPC to a sysmodule), the crash-record walk, the
renderer on NVK (it needs the console's GPU): the console run decides.

### Y5. Build (VERIFICATO)

`out/console-runs/run51-build/` (`-j3`, one build at a time; logs `run51-build.log`, `run51-lra-build.log`):

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` (deko3d, as run 50 + Y3 items 3-6) | 46,979,269 | `81def2b0ec4bcefec8a2c137f6b31959c251e472a460dcb29e8e99337e0e6697` |
| `theft4-lra.nro` (NVK + Liberty Recompiled's renderer, Y3 items 1-6) | 93,198,533 | `2ccc062b342aeb930edb519389e01b5246cadb568fa7af094f8cebdfa1a204a5` |
| `switch-game-dk-debug.elf` | 316,595,656 | `b5d113b4…2f35` |
| `switch-game-lra.elf` | 480,937,632 | `0a674917…c806` |
| `game.toml` (deko3d) / `game-lra.toml` (LRA) | | `9dfdcbfb…e198` / `697f560b…a4c9` |
| `deko3d/` | | unchanged package `deko3d-out-s47` (same hashes as run 50) |
| `lra/font_atlases/` | 39 files | game-derived, optional |

Full list in `SHA256SUMS.txt`. Checked in the ELFs: the new log strings of both builds; `Gta4NativeGraphicsSystem` and
`nx_gpu = lra` only in the LRA one; `sub_8216DBB0` a strong symbol with 2 direct calls; in the LRA ELF the Maxwell
flip tables appear 3 times patched and 0 times unpatched. The LRA renderer compiled with GCC 16 after two fixes
(portlibs include folder after the SDK's Vulkan headers; `REXGLUE_BUILD_CONFIG`) and linked with no undefined symbol.

### Y6. Run 51: what to do, what confirms, what refutes

Two NROs, one session each, same route (intro cut-scene to the end, then the first mission past the safehouse save):
1. **`theft4-lra.nro` first** (the bet of this run). SD: `sdmc:/switch/theft4/` gets the NRO, `game-lra.toml` renamed
   `game.toml`, and the `lra/` folder. Confirms: `graphics: nx_gpu = lra` and `lra: … frames published` lines with
   fps > 0, a picture (photo with the phone: this path has no PNG captures). Refutes / to read: a black screen with
   sound (NAK or pipeline failures: `vk`/`pipeline` errors in game.log), device lost, `bad_alloc`, out of memory.
2. **`theft4-dk-debug.nro`** with `game.toml` and `deko3d/` (the comparison and the fallback).
Both: `clocks (…): sys-clk service '…' … set` and `clocks read: CPU 1785.0` (if sys-clk refuses, the line says so);
`audio:` % of real time and `NEON mixer voice loops` > 0; a crash now records `caller NN` lines.

Not solved, not claimed: run 50's 35 s fault (cause not found; the callers will be in the next record), the save fix
(not reached in run 50), the deko3d recorder's pictures (no picture fix in it this run; the LRA build is the answer
being tried), the mixer thread at 95 % of a core (the NEON loop is ~7 % of it; the clock is the larger lever).

## 2026-11-04: run 51 read and run 52 (Z): both NROs stopped by the NEON mixer, deko3d stays the renderer, nfsmw-nx's geometry store, rectangle lists as the Vulkan path

Direction from the user after run 51: the goal is a native HOS port on deko3d, faithful to the Xbox 360 version (720p,
30 fps at standard clocks as the reference, no reduced resolution, traffic or quality). LRA/NVK is not the destination
and no more time goes into it; single techniques from other ports may be reused (permission from their authors).

### Z1. Run 51 read (`out/console-runs/run51-logs/dk`, `.../lra`)

| | |
|---|---|
| Both NROs | **unhandled fault at 21 s (deko3d) and 24 s (LRA)**, at the first sounds. Symbolized: `pc` in `vld1q_u8` inlined into **`sub_8216DBB0`, run 51's NEON voice loop**; `far` = `base + 0xDAA00730` (deko3d: `0x33A9400730` = x1 `0x32CEA00000` + `0xDAA00730`). The record's new `caller NN` lines gave the chain (the mixer's callers in `gta4_recomp.2`). |
| Cause | **run 51's regression**: the loop read guest memory as `base + address`. On the Switch guest memory is not one block (design A, 03-memory.md): every address goes through `rex::memory::GuestToHost` (`rex_guest_table`, one entry per 16 MiB slice), which the generated code does in `REX_RAW_ADDR`. The voice buffers are in the physical heap (`0xDA…`), far from `base + address`. Run 51's offline test could not see it: it used one flat buffer for both sides. |
| Clocks | `clocks read: CPU 1785.0, GPU 460.8, memory 1600.0 MHz … read back as set` with sys-clk running: the raise held this time. |
| LRA | the same fault (same mixer), before any renderer question was reached; per the user's direction not pursued further. |

### Z2. deko3d ports examined (provenance)

| Repository (author) | deko3d code | Taken / why not |
|---|---|---|
| nfsmw-nx (StevensND), `app/src/nfsmw_dk_*` | a deko3d renderer for an Xbox 360 title with XenosRecomp shaders: work slots and pre-lists (`nucleo`), geometry store (`geometria`), textures with fingerprints (`texturas`), targets and resolves (`destinos`), draws (`dibujos`) | **taken: the geometry store** (`nfsmw_dk_geometria.h/.cpp` → `deko3d/dk_geometry_store.h/.cpp`), the closest match to our measured bottleneck; its slot/fence model is already ours (Core::BeginWork / WorkDone), so the port is direct. Next candidates: texture fingerprints (our textures cost 1.9-4.3 us per draw), index ranges through the same store. |
| sa2-nx (chanseyisthebest), mgr-nx-deko: reNut's renderer (masterspike52) on plume's deko3d back end | a D3D9-level renderer (`renut_engine/gpu/render/draw.cpp` ~5,800 lines, `textures.cpp`, `surfaces.cpp`) over plume | not taken this run: its draw path sits under the game's D3D calls and plume, ours under gta4_native's title commands; replacing the recorder with it is a rewrite of the front end, not a module swap. To study next: its surface cache and draw batching. |
| plume deko3d back end (renderbag/plume, via banjo-nx) | already in `thirdparty/plume-deko3d` (M2) | unchanged |
| LRA `graphics_system.cpp` (Vulkan; the reference) | not deko3d | **taken: its rectangle-list rule** (below), for Xenos fidelity |

### Z3. The changes (uncommitted, tree `17f3a18-dirty`)

1. **NEON mixer through GuestToHost** (`tools/switch-game/nx_audio_mixer.h/.cpp`): the kernel takes the translation as
   a callable; the console passes `GuestToHost`, per 16-byte vector (a buffer crossing a 16 MiB slice is right too).
   Checked in the ELF: the loop loads `rex_guest_table` (an `adrp` to its page). Run 51's crash cause removed.
2. **Geometry store** (`deko3d/dk_geometry_store.h/.cpp`, port of nfsmw-nx's; cvar `nx_dk_geometry_mb`, default 96,
   0 = off): vertex ranges read in place from guest memory (not the front end's submit copies, not DrawPrimitiveUP, not
   rectangle lists) are looked up by host pointer, byte count and mode (stride + fix-ups) and hashed (XXH3 per 64-byte
   line); a range seen twice with the same bytes is written once into a persistent GPU block and bound from there
   afterwards, with no copy and no byte swap. Blocks are returned only when every work that bound them is done.
   **Differences from nfsmw, for exactness**: no trust for the rest of an epoch and no sampled checks (both options,
   off): every lookup hashes the whole range, because our recorder reads guest memory after the title has gone on.
   Report line `deko3d: run 52 geometry store (totals)`.
3. **Rectangle lists as the Vulkan path** (`deko3d/dk_rect_list.h`): the corner is the vertex opposite the longest
   edge of the position, the fourth vertex is `c1 - c0 + c2` for float32 elements only. Through run 51 v0 was always
   the corner and every 4-byte word that read as a finite float was extrapolated, colours included. Text and sprites
   use rectangle lists; a wrong corner gives a sliver instead of a glyph: a candidate for run 50's missing letters
   (unproven until the console shows it).
4. Kept from run 51: the sys-clk override, the crash record's callers, the cached `XFile` trace flag. The LRA build
   stays in the tree (`build_variants.sh lra`), not built for this run.

### Z4. Offline tests (VERIFICATO)

`python tests/deko3d/test_run52.py` (new; `dk_run52_probe.cpp` and the test need `git add -f`): **OK**.

| Part | Result |
|---|---|
| STORE | 72,000 simulated draws over static, sometimes-changed and always-changed buffers, 3 slots, a GPU finishing late, a 1 MiB store (19,867 evictions): **0 ranges bound with bytes other than the guest's of the moment, 0 blocks changed while a work that bound them could still run**; 99.5 % of static lookups served from the store |
| RECT | 20,000 rotated rectangles, three corners in random order: **0 errors** (the four true corners in perimeter order, uv kept, colours untouched); the old rule was wrong for 13,324 of them |
| MIXER (test_run51) | now on guest memory split into 1 KiB slices in reverse order, reached only through the translation (what run 51's test missed): 4,000 voices, 0 bytes different |
| NAK, earlier runs | OK (test_run51 → test_run50 → test_run49, test_constant_diff, test_packet_ring) |

### Z5. Build (VERIFICATO)

`out/console-runs/run52-build/` (`-j3`, log `run52-build.log`); full list in `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,003,845 | `579acd4140ccb5042b2cd1802f91b8c1c070d882d74747e20970e1491112e8e6` |
| `switch-game-dk-debug.elf` | 317,201,096 | `adecea2ac4cc813f100b08c5df7ddd838e7d6dec4aa0fd6513f88b6e86f24bbb` |
| `game.toml` | | `89086a96e1c32d826b3868497ed54038a3f04563df1c63d40ca79da8e6e007ff` |
| `deko3d/` | | package `deko3d-out-s47`, unchanged |

### Z6. Run 52: what to do, what confirms, what refutes

Standard clocks (game.toml). Intro cut-scene to its end, then the first mission; captures every 10 s.
- **Start-up**: past 24 s with sound (run 51 stopped there); `audio:` with `NEON mixer voice loops` > 0.
- **Speed**: `recorder time per drawn draw … vertices` (run 50: 18-19 of 32 us) and play fps (run 50: 2.7-4.4 at the
  same per-draw cost); `run 52 geometry store (totals)` hits against lookups. This run is at standard clocks: compare
  per-draw times first.
- **Picture**: the help text's letters (run 50: `Pa k  u`), sprites, the HUD.
- If something draws wrong that run 50 drew right: `nx_dk_geometry_mb = 0` isolates the store.

Not solved, not claimed: characters in pieces, the curved shapes in the sky, the dark lighting, run 50's 35 s `XFile`
fault (not seen in run 51), the save fix (still not reached).

## 2026-11-04: run 52 read and run 53 (AA): letters back, the clock left by a fault, the store's own cost, indices and copies through the store, the skinning checked on the CPU, the LOD bias

### AA1. Run 52 read (`out/console-runs/run52-logs/`, 39 captures)

| | |
|---|---|
| Start-up | past run 51's stop: **no fault**, ended with `+` (exit mode D). The NEON mixer ran: `NEON mixer voice loops` 130 M. |
| Clocks | **not the standard ones the game.toml asked for**: `clocks read: CPU 1785.0, memory 1600.0 MHz … CPU ABOVE 1020 MHz`. Cause: the sys-clk override run 51 set was never released (run 51 ended in a fault, and the override does not expire). Run 52's numbers are at 1785/1600, comparable with run 51, not with the standard-clock goal. |
| Text | **the help text is whole** ("Get into Roman's car."): run 50's missing letters are gone, consistent with run 52's rectangle-list rule (the only change on that path). |
| Speed | play **4.6 fps at 5,400-5,500 draws per frame**; recorder **21.3-21.5 us per draw** = textures 1.3 + **vertices 12.8** + indices 0.9-1.0 + index scan 1.3 + constants 1.6-1.7 + rest 3.4 (run 50: 31.8-34.4 us, vertices 18-19, at CPU 1020). |
| Geometry store | lookups 2.70 M, **hits 2.65 M (98 %)**, 38.4 GB not copied, changed 12, evictions 17,863, 84 of 96 MiB used. But **hashed 39.0 GB**: every lookup hashed its whole range, so the store cost almost what it saved; the 12.8 us of "vertices" are now mostly hashing. And **3.79 M draws used the front end's submit copies**, which run 52 sent to the ring, not the store. |
| GPU | **busy 7.6 s of every 10 s** (handheld, 460.8 MHz): the GPU is near its limit too. Most of it: the main 1280x720 targets 2.28 s (138 passes, 60 k draws), targets `40076EB0`/`400007C0` 1.66 s in **6,486 passes** (22 k draws: ~140 passes a frame), the 2560x2560 shadow map 1.52 s (146 k draws). |
| Audio | 77-100 % of real time, median 96 % (run 50: 53-55 %): the NEON loop and the raised clock together; how much each is not separable from this run. |
| Characters | still in pieces, stretched toward the bottom of the screen; skinned draws logged with sane inputs (weights summing to 255, indices in the palette). |

### AA2. What was looked at for the characters and the lighting (each refuted with the reason)

| Candidate | Finding |
|---|---|
| Shared constants `g_Swapped*` (texcoords, normals, blend weights) | LRA never sets them (always 0); ours neither. Same. |
| Booleans `g_Booleans` | same source words (device +0x2780 / +0x2790) and packing as LRA. Same. |
| Half-pixel offset, clip plane, alpha threshold, fragment scale | same values as LRA. |
| Vertex formats (D3DCOLOR BGRA, UBYTE4N, DEC3N w = 1, 16-bit halves) | same as LRA's `ConvertVertexElementFormat` and conversion pass, fix for fix. |
| Inputs without an element (zero record) | LRA reads zeros too (`kDefaultVertexBinding` over a zeroed record); a skinned shader with no blend inputs collapses to a point, which is invisible, not a visible shard. |
| Base vertex | first vertex = base + lo, draw offset -lo: same as LRA. |
| Guest memory slices | inside each window (virtual, 0xA0/0xC0/0xE0) the guest table maps contiguously, so a buffer is never read across two host blocks. |
| uam patches 7 (no dual issue) and 8 (stall 2 before a new block), nfsmw-nx's list of deko3d/uam pitfalls | both present in `thirdparty/uam`; tile-size fix present; no sRGB targets; device flags as required. |
| EU hooks left at USA addresses | `kEaaParameterUploadReturnAddress` and `kMainPresentCaller` (census: ambiguous / not found) and three trace globals: they only neutralise the game's own EAA (keeping it is the Xbox behaviour) and label diagnostics. |
| Resolve colour exponent | one texture (`40079D00`, guest format 29, exponent 1) is resolved with it; LRA, the reference, does not apply it either. Left as LRA. |
| Texture LOD bias | **real difference**: the shaders read it from the shared block (1,328 of 1,912 translated shaders read bytes 752+), LRA writes it, our recorder only counted it. Fixed (AA3.5). |

The skinning path is the same as LRA's in every place that can be compared by reading. What is left is whether the
data the GPU gets for a skinned draw (the palette in the draw's constant version, the first vertex) is a sensible set
of rigid bones: run 53 checks that on the console (AA3.4), so that run 54 can tell geometry data from shader execution.

### AA3. The changes (uncommitted, tree `17f3a18-dirty`)

1. **Geometry store, its own cost** (`dk_geometry_store.*`): without sampling, one XXH3 pass per range instead of a
   hash per 64-byte line folded together; **trust per epoch on** (nfsmw-nx's mode, cvar `nx_dk_geometry_trust`,
   default true): a range checked once is trusted until the next work, frame or non-draw command (`Forget` in Clear,
   Resolve, ResourceEvent and DepthHandoff), which is where the title can have waited for the GPU. Run 52 saw 12
   content changes in 2.5 M lookups.
2. **Submit copies through the store**: a copy lives at a new host address every draw, so it is keyed by the guest
   address of its range and marked never-trusted (mode bit 63: its buffer was copied because the title rewrites it).
3. **Indices through the store** (nfsmw-nx keeps indices with their vmin/vmax): converted straight into the store on
   admission, bound from it afterwards with the stored lo/hi, no conversion and no scan.
4. **CPU skin check** (`dk_skin_check.h`): 1 skinned draw in 64, its first vertex skinned on the CPU with the draw's
   own constant version, decoded as the GPU does (D3DCOLOR BGRA, register = floor(i / 255 * 765.0059 + 0.5)); reports
   bones that are not rigid, all-zero bones, and vertices more than 3 m from their bones. Lines `run 53 CPU skin check`
   (16 samples, then up to 32 implausible ones) and `(totals)`.
5. **Texture LOD bias** in the shared block (byte 752 + 4 x slot) with LRA's `NativeSamplerLodBias` (signed 10 bits in
   1/32 units, clamped to 15).
6. **sys-clk override left by a fault**: a marker file (`sdmc:/switch/theft4/sysclk-override.txt`) is written while an
   override of ours is set and removed when it is released; found at start-up it means an earlier run did not exit
   cleanly, and that override is released before this run's clocks. An override set by hand in the overlay has no
   marker and is not touched.
7. **Measurements**: line `run 53 vertices per drawn draw`: store lookups (hash included) and ring copies in us per
   draw, streams and MiB each, ring MB/s, waits for a free work slot, index ranges from the store against converted;
   the store's totals add `hits without a hash`.

Not changed, and why: the GPU's 140 passes a frame on `40076EB0`/`400007C0` and the resolves through the copy engine
(full barriers both sides). kabba536's port moved resolves to full-screen draws for the same reason on NVK; on deko3d
it needs a blit shader package entry and a test of every resolve format: the next GPU step, measured first by this
run's per-pass GPU times.

### AA4. Offline tests (VERIFICATO)

`python tests/deko3d/test_run53.py` (new; `dk_run53_probe.cpp` and the test need `git add -f`): **OK**. Guest memory in
the probe is split into 1 MiB slices placed out of order, as the Switch's guest table, and the copies live at a new
host address every draw.

| Part | Result |
|---|---|
| STORE | 48,000 draws, trust per epoch on, buffers rewritten between epochs and dynamic ones at any moment through copies: **0 ranges bound with other bytes than the draw's, 0 blocks changed while a work that bound them could run**; 176 copies and 473 trusted hits served |
| INDEX | 18,000 lookups, 17,891 hits: **0 stored bytes and 0 lo/hi different from ConvertIndices** (a first version of the test changed a list inside an epoch and got 16 stale hits: the case trust excludes by design, and why copies are never trusted) |
| SKIN | 3,000 palettes of random rigid bones: the CPU skin equals a direct computation, **0 good palettes flagged, 0 scaled or zeroed bones missed** |
| LODBIAS | all 1,024 signed 10-bit values decoded and scaled as LRA: 0 errors |
| earlier runs | test_run52 (store, rectangle lists) → test_run51 (mixer on scattered memory, NAK) → test_run50 → test_run49, constant diff, packet ring: OK |

### AA5. Build (VERIFICATO)

`out/console-runs/run53-build/` (`-j3`, log `run53-build.log`, 0 errors); full list in `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,020,229 | `62b43438d06d66ea0bb73b17973fc9b6ce96760f61f2ef0c4d3f55ba16c93046` |
| `switch-game-dk-debug.elf` | 317,367,744 | `f604a8058efbc36f52e45c193d2807dd0ccef439c214d570ad9af29ee26084cf` |
| `game.toml` | | `f6f8030941745e51130cb6a6ed3724a4eff677a63c8855e319f81b3e582c7f58` |
| `deko3d/` | | package `deko3d-out-s47`, unchanged |

### AA6. Run 53: what to do, what confirms, what refutes

Standard clocks (check `clocks read: CPU 1020.0`, and a line `released` if the left-over override was found). Intro
cut-scene to its end, then the first mission; captures every 10 s.
- **Speed**: `recorder time per drawn draw` (run 52: 21.3 us, vertices 12.8, at CPU 1785) and `run 53 vertices per
  drawn draw` (store us, ring us, slot waits). Per-draw times at 1020 MHz against run 52's at 1785 MHz: the same work
  costs ~1.75x more at the lower clock, so equal per-draw times would already be a gain; fps at standard clocks is the
  goal's measure. GPU busy per 10 s and the pass list.
- **Characters**: the `run 53 CPU skin check` lines. Rigid bones and vertices within 3 m of their bones with
  characters still in pieces on screen = the data is right and the fault is in the shader path (translation, uam
  compile, or how the GPU reads the constant version); otherwise the line says which part of the data is wrong.
- **Picture**: texture sharpness where the title sets a LOD bias (distant road surfaces, signs).
- If anything draws worse than run 52: `nx_dk_geometry_trust = false`, then `nx_dk_geometry_mb = 0`.

Not solved, not claimed: characters in pieces, the lighting, the curved shapes in the sky, 720p/30 fps.

## 2026-11-05: run 53 read and run 54 (AB): skinning data proven sane, a GPU read-back of the vertex shader, texture locks read back for the exposure

### AB1. Run 53 read (`out/console-runs/run53-logs/`, 32 captures)

| | |
|---|---|
| Run | no fault, ended with `+`. Clocks CPU 1785 / memory 1600 (the user's overclock). |
| CPU skin check | **the data is sane**: 501 skinned draws checked, 708 bones used, **0 not rigid, 0 all-zero**; the palettes are in model space (bone origins within a metre, vertices 0.5-1.3 m from them). The 18 "more than 3 m" are single-bone draws whose one bone is identity or a whole-object transform (props), not characters. The fault is therefore after the data: in the vertex shader's execution or what follows it. |
| Speed | play **6.0-9.0 fps** at 2,200-4,200 draws per frame (run 52: 4.6 at 5,400-5,500); cut-scene 10-15 fps. Recorder per draw: textures 3.0 + vertices 4.6 (store lookups 3.8, ring 0.7) + index scan 1.2 + constants 1.5 us, and **130 us waiting for a free work slot**: the GPU is the limit now. Store: 8.25 M lookups, 98.5 % hits, 470 k trusted (no hash); index ranges 48 k from the store against 80 converted per window. |
| GPU | **busy 9.8 s of every 10 s**. `40076EB0`/`400007C0` 1280x720: 5.1 s in 5,420 passes (9,165 draws), alternating with `400007C0`/`400007C0` (4,964 passes of 1 draw): the deferred lights, one stencil pass and one light pass each, ~1 ms per light pass. The main 1280x720 targets 1.5 s; the 256x256 targets 0.36 s; resolves ~0.25 s each target. |
| Audio | 77-100 %, median 99 %. |
| Pictures | characters still in pieces; daytime **washed out to white**, night **almost black**, black shapes in the sky. The swing in both directions points at the exposure (the luminance the title measures), not at the light shaders themselves (AB2). |

### AB2. Causes found and what was done about each

**Lighting / exposure: the title's texture locks were never read back.** `Deko3dGraphicsSystem::ExecuteTitleCommand`
answered `kTextureLock` at once: "Not read back to guest memory yet (B4) … the title locks the guest copy as it is".
GTA IV measures its scene luminance on the GPU and reads the result on the CPU by locking that GPU-produced texture
(Liberty Recompiled's hook `sub_82A44168`, the Vulkan path's synchronous `ReadbackTextureToGuest`); the adaptation then
sets the exposure. With the lock answered from stale guest memory the exposure is whatever those bytes say: a white
day and a black night are the expected result. Run 54 implements the lock as the Vulkan path does:
- `ExecuteTitleCommand(kTextureLock)` puts a `RecorderTextureLock` packet into the ring after every draw already
  submitted (under the front-end lock, so in stream order) and the title's thread waits on it (5 s at most, then the
  guest copy as before, logged);
- `DkRecorder::TextureLock`: a resolve destination written by the GPU is copied (`dkCmdBufCopyImageToBuffer`) after the
  open work's draws into a CPU-cached read-back block, the work is submitted and waited for, and the texels are
  written into the texture's guest memory with the fetch constant's layout (`TextureInfo::Prepare`, `GetMipLocation`,
  tiled offsets `GetTiledOffset2D`, `CopySwapBlock` by its endianness): the same steps as LRA's
  `ReadbackTextureToGuest`. Not GPU-produced: the guest copy, as LRA. Levels, array slices, depth: refused, as LRA.
- Lines: `run 54 texture locks (totals)` (title side: locks, copied, timeouts, wait) and `run 54 texture locks in the
  recorder (totals)`; the first lock of each texture is named once. Off switch `nx_dk_texture_lock_readback = false`.
Not proven on the console: that the luminance texture is among the locked ones and that the picture's exposure changes.

**Characters: a differential CPU/GPU test of the same draw.** deko3d 0.5.0 has no transform feedback, but a vertex
shader can write a storage buffer. `deko3d/dk_shader_probe.h` instruments the draw's own GLSL (the package's variant
text): at the end of main every variable declared at main's top level, then gl_Position, is stored as raw bits for the
first 64 vertices (gl_VertexID = index - index_min). On the console (`nx_dk_skin_diff`, default 1) the first skinned
draw is followed, once the instrumented shader has compiled on the console's uam, by the same draw with that shader
and the rasterizer off (nothing on screen; the draw's shaders and rasterizer state bound again right after); the
values are read back when the work is done and saved with the exact inputs the GPU had (attribute states, the vertex
buffers in host form for those 64 vertices, the vertex constant version, the shared block) in
`sdmc:/switch/theft4/probe/vs_<hash>_<variant>_<draw>.probe`. On the PC `tools/deko3d/probe_compare.py` decodes the
inputs as deko3d's vertex fetch does, runs the same GLSL with float32 arithmetic and names, per vertex, the first
variable whose GPU value differs. This is the "where does the GPU leave the program" answer runs 48-53 could not give.

**The sky's curved shapes**: not identified in this run. The pass list of run 53 does not name a sky draw, and the
probe covers vertex shaders of skinned draws. If the run 54 probe shows the skinning executes right, the same tool
can be pointed at the sky's vertex shader next (a hash list), once a capture names it.

**Resolves through the copy engine**: they are copies already (`dkCmdBufCopyImage`, same format; `dkCmdBufBlitImage`
nearest otherwise), with full barriers around them; measured at ~1.5 ms per 1280x720 resolve in run 53's start-up
window. Not changed: the user asked for the picture first.

### AB3. Offline tests (VERIFICATO)

`python tests/deko3d/test_run54.py` (new; `dk_run54_probe.cpp`, `test_run54.py` and `tools/deko3d/probe_compare.py`
need `git add -f`): **OK**.

| Part | Result |
|---|---|
| INSTRUMENT | the console's instrumentation on **636 of 636** of the package's vertex shaders (0 refused); the **12 skinned shaders** of runs 52-53 instrumented **and compiled by uam** (the PC build of the console's compiler) |
| INTERP | the interpreter on the real character shader `5E7C32C4EF6771C0` against an independent skinning, 200 vertices on random rigid palettes: worst relative error **1.2e-7** (its `_495` = skinned position + c3) |
| COMPARE | synthetic probes through the file format: clean = no difference; an altered bone register (`_150`) and an altered gl_Position found **exactly**, an unwritten vertex reported as not run |
| COVERAGE | the interpreter runs **514 of 636** vertex shaders; the other 122 fetch textures (refused by design, never guessed) |
| LOCK | the synchronous lock wait: completed by another thread; a wait nobody completes times out |
| earlier runs | test_run53 → 52 → 51 → 50 → 49, constant diff, packet ring: OK |

Not testable offline: the lock's GPU copy and its guest layout on the console, the probe draw on the GPU.

### AB4. Build (VERIFICATO)

`out/console-runs/run54-build/` (`-j3`, log `run54-build.log`, 0 errors); `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,196,357 | `319d8b5751b05b1384b207ccc34b74f7ff7fb34c6388a765252525e042b02483` |
| `switch-game-dk-debug.elf` | 321,942,888 | `9bd0a510c6520e2ca17b5af5decee7357042e32d5b721b453185abeba041f02c` |
| `game.toml` | | `4cd06fb2d756eb0c645973a801517a68d23904a840d9e93325b82c8e60d2a919` |
| `deko3d/` | | package `deko3d-out-s47`, unchanged |

### AB5. Run 54: what to do, what to bring back

Empty `sdmc:/switch/theft4/captures/` and `sdmc:/switch/theft4/probe/`. Intro cut-scene to its end, then the first
mission on foot near other people (a skinned draw must be drawn; the probe compiles in the background, 5-30 s).
Bring back game.log, game-crash.log, captures/ and **probe/** into `out/console-runs/run54-logs/`.
- `run 54 skin probe: … compiled in … s: ready` and `run 54 skin probe written: … .probe`: then
  `python tools/deko3d/probe_compare.py out/console-runs/run54-logs/probe/<file>.probe` names the first variable where
  the GPU differs (or says the shader executes as written, which moves the fault after the vertex shader).
- `run 54 texture locks (totals)`: locks > 0 and copied > 0; `texture lock … read back from the GPU` lines name the
  textures. Captures by day and by night: exposure.
- If the title stalls: `nx_dk_texture_lock_readback = false`.

Not solved, not claimed: characters, lighting, sky, 720p/30 fps.

## 2026-11-05: run 54 read and run 55 (AC): the GPU probe answered (one draw, not a character), the costs measured again, code taken from the other ports

### AC1. Run 54 read (`out/console-runs/run54-logs/`, 69 captures, 1 probe)

| | |
|---|---|
| Run | 659 s, ended with `+`, 0 faults. Clocks **CPU 1785 / memory 1600 (overclock), GPU 460.8 MHz handheld** in every heartbeat (`clocks read != wanted`: an external tool set them). |
| Texture locks | **53 locks, 0 of a GPU-produced texture** (the recorder answered "not a resolve destination, the guest copy" to all 53); the title waited **1,965 ms** for them in all, all during loading (t = 70 s). The run 54 hypothesis (exposure read through a texture lock) is **refuted**: GTA IV does not lock its luminance target. |
| GPU probe | written once: `vs_AD3E412AF420299A_0_1233849.probe`. run 54's `probe_compare.py` stopped at `dk_sel` (fixed: dk_sel, specialization constants, gl_VertexID, the probe's own stores). Result: **4 of 4 vertices: every one of the 57 values equal to the PC's**. But the draw had 4 vertices and one identity bone (a quad, `skinned (-5.14, 3.22, 0.00)`), not a character: the probe chose the first skinned draw. What it proves: the GPU executes that skinned shader exactly as the GLSL says (no uam or driver fault for it). What it does not prove: anything about the characters. |
| Speed | play windows (draws/frame > 1,500): **5.3 fps median** at 4,064 draws/frame; **22,400 draws/s**. GPU busy 7.6 s of 10. Title render thread (`id 12`) in `SubmitTitleCommand` 7.8 s of 10 (front end 6.0, lock wait 1.0, queue 0.7). |
| Pictures | characters still patchy (the cut-scene face: textured and dark patches), daytime washed out to blue/white, dark triangular shards over the city and the sky. |

### AC2. Runs 50, 52, 53, 54 side by side (medians of the play windows, VERIFICATO from the logs)

| Run | clocks CPU/GPU/mem | fps | draws/frame | draws/s | GPU busy (ms/10 s) | title front end / lock wait | recorder µs/draw | lights 40076EB0/400007C0 |
|---|---|---|---|---|---|---|---|---|
| 50 | 1020 / 460.8 / 1331 | 3.2 | 5,214 | 16,573 | 4,197 | 5,947 / 1,267 | 33.5 | 1,055 ms, 0.45 ms/pass |
| 52 | 1785 / 460.8 / 1600 | 6.7 | 3,517 | 24,619 | 7,471 | 5,380 / 1,169 | 21.3 | 1,820 ms, 0.26 ms/pass |
| 53 | 1785 / 460.8 / 1600 | 7.7 | 2,770 | 21,487 | 7,442 | 4,810 / 976 | 15.8 | 2,333 ms, 0.29 ms/pass |
| 54 | 1785 / 460.8 / 1600 | 5.3 | 4,064 | 22,401 | 7,589 | 6,009 / 960 | 15.0 | 1,739 ms, 0.29 ms/pass |

- **Why run 54 is slower than run 53:** the same throughput (22.4k against 21.5k draws a second) on a scene with 47 % more
  draws a frame (4,064 against 2,770). Not a regression of the code: the probe was one draw; the texture locks cost 1.97 s
  at loading only; the store, copies and recorder per draw are as run 53 (15.0 against 15.8 µs).
- **Correction of AB1:** the deferred lights' "5.1 s of 10" was one window of run 53. Over the play windows they are
  1.7-2.3 s of 10 (23-31 % of the GPU), ~0.29 ms a pass, ~0.6 ms a light (stencil pass + light pass). The largest GPU item is
  the main 1280x720 scene targets (2.2-3.0 s of 10), then the 2560x2560 shadow maps (1.1-1.9 s) and a 320x180 target
  (0.9-1.2 s).
- **The two limits:** the GPU is busy 75 % of the time and the title's render thread spends ~78 % of its time in our front
  end. Its profile (run 54, t = 460 s, 985 samples): 13.5 % waiting for newlib's malloc lock (`mutexLock` <- `_memalign_r`
  <- `operator new` from `CaptureDraw` / `DrawDataPool::Take`), 2.6 % the front-end lock, 6.6 % `memcpy` (submit copies),
  5.2 % `malloc`/`free`, 1-2 % `ScanIndices`, `DiffRegisters`, `memset`. The recorder's own profile: 12 % waiting for the
  same malloc lock in `_free_r` (the control blocks it releases).
- **Clocks:** every run since 52 at CPU 1785 (overclock), all at GPU 460.8 (handheld). The target (standard clocks) is
  CPU 1020; docked GPU 768. Nothing measured here is at the target's clocks.

### AC3. What the other ports had (the table asked for; benefits only where their documents measured them)

| Port, file | Technique | Problem solved | Reuse here | Benefit measured there | Done in run 55 |
|---|---|---|---|---|---|
| mgr-nx-deko / sa2-nx (Renut, masterspike52; banjo-nx), `banjo_renut_tuning.cpp` "package 28" | depth-only draws without the pixel shader | shadow maps and depth passes ran full pixel shaders | yes: our shadow maps (2560x2560, depth only) and the lights' stencil passes | "about half the draws. Same image" (no time) | **yes** (AC4.3) |
| sa2-nx `docs/renut-renderer.md`, uam `nv_shader_header.h` | KillsPixels from the program header | knowing whether a variant discards | yes | – | **yes** (flags from the DKSH) |
| LRA `graphics_system.cpp` (kabba536 / Liberty Recompiled) | depth bias x 2^24, slope x 1/16, always on | our bias off at stage 3, slope 16x | yes | – | **yes** (AC4.4) |
| LRA `ReadbackTextureToGuest` | locks of non-GPU textures answered at once | run 54: 37 ms a lock spent waiting for the ring | yes | – | **yes** (AC4.2) |
| masseffect-nx `docs/kernel-waits.md`, sa2-nx guide §7 | no allocation per draw / fewer locks per draw | newlib's one malloc lock shared by threads | yes | sa2-nx: device deltas 590 B a draw instead of 13,088; store cards hashing 16 % -> 0 | **yes** (AC4.1, our own pool) |
| masseffect-nx `optimization-paths.md` S6 | vertex shader writes only what the pixel shader reads (spec. constant; zero for depth-only) | VS output cost | yes: our GLSL has `g_banjoVsOutputs` (905) left at all ones | **scene 66 -> 36.6 ms** | no: needs VS x PS-input variants (next) |
| banjo-nx `banjo_renut_tuning.cpp` | colour resolves drawn, not on the copy engine | copy engine needs the GPU idle before and after | yes (our resolves: `dkCmdBufCopyImage` + 2 full barriers) | "Same bits" (no time) | no (next, with measurements of the barrier cost) |
| banjo-nx | hardware compression for depth targets | depth bandwidth | yes | "Same pixels" | no |
| banjo-nx | alpha-test function as a pipeline constant (no discard in variants without it) | early depth | already: our early/late variants | – | already |
| banjo-nx | eager kicks (several submissions a frame) | GPU idle while the CPU records | yes (GPU idle 2.5 s of 10) | latency, not throughput | no |
| sa2-nx guide §7 | store cards (guest stores mark 4 KB pages) | hashing of vertex/index/texture copies | yes (our store hashes 4.7 µs/draw) | hashing 16 % -> 0 | no (needs the codegen) |
| sa2-nx, nfsmw-nx `pgo/` | PGO of the recompiled code | CPU | yes | not read | no |
| masseffect-nx S23 | precise mad in vertex shaders | depth fighting | – | tried, no gain | see AC5 |

### AC4. What run 55 changes

1. **Copies at submit without allocation** (`native_draw_snapshot.h`). `DrawDataPool` keeps each entry as a shared_ptr made
   once; the recorder only drops its reference (no `free`, no lock of ours); an entry whose count is back to 1 is taken
   again with its storage (the round-robin's next one is the oldest: packets are consumed in order). Bytes grow without
   zero fill (`DefaultInitAllocator`); storage beyond 192 MiB in all is let go at the next take. Line: `run 55 copy pool`.
2. **Texture locks answered at once when no resolve wrote the texture** (`dk_graphics_system`): the front end keeps the
   set of resolve destinations; any other lock gets the answer the recorder would give, without the ring wait. Line:
   `run 54 texture locks (totals) … run 55: N answered at once`.
3. **Depth-only draws** (`dk_draw_rules.h` `IsDepthOnlyDraw`, `nx_dk_depth_only`): no colour written, a depth target, no
   alpha test or alpha to coverage, a pixel shader whose variants without the alpha test neither kill pixels nor write
   depth: drawn with the empty pixel shader, the same targets (no new pass). The flags come from the package: run 55's
   `deko3d-out-s55` = s47 with the record's reserved word set from each DKSH's program header (KillsPixels, omap depth);
   **204 of 1,276 pixel shaders kill pixels, 4 write depth**; the package differs from s47 in those 1,912 words only. A
   package without flags (s47) leaves the rule off. Line: `run 55 draws in the interval`.
4. **Depth bias as the Vulkan path** (`DepthBiasFor`, `nx_dk_depth_bias`): on at every stage (through run 54 only at
   stage 4; the runs used 3), slope x 1/16. The shadow maps were drawn without the bias the title asks for: dark patches
   from self-shadowing are what that gives on curved surfaces (faces). A hypothesis for the patches, not proven.
5. **The probe on characters** (`dk_shader_probe.h`, `dk_recorder.cpp`, diagnostic mode): meshes of 200+ vertices only,
   one per vertex shader, up to 4; file version 2 adds gl_Position of every vertex (up to 8,192), the indices as
   gl_VertexID, the viewport, the target's size, depth state, cull mode and pixel shader. `probe_compare.py` reads v1 and
   v2, compares every position and draws the mesh as the GPU placed it beside the PC's (`<file>.probe.png`): an intact
   mesh moves the fault after the vertex shader (rasterization, depth, lighting), a broken one before it (inputs).
6. **The diagnostic mode** (`nx_dk_diagnostics`, default off): the GPU read-backs only with it. game.toml of run 55 turns it
   on for this run (4 draws at most).
7. **The costliest pass of each target** in the 10 s GPU line (`run 55 costliest single pass per target`): its time,
   draws and the first draw's shaders: names the light passes' shaders for the next step.

### AC5. Refuted in this run (VERIFICATO offline)

- **`invariant gl_Position` (devlog W4's untried lead):** uam 1.1.0 compiles the 12 skinned vertex shaders and an
  unskinned one to **the same bytes with and without it**; with every temporary `precise`, the skinned ones are still
  byte-identical (the translation wraps every multiply in a zero/denormal select, so nothing can be fused), the unskinned
  one changes. So fused multiply-adds cannot make two skinned passes disagree; not shipped (it would change nothing).
- **The exposure through texture locks** (run 54): 0 of 53 locks were of a GPU-produced texture (AC1).

### AC6. Offline tests (VERIFICATO)

`python tests/deko3d/test_run55.py` (new; it and `dk_run55_probe.cpp` need `git add -f`): **OK**.

| Part | Result |
|---|---|
| POOL | 20,000 copies equal to fresh ones; **0 allocations in 20,000 warm takes and copies** (run 50's pool: >= 1 each); two threads: 60,000 copies checked by the "recorder", 0 bytes wrong |
| RULES | the depth-only truth table (256 cases) and the depth bias against the Vulkan path's formula bit for bit (100,000 random pairs): 0 errors |
| FLAGS | synthetic DKSH 7 of 7; run 55 package validated, flags valid on 1,276 of 1,276 pixel shaders, 0 KillsPixels without a discard in the text, 0 depth writes without gl_FragDepth, 0 bytes changed outside the flags words |
| PROBE54 | run 54's console probe: 4 of 4 vertices equal (run 54's tool could not run it) |
| PROBE2 | version 2 round trip; positions equal; one moved vertex found; strip triangles; PNG written |
| INSTRUMENT | the instrumentation with every position on the 12 skinned shaders: 12 of 12 compiled by uam |
| earlier runs | test_run54 -> 53 -> 52 -> 51 -> 50 -> 49: OK |

Not testable offline: the GPU time of the depth-only draws and of the bias, the console's probe of a character.

### AC7. Build (VERIFICATO)

`out/console-runs/run55-build/` (`-j3`, `run55-build.log`, 0 errors; `SHA256SUMS.txt`):

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,200,453 | `6b742d9f2105722b039588db60f8dcb427462cd612f64c2f0046fc88171d03d3` |
| `switch-game-dk-debug.elf` | 322,143,944 | `28a3c39c167ff57022980cfb7b11203340d410fad6f20cd759ccc670e7771eda` |
| `game.toml` | 1,351 | `63816f345d1d0f8fc96aa12b469b94cab9285ad7e27da317c288f3017d776526` |
| `deko3d/gta4_deko3d_runtime.bin` (s55) | 92,679,424 | `7f84b056652128631752d5c984e1aff486178ca2b730777c80df103c807e6ab9` |
| `deko3d/gta4_deko3d_glsl.bin` (s47's) | 41,365,921 | `02ef7d59373746bea6e45f6b13f6ee753c0b5e748673363a4cf879619a919ea9` |

### AC8. Run 55: what to do, what to bring back

Standard clocks (no clock tool profile). Empty `captures/` and `probe/`. Copy the NRO, game.toml and **the whole
`deko3d/`** (the runtime package changed). The intro cut-scene to its end, then the first mission on foot near people,
one minute at least; one stretch by day and one at night if possible. Bring back game.log, game-crash.log, captures/ and
probe/ into `out/console-runs/run55-logs/`.
- `clocks read:` CPU 1020: the run is at the target's clocks.
- `run 55 draws in the interval`: depth-only draws and biased draws > 0.
- `GPU time from timestamps` and `run 55 costliest single pass per target`: the shadow maps' and the lights' time against
  run 54 (AC2).
- `threads in the last …` and `run 55 copy pool`: the front end's time and lock wait against run 54.
- `run 54 skin probe written` (up to 4): then `python tools/deko3d/probe_compare.py <file>.probe` and look at the PNG.
- Off switches if a picture breaks: `nx_dk_depth_only = false`, `nx_dk_depth_bias = false`.

Not solved, not claimed: characters, lighting, sky, 720p/30 fps.

## 2026-11-05: run 55 read and run 56 (AD): the first run at the target clocks, the characters' vertices proven, the CPU's waits named, the vertex outputs pruned

### AD1. Run 55 read (`out/console-runs/run55-logs/`, 92 captures, 4 probes)

| | |
|---|---|
| Run | 852 s, ended with `+`, 0 faults. **Clocks CPU 1020 / GPU 460.8 / memory 1331.2 (the target's) in every heartbeat** after start-up (the start-up line still read 1785 / 1600, then the standard ones held): the first run since 50 at the target's clocks. |
| GPU probe | **4 meshes of real characters**, the first ones the probe ever read: a head (2,943 vertices, 5,534 triangles), a torso (3,463 / 4,365), a shadow-map skinned draw (2,811 / 4,999, 2560x2560, no pixel shader) and a 420-vertex mesh crossing the near plane. **Every position of every vertex equal to the PC's** (9,637 vertices). The only differing values: lanes of SPIR-V temporaries the program never writes (`_465.w`, `_486.w`, `_672.yzw`), **NaN on the console, 0 in the PC's interpreter**, and never read in these four shaders. The meshes drawn from the GPU's positions and indices, fitted to their own bounds, are whole (a head with ears and eyes, a jacket): **the characters' vertex shaders execute right; the fault is after the vertex shader.** |
| Pictures | Close-up faces with black triangular holes and skin-coloured triangles floating beside them; in the cut-scene the bodies are **missing and the sky and wall behind them show through**, with shards of clothing; night: buildings with holes, sky through them; day: washed out to cyan/white. |
| Speed | play windows (draws/frame > 1,500): **4.6 fps median (3.1-7.9) at 3,854 draws/frame, 17.7 k draws/s**; GPU busy 5.55 s of 10 (idle 4.4); title render thread (`id 12`) in `SubmitTitleCommand` 7.8 s of 10 (front end 5.3, front-end lock 1.2, queue 1.1); recorder 22.2 µs/draw, busy 5.0 s of 10. |
| Run 55's changes | depth-only draws **153 in the whole session**, depth bias **0** (GTA IV's shadow maps bind no pixel shader at all; the title sets no bias); the copy pool at its 192 MiB budget all the time. |

Run 55 against 54 (medians of the play windows; *one sample each: no difference is proven*): 4.6 against 5.3 fps, 17.7 k
against 22.4 k draws/s, recorder 22.2 against 15.0 µs/draw: **run 55 ran at CPU 1020, run 54 at 1785** (x0.57); the
ratio of draws/s (0.79) is better than the clock ratio, the recorder's µs/draw (x1.48) close to it. Not comparable as a
code change; **run 55 is the new baseline at the target's clocks.**

GPU per frame (play window t = 400 s, 35 frames, VERIFIED from the log): lights `40076EB0/400007C0` 36 ms, G-buffer
`402267B0/40218920` 34 ms, shadow maps 2560x2560 26.5 ms (1,833 draws a frame), a 320x180 pass 17.8 ms (~1,000 draws a
frame), the shadow map's resolve 8.4 ms (a 52 MB ZF32_X24S8 copy on the copy engine), 128x128 5.8 ms: ~160 ms of GPU a
frame. **30 fps needs ~5x on the GPU and ~6.5x on the CPU at these clocks.** The vertex-heavy passes (shadow maps, the
320x180 one) are where the vertex shaders' cost is.

### AD2. Where the CPU waits (the profiler's whole stacks, new `where.py --stacks`)

nx_profiler has logged the 5 most frequent whole stacks of each thread every 10 s since C2 (sa2-nx's switch_perf), but
`where.py` only added up pc and lr, which stop in libnx's `mutexLock`. Run 56's `--stacks` adds the stacks up and names,
for every wait, the first frame that is not a lock or wait primitive (run 55, whole session):

| Thread | Share of its samples | Where |
|---|---|---|
| main (`id 6`) | **40.4 %** | `std::mutex::lock` in **`XSemaphore::ReleaseSemaphore`** (`NtReleaseSemaphore`: the job dispatcher) |
| main | 10.7 % | `XObject::Wait`'s mutex (`KeWaitForSingleObject`) |
| render (`id 12`) | 30.7 % | condition wait in `SubmitTitleCommand` (the packet ring full: `full 110 times`) |
| render | 16.5 % | a mutex in `SubmitTitleCommand` (the front-end lock: 1.2 s of 10 measured) |
| render | **~23 %** | newlib's malloc: its lock (`rmutexLock` <- `_memalign_r`) and `_malloc_r`, **under run 55's pool's vector growth** (`std::__uninitialized_copy_a<move_iterator>`: the old bytes moved into the new storage) |
| render | 14.5 % | `memcpy` in `CopyDrawData` (the copies at submit: **6.45 M draws, 85 GB in the session, ~13 KB a draw**; 17,048 buffers caught changing by the recorder) |
| jobs (`id 9-11`) | 6 % + 3 % | `XObject::Wait`'s mutex, `XEvent::Set`'s mutex |
| recorder | 9.5 % | `svcArbitrateLock` (malloc's and the ring's); 6 % XXH3 (the geometry store) |

C5 (lock-free release, on in run 55) adds the count without the mutex, but its `WakeSleepers` still takes the object's
mutex whenever a thread sleeps on it, only to make the condition variable's notify safe; the woken job thread holds the
same mutex when it wakes, so the main thread blocks on it.

### AD3. What run 56 changes (tested offline as AD5 says; none run on the console yet)

1. **Copies at submit in one arena** (`native_draw_snapshot.h` `DrawCopyArena`, `DrawDataPool::TakeCopy`). One 128 MiB
   block used as a ring; a copy takes its next bytes, a span is free again once its entry left the recorder or was taken
   for another copy (a generation per entry); spans given back oldest first (packets are consumed in order). No room: the
   entry's own vector, as through run 55, counted. The vector path no longer moves old bytes when it grows. Line:
   `run 55 copy pool ... run 56 arena: N copies in it, N without room, peak N of 128 MiB`. Expected (ESTIMATE): the render
   thread's ~23 % in malloc and part of the recorder's lock waits gone.
2. **Single-object waits on Horizon's address arbiter** (`rex/thread/nx_futex.h`, `nx_multi_wait.h`, cvar
   `nx_sync_futex`, default on). Waiters sleep with `svcWaitForAddress(WaitIfEqual)` on a per-object epoch; a lock-free
   release moves the epoch and calls `svcSignalToAddress` only if someone sleeps, and takes the object's mutex only when
   a multi-object waiter is registered (their path is unchanged). Consumers still take the count under the mutex (no
   underflow). Expected (ESTIMATE): the main thread's 40 % in `ReleaseSemaphore` gone; whether that raises the frame rate
   depends on the render thread, which is the limit (the main thread's CPU is 14 % of a core).
3. **Vertex shaders that write only what is read** (masseffect-nx S6, banjo-nx's output gate). Every vertex shader of
   the package declares and writes 18 vec4 outputs, **9 to 18 of them the constant zero** (banjo-nx's
   `// banjo-vszero` mask; 211 of the 636 shaders have 16 zero outputs); each written output is stored per vertex and set
   up per primitive. `tools/deko3d/gta4_deko3d_vs_prune.py` compiles, with the PC's uam, two variants of every vertex
   shader variant through the gate constant `SPIRV_CROSS_CONSTANT_ID_905` the package left at all ones: **position
   only** (no generic output) and **live outputs** (only the non-zero ones), and checks each DKSH's program header
   (VTG `omap_generic_vector`): the full variant writes all 18, position-only 0, live none outside its mask: **636 of 636
   pass both**. The package records the zero mask (vertex) and the declared input locations (pixel) in the record's
   last word (flag bit 3; `gta4_deko3d_runtime_check.py` and `dk_shader_package.cpp` accept it only with the flag), and
   the variants under value | bit 31 / bit 30. The recorder (`nx_dk_vs_prune`, default on) takes position-only for every
   draw drawn without a pixel shader (the shadow maps: ~1,800 draws a frame) and live outputs when the pixel shader
   declares none of the vertex shader's zero outputs; never compiled on the console. Line: `run 56 vertex output
   pruning`. Expected (ESTIMATE): shadow-map and vertex-bound passes cheaper; masseffect-nx measured 66 -> 36.6 ms.
4. **Vertex shader locals given a value** (`tools/deko3d/gta4_deko3d_zero_init.py`). The probe's only GPU/PC
   differences were never-written lanes, NaN on the console and 0 on the PC. Where such a lane reaches a result the
   console differs from the PC; the text cannot say where (lanes move through swizzles and the legacy multiply's zero
   guard), so uam was asked: compiled as they are and with every `vec4 _n;` set to zero, **0 of ~98 pixel shaders and 14
   of ~22 vertex shaders** of a sample compiled differently (some only because both branches of an `if` assign, which
   the compiler does not see). Run 56 ships the vertex shaders zero-initialised (**342 of 636 vertex variants change
   code, the other 294 are byte-identical; no pixel shader is changed**). Defined where the PC already reads 0; a
   hypothesis for part of the shards, NOT proven.
5. **The frame's anatomy** (`dk_anatomy.h`, `DkRecorder::AnatomyCapture`, cvar `nx_dk_anatomy`). The frame after the
   1st, 7th, 13th ... capture (every 60 s with `nx_dk_capture_s = 10`, 8 at most) saves **every resolve's destination**
   (G-buffer targets, the resolved scene depth, shadow maps, the luminance and post-processing targets: what the title
   samples next) and the shown image, as `captures/dk-f<frame>-a<n>-<kind>-<source>-<destination>.png`, each logged with
   per-channel min/max/mean and its NaN and Inf texel counts (NaN/Inf shown magenta; float formats scaled to their 99th
   percentile; depth stretched over its range, stencil in red). That frame waits for the GPU at each resolve and is
   logged `(not a benchmark frame)`. This answers "first target where the picture diverges" for the characters' holes,
   the shards, the sky and the exposure, which the shown frame alone could not.
6. **Profiling separate from the benchmark in one session**: `nx_profile_start_s` (run 56's toml: 420 s): the sampling
   thread sleeps until then and logs `profile: sampling starts now`; the windows before it are the benchmark. Stacks per
   thread 5 -> 12 (`nx_profiler.cpp`); `where.py --stacks`.

Not done, and why (each measured above): the packet ring's futex wake (its mutex is taken only when the recorder sleeps;
the render thread's 30.7 % there is the ring being full, i.e. the recorder, not the lock); resolves drawn instead of the
copy engine (8.4 ms a frame for the shadow map's: next); the double copy (front end then recorder) of every draw's
vertices (needs the front end to write the GPU's format into the upload ring); reNut's store cards (need the codegen's
stores to mark pages); VS output pruning per (vertex, pixel) pair (needs the pairs: the run 56 log counts the draws kept
whole for that reason).

### AD4. The other ports (reading of this session)

The repositories named for run 56 were not fetched again in this session; the techniques taken come from the code and
documents already read for runs 50-55 (table AC3) and from what run 55's measurements pointed at:

| Technique | Origin | Here | Status |
|---|---|---|---|
| vertex outputs nobody reads; zero for depth only (S6) | masseffect-nx `optimization-paths.md`; banjo-nx's `g_banjoVsOutputs` gate (already in our GLSL) | `gta4_deko3d_vs_prune.py`, `nx_dk_vs_prune` | done (AD3.3) |
| sampling profiler with whole stacks (`perfil_pilas`-like) | sa2-nx `switch_perf.cpp` (our nx_profiler since C2) | `where.py --stacks`, 12 stacks, `nx_profile_start_s` | done (AD3.6) |
| no allocation per draw, one ring for transient data | sa2-nx guide §7, masseffect-nx `kernel-waits.md`, nfsmw-nx upload ring | `DrawCopyArena` | done (AD3.1) |
| futex-style waits instead of condvar + mutex | Horizon's address arbiter (libnx `svcWaitForAddress`) | `nx_futex.h` | done (AD3.2) |
| resolves drawn, not on the copy engine | banjo-nx `banjo_renut_tuning.cpp` | 8.4 ms a frame measured for one resolve | next |
| store cards (4 KB write tracking) | reNut (sa2-nx) | needs codegen stores | not started |
| geometry store, chain reuse of state | nfsmw-nx (run 52), reNut | store in since run 52; state reuse: `e_` (emitted state) | in, unchanged |

### AD5. Offline tests

`python tests/deko3d/test_run56.py` (new; it, `dk_run56_probe.cpp`, `gta4_deko3d_vs_prune.py` and
`gta4_deko3d_zero_init.py` need `git add -f`): first full run **2 failures, both in the tests, fixed; the re-run was stopped by the PC's low memory and is
NOT done**.

| Part | Result |
|---|---|
| ARENA | ring alone: 180,661 spans with random sizes, **0 overlaps of live spans**; pool + recorder thread: **60,000 copies, 60,000 checked at "record" time, 0 wrong** although the source was rewritten after each submit, 60,000 in the arena, 0 without room. Failure: 8 `operator new` in the warm half = new pool entries made while more copies were in flight (the pool grows to 8,192 entries by design), not per-draw allocations; the check now counts only takes that reused an entry (a standalone run before the change had 0). Re-run with the refined check: NOT done |
| FUTEX | futex mode and condition-variable mode: 80,000 lock-free releases taken by 3 consumers (0 left), 20,000 event round trips (0 lost), timed wait times out, multi-object + single-object waiters 20,000 of 20,000: **0 errors in both** |
| ANATOMY | half floats, NaN marking, depth/stencil, RGBA8: 0 errors |
| PACKAGE | s56 validated; io masks on 636 of 636 vertex and 1,276 of 1,276 pixel shaders; **1,272 of 1,272 pruned variants checked in their program headers**; C++ reader opens the real package (1,912 shaders, 6,621 DKSH, 64 of 64 read back). Failure: test_runtime_package.py's fixture `reserved_not_zero` set flags = 1, a valid flag since run 55 (kFlagKills): the fixture was stale since run 55; now an unknown bit (1 << 4), plus `io_mask_with_flag` (must open). Re-run: NOT done |
| STACKS | where.py --stacks on run 55 names the main thread's first wait: 40.4 % `XSemaphore::ReleaseSemaphore` |
| earlier runs | test_packet_ring.py OK (2,000,000 packets, 0 errors, 0 steady allocations); **test_run55.py -> 54 -> 53 -> 52 -> 51 -> 50 -> 49: OK** |

### AD6. Build

`out/console-runs/run56-build/` (`tools/switch-game/build_variants.sh ... dk-debug`, `-j3`, log `run56-build.log`, 0
errors; tree `17f3a18-dirty`, not committed). The ELF holds the new code (`DkRecorder::PrunedShaderFor`,
`AnatomyCapture`, `DrawDataPool::TakeCopy`, `svcWaitForAddress`/`svcSignalToAddress` linked: the futex is the Switch
path, not the PC tests' fallback). `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,233,221 | `ae58db7e4cfd77f04e31be398117ea8f8c4965882fbaed85de9cdaa6ca6186a1` |
| `switch-game-dk-debug.elf` | 322,552,624 | `f453ddb6f459bdf89764b206496c69a7a0e3f99283a9f034ea1c6ea59e3f2c66` |
| `game.toml` | 1,807 | `7a0b84a07fbdea7cc1230fc499b09366903ff6041d137fc9af453eb56c49da34` |
| `deko3d/gta4_deko3d_runtime.bin` (s56) | 95,650,048 | `936453835ed7cebd04a9354aa9dc12fdf50f7180a05b4b2e6f51e65b08f2547f` |
| `deko3d/gta4_deko3d_glsl.bin` (s56) | 41,538,489 | `6041580fc4eddc73ac237c90fb492abcbbcaf218ef13cb9ceb08afb0508e3d57` |

Package `C:/dev/Theft4/eu-private/deko3d-out-s56` (1,912 shaders, 13,392 variants of which 1,272 pruned, 0 without
DKSH, 6,621 DKSH); `deko3d-out-s56-nozero` is the same without the zero-initialised vertex shaders (fallback).

### AD7. Run 56: what to do, what to bring back

Standard clocks (no sys-clk / Horizon OC profile). Empty `sdmc:/switch/theft4/captures/` and `probe/`. Copy the NRO,
`game.toml` and **the whole `deko3d/`** of `run56-build/` (the runtime and GLSL packages changed). Play: the intro
cut-scene to its end, then the first mission on foot near people, by day and if possible at night, **at least 12 minutes
in all** (the first 7 are the benchmark, the profile starts at 420 s). Exit with `+` held 1 s. Bring back game.log,
game-crash.log, game-exit.log and the whole `captures/` into `out/console-runs/run56-logs/`.

Read first:
- `clocks:` CPU 1020 / GPU 460.8 / memory 1331.2 in the heartbeats;
- `run 56 single-object waits on the address arbiter: 1`, `run 56 vertex output pruning` (position-only draws > 0),
  `run 56 arena` (copies in it > 0, without room ~0);
- the play windows before 420 s against run 55 (AD1): fps, draws/s, `threads in the last`, `GPU time from timestamps`;
- `run 56 anatomy` lines and their PNGs (`-a<n>-`): which target first shows the holes, the shards, the sky, NaN counts;
- after 420 s: `python tools/switch-game/where.py --stacks --elf out/console-runs/run56-build/switch-game-dk-debug.elf
  out/console-runs/run56-logs/game.log`.

Off switches if something breaks: `nx_sync_futex = false` (a hang or a stutter in waits), `nx_dk_vs_prune = false`
(missing or wrong geometry in shadows), `nx_dk_anatomy = false`; the run 55 shader package is `run55-build/deko3d/`.

Not solved, not claimed: characters, lighting, sky, 720p/30 fps. Expected only as estimates: less CPU in malloc and
in the semaphore mutex, cheaper shadow maps.

## 2026-11-06: run 56 read and run 57 (AE): the exposure chain resolves surfaces nothing draws into, EDRAM placements ported, the CPU gains measured

### AE1. Run 56 read (`out/console-runs/run56-logs/`, 364 captures, 8 anatomy frames)

| | |
|---|---|
| Run | 1,308 s at **CPU 1020 / GPU 460.8 / memory 1331.2** in every heartbeat. Ended by a **fault at exit** (after `+`): `Core::Destroy` -> `DestroyBlock` on the anatomy read-back block, which `AnatomyCapture` had already destroyed when it grew without telling the core (run 56's own bug; no fault during play). Fixed (AE3.4). |
| Run 56's changes | futex waits on (`... address arbiter: 1`); arena 11.4 M copies, **0 without room**, peak 76 of 128 MiB; pruned vertex shaders 5.6 M position-only and 11.6 M live-outputs draws, **0 refused**, 159 variants loaded. |
| Anatomy | 8 frames of 34-36 images. **Each stalled ~5 s** (most of it the two 2560x2560 shadow maps of 26 and 52 MB): reduced in run 57 (AE3.5). |

**Speed (medians of the play windows without anatomy frames; *one sample each: no difference is proven*).** Run 55
was profiled from start to end, run 56 only after 420 s:

| | run 55 (all, profiled) | run 56 (all) | run 56 (unprofiled, < 420 s) |
|---|---|---|---|
| windows | 44 | 60 | 17 |
| fps (min) | 4.60 (3.1) | 5.00 (3.0) | 5.50 (4.2) |
| draws/frame | 3,854 | 4,032 | 3,679 |
| **draws/s** | 17,722 | **19,661 (+11 %)** | 21,278 (+20 %) |
| **front end µs/draw** | 28.8 | **24.1 (-16 %)** | 23.0 |
| recorder µs/draw | 22.2 | 23.0 | 25.5 |
| GPU busy ms/10 s | 5,550 | 5,246 | 7,373 |

Stacks (`where.py --stacks`, the profiled part): **the main thread's 40 % in the semaphore mutex is gone** (29 samples left
in `ReleaseSemaphore`, all `svcSignalToAddress`); its waits are futex waits for the job threads. **The render thread's
malloc fell from ~23 % to ~5 %.** The render thread now waits 61 % of its samples in `SubmitTitleCommand` for ring space
(the recorder), and the recorder 30 % in `Core::BeginWork` for a work slot's fence (the GPU): **the limit moved to the
GPU** (74 % busy in the unprofiled windows). Vertex output pruning on the shadow maps: 13.4 -> 12.6 µs/draw (-6 %):
that pass is bound by rasterization and depth bandwidth, not by vertices. Audio 55 % of real time (the guest mixer,
thread 16, 93 % of a core: its own DSP code; not addressed in run 57).

**The first divergence, found in the anatomy frames.**
- *Exposure.* In every anatomy frame the HDR scene (`40076EB0`, RGBA16F, R up to 32, mean ~1) is alive, but **the
  640x360 surface `402268C0` and the whole luminance chain after it (320x180 ... 1x1) are exactly 0**. The log says why:
  **each of these 16 surfaces is created by its first resolve** (its `render target` line is followed at once by its
  `resolved texture` line, `test_run57.py EVIDENCE`): no draw ever binds them. On Xenos a render target is a range of
  EDRAM; GTA IV resolves the EDRAM the HDR scene was drawn into through these other surface objects (a 640x360 4X view
  of a 1280x720 1X surface is the same sample space: its resolve is a free 2x2 average). This recorder kept one image
  per surface object, so it resolved freshly cleared images: the measured luminance is 0, and an exposure that adapts to a scene it
  measures as black is consistent with the washed-out day (how the title clamps it at night is not known here). The Vulkan path keeps EDRAM placements (`FindPlacementOwner`,
  `ClaimSurfaceContent`, `RecordSurfaceMaterialization`, `surface_view.h`'s `GuestPlacementKey`); the deko3d recorder
  never had them.
- *Characters.* The G-buffer itself is already wrong: in the albedo (`402266E0`), the normals (`40226770`), the specular
  target and the resolved depth (`402183D0`) of the same frame **Niko's jacket has the same holes, with straight edges,
  and the wall behind it shows through**; the head is whole. Same coverage in all MRTs and in the depth: whole
  triangles are not rasterized or fail a test (not the pixel shader, not blending). Run 55's probe proved the vertex
  shaders' positions; the cause after them is **not found** in this run (culling convention checked on the probes'
  meshes, stencil and depth translation read again: nothing wrong seen). The anatomy of run 57 shows whether the
  placements change it.

### AE2. What run 57 changes

1. **EDRAM placements** (`deko3d/dk_placement.h`, `DkRecorder::ClaimPlacement` / `PrepareSurfaceContent`, cvar
   `nx_dk_placement`, default on). Ported from the Vulkan path (Liberty Recompiled, kabba536, `graphics_system.cpp` and
   `surface_view.h`): every colour or depth target bound by a draw or a clear becomes the owner of its placement (EDRAM
   base in tiles, sample pitch, sample width and height); a colour surface that is bound or resolved while another
   surface owns its placement first gets that content: an image copy when the views are identical, else a conversion
   pass between the sample layouts (`kPlacementPs`: a 4X view of a 1X surface holds the 2x2 average, a 1X view of a 4X
   one replicates, 2X 1x2), compiled at start by the console's uam; then it becomes the owner. Depth views are not
   materialized (the Vulkan path refuses them too). A full clear needs no content first (the Vulkan path's
   `gta4_native_clear_skips_materialization`). Lines: `run 57 EDRAM placements in the interval` and, once per surface,
   `EDRAM placement of X ... materialized from Y by ...`; a resolve of a surface nothing drew into and no owner holds is
   logged once with its EDRAM view and the owners at the same base (so a near miss is visible).
   **Expected on the console (ESTIMATE):** the anatomy images of `402268C0` and the luminance chain no longer zero, and
   the day no longer washed out.
2. **Draws without colour output keep the colour targets bound** (cvar `nx_dk_keep_targets`, default on): the deferred
   lights' stencil passes (no pixel shader) dropped the colour target, so every light switched targets twice (run 56:
   ~36 light passes and ~20 depth-only passes a frame). Same pixels (every colour write masked). Line: `run 57 target
   binds in the interval`. **Expected (ESTIMATE):** fewer target binds and passes on the lights' targets; GPU time per
   light lower only if the switches cost (not measurable offline).
3. Kept from run 56 (measured above): the copy arena, the futex waits, the pruned vertex shaders.
4. **Run 56's exit fault fixed**: `Core::DestroyReadbackBlock` destroys a read-back block and forgets it; the anatomy
   block goes through it.
5. **Anatomy cheaper**: every 12th capture (2 minutes), 4 frames, no image over 16 MB (the shadow maps).
6. `where.py` passes the addresses to addr2line on stdin (12 stacks a thread were more than a Windows command line).
7. `kPlacementPs` shifts instead of dividing: uam warned that a division by a non-constant integer is emulated in
   floating point "with resulting loss of precision".

Not done: the characters' cause (AE1); the GPU's own load (~134 ms a frame in run 56's unprofiled windows at 5.5 fps:
lights, G-buffer, shadow maps are the work itself, ~4x from 30 fps at 460.8 MHz); audio.

### AE3. Offline tests

`python tests/deko3d/test_run57.py` (new; it and `dk_run57_probe.cpp` need `git add -f`): **OK**.

| Part | Result |
|---|---|
| PLACEMENT | plans: 1X 1280x720 -> 4X 640x360 = 2x2 average, -> 2X 1280x360 = 1x2, 4X -> 1X = 2x2 replication, 2X -> 4X = 2x1, identical views = copy; another EDRAM base, depth views and a size outside the sample space refused; the pass's arithmetic on a 1280x720 image: worst error 0 |
| PASS | `kPlacementPs` compiled by the PC's uam, no warning |
| EVIDENCE | run 56's log: 16 colour surfaces created by their first resolve (402268C0 640x360, 40226900, 40226AA0, 402269B0 320x180, 402269F0 160x90, 40226A30 80x45, 40226B90 40x40, 40226AE0 20x20, 40226B20 10x10, 40226BD0 5x5, 40226CC0 and 40226D50 1x1, four 64x64), 9 of them all-zero in the anatomy |
| PACKAGE | test_runtime_package.py's fixtures: all pass (run 55's stale `reserved_not_zero` fixture corrected in run 56) |

Not run again in this session: `test_run56.py`'s full chain (runs 49-55; it passed in the run 56 session, see AD5; the
code it covers is unchanged in run 57).

### AE4. Build

`out/console-runs/run57-build/` (`build_variants.sh ... dk-debug`, `-j3`, `run57-build.log`, 0 errors; tree
`17f3a18-dirty`, not committed). The ELF holds `DkRecorder::PrepareSurfaceContent`, `ClaimPlacement`,
`Core::DestroyReadbackBlock` and the run 57 lines. `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,249,605 | `5d9770242dd58ecb4ba9cad161e0c803e83ad3d5f671a26ebc4903877756071f` |
| `switch-game-dk-debug.elf` | 322,980,368 | `ac7c516ef5a6cc85d7abd91e513938e00fa7b4edf60a315b6aab18c96d112be4` |
| `game.toml` | 1,613 | `0b848e9fcd6c99b93ea6a13ed8ff31a7653f8f3dce1de207032d31b90b91805b` |
| `deko3d/gta4_deko3d_runtime.bin` (s56, unchanged) | 95,650,048 | `936453835ed7cebd04a9354aa9dc12fdf50f7180a05b4b2e6f51e65b08f2547f` |
| `deko3d/gta4_deko3d_glsl.bin` (s56, unchanged) | 41,538,489 | `6041580fc4eddc73ac237c90fb492abcbbcaf218ef13cb9ceb08afb0508e3d57` |

### AE5. Run 57: what to do, what to bring back

Standard clocks, no sys-clk / Horizon OC profile. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`,
`game.toml` and the whole `deko3d/` of `run57-build/` (the package is run 56's: copying it again does no harm). Play as
run 56: the intro cut-scene to its end, then the first mission on foot, by day and if possible at night, **at least 12
minutes**; `+` held 1 s. Bring back `game.log`, `game-crash.log`, `game-exit.log` and the whole `captures/` into
`out/console-runs/run57-logs/`.

Look at, in this order:
1. **The daytime picture**: no longer washed out to white/cyan (compare with run 56's captures of the same places).
2. `run 57 EDRAM placements`: materialized > 0; `EDRAM placement of 402268C0 ... materialized from 40076EB0 ...`.
3. The anatomy lines `colour 402268C0 -> DA1A63E0` and the 320x180 ... 1x1 chain: **not all zero**.
4. `game-crash.log`: no fault at the exit.
5. The windows before 420 s against run 56's: fps, draws/s, `run 57 target binds`, `GPU time from timestamps`.

Off switches if a picture breaks: `nx_dk_placement = false`, `nx_dk_keep_targets = false`.

Not solved, not claimed: characters, 720p/30 fps. Expected only as estimates: the exposure, fewer target switches.

## 2026-11-06: run 57 read and run 58 (AF): the exposure works in play, the freezes were the anatomy frames, the CPU is the limit, every run 57 feature compared in one session

### AF1. Run 57 read (`out/console-runs/run57-logs/`, 107 captures, 4 anatomy frames)

| | |
|---|---|
| Run | 1,120 s at **CPU 1020 / GPU 460.8 / memory 1331.2** in every heartbeat; ended with `+` (mode D, 9 steps), **0 faults: run 56's exit fault is fixed** (VERIFIED, `game-crash.log`). |
| Placements | 22 placements known in play; ~13 materializations a frame (e.g. 558 in a 10 s window at 4.3 fps), about half image copies, half conversion passes; 0 refused, 0 stale owners. |
| **Exposure** | **The luminance chain is alive: 45 anatomy images of the 640x360 ... 1x1 chain, 0 all zero (run 56: 91 of 105 all zero)** (VERIFIED, `test_run58.py EVIDENCE`). **In play the picture is the first plausible one of this port** (VERIFIED by eye on the captures): a daytime street with its sky and lit façades (`dk-f011282`), a night street with the car readable (`dk-f006951`, `dk-f007609`), the safehouse lit (`dk-f009046`); run 56's captures of the same places are washed out to cyan (`dk-f007506`) or black (`dk-f015220`). In the intro cut-scene some shots right after a camera cut are white (`dk-f004187`, `dk-f004369`) while the next shots are dark and readable (`dk-f004191`, `dk-f004192`): the adaptation starting from the previous shot's luminance at 4-15 fps; whether the title would show them that bright is NOT known (no PC reference of those frames). This is the "less drawn in the intro" of the report: the geometry is there, washed out. |
| Keep targets | the lights' pass went from **84 passes a frame (run 56) to 4**; its GPU time per draw rose from 48 to 53 µs (29.4 ms / 607 draws against 36.1 ms / 684 draws): another scene, no proof either way. |
| **Freezes** | **The three anatomy frames stalled the game 2,908, 2,926 and 2,823 ms** (plus 348 ms at frame 320) (VERIFIED, log lines `the frame stalled`): the freezes of the report. The ordinary captures are asynchronous. |

**Speed (medians of the play windows, draws per frame > 1,500; `scratchpad gpu.py`; one sample each, different scenes):**

| | run 56 | run 57 |
|---|---|---|
| play windows with GPU lines | 65 | 71 |
| fps | 4.90 | 4.60 |
| draws a frame | 4,074 | 4,422 |
| draws/s | 19,234 | **20,298 (+6 %)** |
| GPU busy ms a frame | 115.4 | 132.1 |
| GPU busy share of the time | 52 % | 61 % |
| lights `40076EB0/400007C0` | 29.4 ms, 84 passes, 607 draws | 36.1 ms, 4 passes, 684 draws |
| G-buffer 1280x720 | 23.9 ms, 983 draws | 27.0 ms, 1,191 draws |
| 320x180 pass | 7.3 ms, 302 draws | 17.3 ms, 597 draws |
| shadow map 2560x2560 | 27.7 ms | 29.1 ms |

Run 57 drew 9 % more a frame (the 320x180 pass twice as many draws) and pushed 6 % more draws a second: **no regression of throughput is shown; the lower fps is the heavier scene**. Neither is a gain proven: hence run 58's A/B in one session (AF2.2).

**Where the time goes (the profiler from 420 s, `where.py --profile/--stacks`, VERIFIED):** the GPU was busy 61 % of the play windows' time, so **the CPU is the limit**. The title's render thread (`id 12`) spends 7.6-8.2 s of every 10 in `SubmitTitleCommand` (front end 4.9-5.6 s, front-end lock 1.6-1.7 s, ring 0.8-1.1 s); in its whole stacks **21 % is `memcpy` in `CopyDrawDataTo` (the submit copies), 21 % waiting for the front-end mutex**, 14 % waiting for ring space. **65 % of all draws were copied at submit** (11.9 M of 18.3 M, 160 GB in 1,120 s), because 29,194 buffers were promoted: the recorder found 12 % of the sampled draws changed between submit and record (the null GPU answers the title's fences at once, so the title rewrites buffers the recorder has not read yet). The recorder waits for packets 31-39 % of its time. The job threads (`id 9-11`) block ~11 % each in the host event's mutex (`XEvent::Set`), the main thread 15 % in `WaitMultiple`'s.

**Audio:** 55-61 % of real time in play (100 % on the menus). The guest mixer (`id 16`) is busy all the time on its own DSP code (`vld1q_s64`, `sub_82185E38`, the run 51 NEON loop). The profile's "lr in `FPSCRRegister::disableFlushMode` 21.6 %" was read as FPCR writes and **refuted**: `setcsr` is an inline `msr`, a sample with its *return address* there is in a callee (the call returns into the inlined flush-mode check); the pc samples in it are below the top six. Thread `id 17` spends 18 % in `xeKeKfAcquireSpinLock`'s yield (the mixer holds the lock). Not addressed in run 58: the mixer is CPU-bound, not blocked.

**Characters (not solved):** new facts, VERIFIED from the anatomy frame 6207: in the depth (`402183D0`) and the albedo the **near character is in shards while a far pedestrian is whole**. Seen by eye in the safehouse captures (`dk-f009046` of run 57, `dk-f013986` of run 56): bright triangular shards around the furniture too, so probably **not only skinned meshes** (an observation, not measured). The CPU skin check's samples (4 logged) all put the dominant weight on palette bone 0 (weights and indices both D3DCOLOR, paired byte by byte: the same for any common swizzle, and the same in the Vulkan path). The 135,474 draws with zero-record inputs are the characters' shadow shaders (`AD3E412AF420299A`, `5E7C32C4EF6771C0`) drawn with non-skinned declarations, which the Vulkan path also feeds zeros (`kDefaultVertexBinding`): no divergence. Two suspects can be switched at run time and are in run 58's A/B: the geometry store (ranges trusted for an epoch, index ranges with their stored `lo/hi`) and face culling.

### AF2. What run 58 changes

1. **No anatomy frames** (`nx_dk_anatomy = false` in `game.toml`): the three 2.8-2.9 s freezes of run 57 do not happen. Captures every 20 s (asynchronous) and one per A/B phase. The profiler starts at 600 s (`nx_profile_start_s`): the first 10 minutes are the benchmark.
2. **Run 57's features and the characters' suspects compared in one session** (`deko3d/dk_ab.h`, cvar `nx_dk_ab`, on in run 58's `game.toml`). Once gameplay is on screen (two 10 s windows in a row above 1,500 draws a frame), the recorder switches one thing at a time for three gameplay windows (the first settles and is not counted; cut-scenes and loading screens neither move nor count):
   `baseline` (as configured) -> `keep targets off` -> `placement off` -> `store off` (every draw's vertices and indices through the upload ring) -> `copies off` (the front end copies nothing at submit: `NativeFrontEnd::SetSubmitCopies`) -> `cull off` -> `baseline` ... (3 minutes a cycle).
   At each phase's end the log line `run 58 A/B phase "<name>" (cycle N, W windows counted, all cycles): F fps (±P % against the baseline's B), draws/s, draws a frame, GPU busy/idle ms a frame, EDRAM materializations a frame, target binds a frame`; a capture is taken in each phase's last window (`run 58 A/B: capture of phase ... at frame F`, then `capture ... dk-fNNNNNN.png`). When placement comes back on, its owners are forgotten (nothing was claimed while it was off).
   What each phase answers: *keep targets off / placement off*: what run 57's two features cost or save in the same scene (placement off also shows the run 56 exposure next to the run 57 one); *store off*: whether the geometry store (run 52-53) is behind the shards (the shards gone = the store; the CPU cost of the ring instead); *copies off*: what the submit copies cost the title's thread (21 % of its samples) and whether dropping them changes the picture (run 49: 8.8 % changed draws but the same pictures); *cull off*: whether the holes are culled triangles (holes filled = a winding/culling divergence; shards unchanged = wrong vertices).
3. Kept unchanged from run 57: the placements (the exposure in play), keep targets, the futex waits, the copy arena, the pruned vertex shaders, the exit fix.

Not done in run 58, with the reason: a fix for the characters (no cause proven; the A/B tests the two run-time suspects); the audio (CPU-bound in the title's DSP); the submit copies' cost (the A/B measures it first: the copies exist because of a real race, see AF1).

### AF3. Offline tests

`python tests/deko3d/test_run58.py` (new; it and `dk_run58_probe.cpp` need `git add -f`):

| Part | Result |
|---|---|
| AB | `dk_ab.h` compiled by clang with `-Wall -Werror` and driven: no start on loading screens or cut-scenes, start after two gameplay windows in a row, a cut-scene resets the streak and does not move a running phase, the settling window not counted, the capture asked for the last window, the six phases in order, the cycle counted, totals and fps right: 24 checks OK |
| EVIDENCE | run 57's anatomy stalls `[348, 2908, 2926, 2823]` ms; luminance chain run 56 105 images (91 all zero), run 57 45 (0 all zero); run 57 GPU busy median 61 % of the play windows, 65 % of the draws copied at submit |
| CONFIG | `run58-build/game.toml`: `nx_dk_anatomy = false`, `nx_dk_ab = true`, `nx_gpu = "deko3d"`, captures on |

### AF4. Build

`out/console-runs/run58-build/` (`build_variants.sh ... dk-debug`, `-j3`, `run58-build.log`, 0 errors; tree `17f3a18-dirty`,
not committed). Rebuilt: `native_front_end.o`, `dk_graphics_system.o`, `dk_recorder.o`, `main.o`. The ELF holds
`DkRecorder::StepAb`, the `run 58 A/B phase` lines and the `nx_dk_ab` cvar (checked with `nm` and `grep`).
`SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,261,893 | `e4c4fd0fd9d24fb9bc1b82ef30ef49f6750d27d057bdee0ae9f1643ea5e5bfe6` |
| `switch-game-dk-debug.elf` | 323,115,760 | `0c1e74db064b0609f0a55e40edb681e886ab12eb4fc0c1a829dc23b60277a1f6` |
| `game.toml` | 1,406 | `986b4737fd2bf9fdc34f60ab9fbbecee01cf861b7c710f1f6317e50448d8e01d` |
| `deko3d/gta4_deko3d_runtime.bin` (s56, unchanged) | 95,650,048 | `936453835ed7cebd04a9354aa9dc12fdf50f7180a05b4b2e6f51e65b08f2547f` |
| `deko3d/gta4_deko3d_glsl.bin` (s56, unchanged) | 41,538,489 | `6041580fc4eddc73ac237c90fb492abcbbcaf218ef13cb9ceb08afb0508e3d57` |

### AF5. Run 58: what to do, what to bring back

Standard clocks, no sys-clk / Horizon OC profile. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`,
`game.toml` and the whole `deko3d/` of `run58-build/` (the package is runs 56-57's). Watch the intro cut-scene to its
end, then play the first mission **for at least 15 minutes of gameplay**, on foot near people and in a car, by day and
if possible at night; stay in gameplay as much as possible (the A/B phases only move during gameplay: one cycle of six
phases takes 3 minutes of it). `+` held 1 s. Bring back `game.log`, `game-crash.log`, `game-exit.log` and the whole
`captures/` into `out/console-runs/run58-logs/`.

Look at, in this order:
1. **No freezes** of seconds (run 57's were the anatomy frames, off now).
2. The lines `run 58 A/B phase "<name>" (cycle N ...)`: each phase's fps against the baseline's, GPU busy/idle ms a
   frame. `copies off` and `store off` say what the copies and the store cost or save on the CPU; `keep targets off` and
   `placement off` what run 57's features do in the same scene.
3. The capture of each phase (`run 58 A/B: capture of phase ... at frame F`, the next `dk-fNNNNNN.png`): **`store off`
   and `cull off` against `baseline`: characters whole or not**; `placement off`: run 56's exposure next to run 57's.
4. Before 600 s (no profiler): fps and draws/s against run 57's windows.

Off switches: `nx_dk_ab = false` (no phases), `nx_dk_placement = false`, `nx_dk_keep_targets = false`.

Not solved, not claimed: the characters (tested, not fixed), the audio (55-61 % of real time, CPU-bound), 720p/30 fps.
Expected only as estimates: no freezes; the A/B ranks run 57's features and the two character suspects.

## 2026-11-06: run 58 read and run 59 (AG): no A/B switch touches the characters, the recorder draws from bytes the title has already rewritten, copies by rewritten page

### AG1. Run 58 read (`out/console-runs/run58-logs/`, 60 captures, 0 faults)

| | |
|---|---|
| Run | 1,021 s at CPU 1020 / GPU 460.8 / memory 1331.2, ended with `+` (mode D), **0 faults**, **no freeze** reported (no anatomy frames). 3 full A/B cycles and part of a fourth. |
| Where the phases ran | the trigger (> 1,500 draws a frame for two windows) fired in the **intro cut-scene** (frame 4,430): most phases are cut-scene shots and the first drive, not the same scene; the phase lines are cumulative over the cycles. fps is confounded by the draws a frame (2,685-6,231 by phase and cycle): **draws/s and GPU time per draw** are the comparable figures. |

**The A/B (last cumulative line of each phase; VERIFIED from the log; the race column from `scratchpad race.py`, window
by window, settling windows included):**

| Phase | windows | fps | draws/s | draws a frame | GPU busy a frame | GPU per draw | target binds a frame | sampled draws changed since submit |
|---|---|---|---|---|---|---|---|---|
| baseline | 8 | 5.33 | 19,788 | 3,716 | 115.5 ms | 31 µs | 33.4 | 17.4 % |
| keep targets off | 8 | 4.46 | 21,855 (+10 %) | 4,895 | 129.6 ms | 26 µs | **238.4** | 8.9 % |
| placement off | 6 | 6.10 | 19,374 (-2 %) | 3,178 | 117.6 ms | 37 µs | 33.0 | 10.2 % |
| store off | 6 | 5.55 | 18,756 (**-5 %**) | 3,378 | 120.9 ms | 36 µs | 34.6 | 10.7 % |
| copies off | 6 | 5.92 | 22,321 (**+13 %**) | 3,771 | 110.9 ms | 29 µs | 33.3 | **61.1 %** |
| cull off | 6 | 4.35 | 20,211 (+2 %) | 4,644 | 139.6 ms | 30 µs | 33.3 | 26.4 % |

Reading (one session, different shots: differences under ~10 % are not proven):
- **Characters: no phase changed them.** Baseline `dk-f006600` (two near characters in shards, two far workers whole),
  keep targets off `dk-f006749`, placement off `dk-f005943`, store off `dk-f006143` (Niko's face in the car with holes)
  and `dk-f010803`, copies off `dk-f006324`, cull off `dk-f006461`: the same shards and holes. The geometry store and
  face culling are **refuted** as the cause (VERIFIED by eye on the captures).
- **Culling off is worse**: large dark quads over the scene (`dk-f007402`): back faces the title culls are not hidden by
  depth (planes, volumes). The title's culling stays.
- **The geometry store saves CPU** (store off: -5 % draws/s); **the copies at submit cost CPU** (copies off: +13 % draws/s:
  the render thread's `memcpy` and `CopyDrawDataTo`, 12 % of its samples in the profile from 600 s).
- **Keep targets**: 7x fewer target binds (33 against 238-341 a frame); no throughput cost shown (keep targets off had
  more draws/s and less GPU per draw, in heavier shots). Kept; whether the bound-but-masked targets cost GPU time is open.
- **Placement**: ~2 % of draws/s for the exposure chain (run 57); kept.
- Text: letters missing in the HUD (`Rom n s p ce`, `dk-f006989`; `OMAN'S T X`, `dk-f006886`; `ra ar`, `dk-f007402`) and a
  diagonal cut through the credits (`dk-f006461`): whole primitives of 2D draws wrong, in every phase.

**The race (new, VERIFIED from the totals lines window by window):** the front end hashes 1 draw in 32 at submit and the
recorder hashes it again when it records it. **With the copies on, 9-17 % of those draws (13.8 % over the session) were
recorded from bytes the title had changed after submitting them; with the copies off, 61 %.** The front end promoted
4,685-6,336 buffer objects *in every phase* (33,650 handles at the end): the promotions never converge, because the title
draws from buffer objects that are new (the logged ones are objects at `0xCF8F....`, in physical memory) in memory it
rewrites while the recorder is up to a frame behind (queue depth up to 2,575 packets). A promotion by handle comes too late
for the next frame's object. The Vulkan path is immune: it snapshots every buffer at submit (`CaptureBufferResource`, with
a shadow comparison).

**The characters' chain, by elimination (each link VERIFIED in this or an earlier run):** constants: 517,871 sampled banks
equal to the front end's at submit, 0 different (bone palette included); the vertex shaders' arithmetic: equal to the
GLSL on the inputs the GPU was given (run 55's probe, 9,637 vertices); the formats, swizzles, fix-ups, index conversion,
base vertex and draw call: the same as `graphics_system.cpp` (read side by side in this session:
`ConvertVertexElementFormat`/`GetCompatibleVertexFormat`/`ConvertGuestVertexPayload` against `VertexAttribute`/
`ApplyVertexFixes`, `RecordIndexedPrimitive` against `Draw`); the store and culling: refuted above. The G-buffer itself
shows **slivers and floating triangles**, not only holes (`run57-logs` anatomy `dk-f006207-a11`, enlarged): vertices placed
wrong, not pixels rejected. What is left between the title and the vertex shader is **the bytes the vertex fetch reads**,
and those are measured wrong for a sixth of the sampled draws. The head of run 55's probe (`EF7ED6F38B1F9211`) reads a
second stream (POSITION1/NORMAL1/TANGENT1, stride 48, deltas of ±1 mm: per-frame blend-shape data) that the far LOD
shaders do not have, which fits "near characters broken, far ones whole". **Not proven that the characters' draws are
among the changed ones** (run 58 logged only the first 16 changed draws, shadow-map draws at start-up): run 59 logs them.

**Audio (profile from 600 s, `where.py --profile`, VERIFIED):** the guest mixer thread (`id 16`) waits in only ~6 % of its
samples (`svcWaitForAddress` 4.4 %, sleep 1.9 %): it is busy all the time in the title's own mixing code, recompiled
(`vld1q_s64`/`vst1q_u8`/`vld1q_u8`: the 128-bit guest vector loads and stores of that code, `sub_82185E38` the top
caller); the XMA decoder (`id 2`) waits 77.5 % (decoding is not the limit); thread `id 17` sleeps or yields 45 % (the
mixer holds the spin lock). **The audio is slow because one core cannot run the title's DSP code in real time at 1020 MHz,
not because of the frame rate** (the mixer does not wait for the renderer). Further gains need its hot guest functions
written natively, as run 51 did for `sub_8216DBB0`; not done in run 59 (graphics first).

**CPU (same profile):** the title's render thread (`id 12`): front-end mutex 12.9 %, `CaptureDraw` 11.4 %, the copies
(`CopyDrawDataTo` + `memcpy`) ~12 %, constant diff 2.8 %, index scan 1.3 %; the recorder waits for packets 34 %: **the
producer (the title's render thread) is the limit**, not the recorder or the GPU (GPU busy ~115 ms of ~190 ms a frame).

### AG2. What run 59 changes

1. **Copies at submit by rewritten page** (`native_draw_snapshot.h` `VolatilePages`, `NativeFrontEnd::CaptureDrawData`,
   `DkRecorder::Draw`). When the recorder finds a sampled draw changed since submit, it marks the 64 KiB pages of the
   bytes that draw read (every stream's range and the indices, in the view the draw reads them through: physical through
   a fetch constant, virtual through a pointer). The front end copies at submit every draw whose range touches a marked
   page, whatever its buffer object: a buffer's whole data is tested first (a word of 64 pages at a time), the exact range
   once the indices are scanned. Pages stay marked. The handle promotions of run 49 are kept (`dynamic_buffers_` for
   rewritten-after-fill, `promoted_buffers_` for the recorder's, apart).
2. **Which part changed, and whose draw** (`DrawDataPartHashes`): at submit the sampled draws also hash the indices and
   streams 0-2 apart; a changed draw names the parts, and the first 48 with skinning inputs or drawn into the G-buffer
   (three colour targets and a depth) are logged: `run 59 draw N read bytes the title changed after submitting it: vertex
   shader ... (skinned) (G-buffer) ... changed: stream 1 ...`. Totals: `run 59 submit race (totals)` (recorder) and
   `front end, run 59 (totals)` (copies by reason: locked, rewritten after the first fill, promoted buffer, rewritten
   pages).
3. **A/B of the new copies in the same session** (`dk_ab.h`: phases `pages off` = run 58's copies, `pages only` = no copy
   by promoted buffer object; `nx_dk_ab_phases`, a mask; run 59's `game.toml`: 193 = baseline, pages off, pages only,
   90 s a cycle). Each phase line now ends with `sampled draws drawn from bytes changed since submit X of Y (Z %)`.

Not done, with the reason: the audio (above: the title's DSP code; needs native rewrites of its hot functions); the CPU's
front-end mutex and constant diff (the producer's other costs, after the picture); a shader-binary test for the
characters (the package's DKSH against the probe's console-compiled shader): run 59 first tells whether the characters'
draws are in the race; if none of them is, that test is next.

### AG3. Offline tests

`python tests/deko3d/test_run59.py` (new; it and `dk_run59_probe.cpp` need `git add -f`): **OK**.

| Part | Result |
|---|---|
| PROBE | `VolatilePages`: pages, the next/previous page, physical apart from virtual, a 2 GiB range (word at a time), word boundaries, the 4 GiB end, counted once; `DrawDataPartHashes`: a change in stream 1 names stream 1 only, a change outside the range is not the draw's, the indices named; `AbSchedule` with mask 193: baseline, pages off, pages only, cycle 1, the race counters add up; no mask = run 58's six phases. **Race model** (buffer objects new every frame in a ring of 6 slots, recorder a frame behind, 1 draw in 32 checked): promoting handles 23,920 of 47,840 draws recorded from changed bytes; marking pages 240 of 47,840 (the first laps). 31 checks, 0 failures (clang `-Wall -Werror`) |
| EVIDENCE | run 58, window by window: with the copies 13.8 % of the sampled draws drawn from changed bytes (8.9-17.4 % by phase), without 61.1 %; 3,152-6,336 buffer objects promoted per phase |
| CONFIG | `run59-build/game.toml`: anatomy off, A/B on, phases 193, deko3d, captures on |

`python tests/deko3d/test_run58.py`: still **OK** (the schedule without a mask is run 58's).

### AG4. Build

`out/console-runs/run59-build/` (`build_variants.sh ... dk-debug`, `-j3`, `run59-build.log`, 0 errors; tree `17f3a18-dirty`,
not committed). Rebuilt: `native_front_end.o`, `dk_graphics_system.o`, `dk_recorder.o`, `main.o`. The ELF holds
`GlobalVolatilePages`, the `run 59` lines and the `nx_dk_ab_phases` cvar (checked with `nm` and the strings).
`SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,278,277 | `66e07ad97852d9ff3545beae52418ce2e7edec28edea286a3467fa0ccc12e00c` |
| `switch-game-dk-debug.elf` | 323,322,592 | `4789e57615a943dcb1d468375e0a3409302b8c2dbdf33255544bc1be08b53c0f` |
| `game.toml` | 1,475 | `83edcb1a2c7f743781ef14fc950d0d896d5abd79868a976137923c3cb68eb6c2` |
| `deko3d/gta4_deko3d_runtime.bin` (s56, unchanged) | 95,650,048 | `936453835ed7cebd04a9354aa9dc12fdf50f7180a05b4b2e6f51e65b08f2547f` |
| `deko3d/gta4_deko3d_glsl.bin` (s56, unchanged) | 41,538,489 | `6041580fc4eddc73ac237c90fb492abcbbcaf218ef13cb9ceb08afb0508e3d57` |

### AG5. Run 59: what to do, what to bring back

Standard clocks, no sys-clk / Horizon OC profile. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`,
`game.toml` and the whole `deko3d/` of `run59-build/`. Watch the intro cut-scene to its end, then **at least 15 minutes of
gameplay**, on foot near people (close to Roman and passers-by) and in a car; `+` held 1 s. Bring back `game.log`,
`game-crash.log`, `game-exit.log` and the whole `captures/` into `out/console-runs/run59-logs/`.

Look at, in this order:
1. **The characters in the `baseline` captures against the `pages off` ones** (`run 58 A/B: capture of phase ...`): whole
   in baseline and in shards with pages off = the race was the cause.
2. The lines `run 59 draw N read bytes the title changed after submitting it: ... (skinned) (G-buffer) ... changed: ...`:
   **whether the characters' draws are in the race, and which stream** (stream 1 = the blend-shape stream).
3. `run 59 submit race (totals)` and each phase line's `sampled draws drawn from bytes changed since submit`: the
   percentage in baseline should fall well under run 58's 13.8 % once the pages are marked; `pages off` keeps run 58's.
4. `front end, run 59 (totals)`: draws copied for rewritten pages; `pages only` against baseline in draws/s (whether the
   promotions by handle can go: fewer copies).
5. The HUD text: no missing letters.

Off switches: `nx_dk_ab = false` (no phases, copies by page on).

Not solved, not claimed: the characters (the cause is narrowed to the bytes the vertex fetch reads; run 59 shows whether
the race is it), the audio (the title's DSP code on one core), 720p/30 fps. Expected only as estimates: fewer draws drawn
from changed bytes; whole characters if their draws are among them.

## 2026-11-06: run 59 read and run 60 (AH): the race was a hashing artefact, the characters' bytes do not change, the package's vertex shader binary put to the test

### AH1. Run 59 read (`out/console-runs/run59-logs/`, 33 captures, 0 faults)

| | |
|---|---|
| Run | **549 s** (less than the 15 minutes asked) at CPU 1020 / GPU 460.8 / memory 1331.2, `+` exit, **0 faults**. 4 A/B cycles (baseline / pages off / pages only), started in gameplay (frame 3,770). |
| Characters | **still in shards in every phase**: baseline `dk-f004444` (Niko beside the car), `dk-f005236` (Niko walking, daylight), pages off `dk-f004218` (the hot-dog vendor). HUD letters still missing in baseline (`You can _ave _ny v_hicle`, `dk-f003865`). |

**The decisive line (VERIFIED, `run 59 submit race (totals)`):** `changed since submit 4088 of 64909 sampled draws
re-checked; by part: indices 0, stream 0 0, stream 1 0, stream 2 0`. Every one of the 4,088 draws whose submit hash
differed had **its indices and its streams 0-2 byte for byte equal** at submit and at record. The 48 logged ones (19
skinned, the characters' shaders among them: `9A16E1CEAD4ABA0D` torso 3,463 vertices, `A2D8E2F3F87A09AE` head 2,943,
`6A8A2868D092BFD7`, `9023A3B988FC0EB2`, `B773DA7363A9F395`; 29 more G-buffer draws) all use stream 0 only and name no
changed part (`changed:` empty).

**Cause of the false changes (VERIFIED by reading and by `tests/deko3d/test_run60.py`):** `DrawDataHash` hashed the
`DrawVertexRange` struct as 12 bytes, three of them the padding after `bool valid`, which neither side writes; the front
end's range (returned from `IndexedVertexRange`) and the recorder's (an aggregate on its stack) carried different stack
bytes there. The same draw hashed differently. So since run 49:
- the "changed since submit" counts were mostly this artefact: run 58's 9-17 % with the copies on and 61 % with them off
  (the off phase hashes the draws the others copy, and the stacks differ by path) were **not** bytes rewritten under a
  waiting draw; run 58's AG1 reading ("the race") is withdrawn;
- every promotion of a buffer object (33,650 in run 58) and every "rewritten page" (1,700 in run 59) came from it, and
  with them the copies at submit: **run 59 copied 6.4 M of 10.0 M draws (73 GB)** (2.57 M for promoted buffers, 3.82 M for
  pages). These are the render thread's `memcpy`/`CopyDrawDataTo` samples (12-21 % in runs 57-58) and the +13 % draws/s
  of run 58's copies-off phase.

**So, the questions of the request:** the characters' draws *are* among the "changed" ones, but **no stream of theirs
changes** (vertices, indices: 0 differences); the 64 KiB page copies protected nothing because there was nothing to
protect; the A/B shows it: baseline (pages on) 20,467 draws/s against pages off 22,567 (+10 %, the copies saved) with
the same pictures. The submit-to-record path is **refuted** as the characters' cause, and with it every data-path
explanation run 58 had left (constants equal, formats and indices as the Vulkan path, store and culling refuted).

**What is left, and the one observation missing.** Run 55's probe proved that the characters' vertex shader *GLSL*,
compiled by the console's uam **with storage-buffer writes added**, places every vertex where the PC's interpreter does
(9,637 vertices, meshes whole). The draws on screen do not run that shader: they run the **package's DKSH** (the same GLSL
compiled offline by the PC's uam, without the writes; for some draws its pruned-output variant). Nothing has ever compared
that binary's output with the GLSL. The missing observation: **are the characters whole when their draws run the probe's
shader instead of the package's DKSH?** Run 60 measures exactly that (AH2.2), nothing else changes between its two phases.

**A/B of run 59 (cumulative last lines, VERIFIED):**

| Phase | windows | fps | draws/s | draws a frame | GPU busy a frame | sampled draws "changed" |
|---|---|---|---|---|---|---|
| baseline (pages on) | 10 | 4.40 | 20,467 | 4,651 | 127.7 ms | 1.2 % |
| pages off | 8 | 4.48 | 22,567 (+10 %) | 5,041 | 135.6 ms | 7.4 % |
| pages only | 8 | 3.85 | 21,318 (+4 %) | 5,537 | 138.4 ms | 2.4 % |

The render thread: `SubmitTitleCommand` 7.6-7.9 s of every 10 (front end 4.6-4.8 s, front-end lock wait 1.5-1.7 s, queue
1.1-1.5 s); the recorder waits for packets 40-44 %: the title's thread is still the limit.

**Audio (run 59's `audio:` lines and run 58's profile, VERIFIED):** 58-61 % of real time, output dry 21-44 times a 10 s
window, **1 ms** a window waiting for a free output buffer (the output side never blocks). The guest mixer thread (entry
`82169400`) runs at the deadline priority **0x2B**, the highest of the title's threads (verified in `NX scheduling`
lines), and is busy ~94 % of its samples: it is not starved by the renderer's threads, it runs out of CPU in its own
recompiled DSP code (2.2 M NEON voice loops a window plus the rest of the mix). The XMA decoder waits 77.5 %; thread 17's
waits are on the mixer's spin lock. Not the renderer, not the scheduling: the mixing work itself at 1020 MHz. Not changed
in run 60.

### AH2. What run 60 changes

1. **The submit hash fixed** (`native_draw_snapshot.h` `DrawDataHash`): the range by its values (`valid`, first vertex,
   count as three words). Expected (ESTIMATE, from run 59's numbers): the promotions and page marks fall to the draws
   whose bytes really change (possibly none), and with them the copies at submit (run 59: 64 % of the draws): less
   `memcpy` on the title's render thread, the limit. The copy machinery stays for real changes (locked buffers, buffers
   written again after their first fill, real detections).
2. **The characters' vertex shader binary under test** (`dk_ab.h` phase `probe shaders`, bit 256; run 60's
   `nx_dk_ab_phases = 257`: baseline and probe shaders, 60 s a cycle): in that phase every skinned draw whose vertex
   shader has a ready probe is drawn with the probe's shader (the package's GLSL plus the storage-buffer writes, compiled
   by the console's uam; its writes go to a 1 MiB scratch block nobody reads) instead of the package's DKSH. Pixel shader,
   inputs, constants, state, targets: unchanged. `nx_dk_skin_diff = 32` (probes for up to 32 skinned vertex shaders, run 55
   had 4; each also writes its probe file). Lines: `run 60 probe shaders: vertex shader X variant V drawn with its probe's
   shader from draw N on`, `run 60 probe shaders (totals)`, the phase line names the shaders used and the probes ready.
3. **The characters' draw state once per shader** (`run 60 skinned G-buffer draw N vertex shader ...`: depth, stencil,
   cull, user clip plane, alpha test and alpha to mask, depth bias, viewport, colour write), for the step after this one.
4. **The HUD text's rectangle lists named** (`run 60 rectangle list draw N ...`, once per vertex shader and stride): which
   elements the fourth vertex extrapolates (float32) and which it copies from c2: the letters missing in every phase of
   runs 58-59 are whole primitives of these draws.

Not done, with the reason: no shader or pipeline change (no proof against them yet: run 60 is the proof); the front-end
lock (1.5-1.7 s of 10: measured after the copies fall); the audio (above).

### AH3. Offline tests

`python tests/deko3d/test_run60.py` (new; with `dk_run60_probe.cpp`): **OK**.

| Part | Result |
|---|---|
| PROBE | the same range over different padding bytes: runs 49-59's hash differs (the regression reproduced), run 60's is equal; a real byte change inside the range, a change of range and of the indices still seen, a change outside the range not; mask 257 = baseline then probe shaders, cycle 1; no mask = run 58's six phases. 14 checks, clang `-Wall -Werror` |
| EVIDENCE | run 59: 4,088 of 64,909 "changed", 0 with a changed part; 2,574,534 draws copied for promoted buffers and 3,824,799 for pages; the 48 logged draws (19 skinned) name no part |
| CONFIG | `run60-build/game.toml`: anatomy off, A/B on, phases 257, skin probes 32, deko3d, captures on |

Still OK: `test_run59.py`, `test_run58.py`, `test_run56.py` (its chain runs 49-55's tests: snapshot, packet ring, probe
v2), `test_run50.py`, `test_run49.py`.

### AH4. Build

`out/console-runs/run60-build/` (`build_variants.sh ... dk-debug`, `-j3`, `run60-build.log`, 0 errors; tree `17f3a18-dirty`,
not committed). Rebuilt: `native_front_end.o`, `dk_graphics_system.o`, `dk_recorder.o`, `main.o`. The ELF holds the `run 60`
lines and the scratch block's name (strings checked). `game.toml` turns on `nx_dk_diagnostics`: `nx_dk_skin_diff` counts only
in the diagnostic mode (`main.cpp`), which gates nothing else; without it there would be no probe and nothing to swap.
`SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,282,373 | `aade5681ce6d00086517a2a876e0fe5be844683579b0cf111baa3397fc7b9c6a` |
| `switch-game-dk-debug.elf` | 323,482,608 | `dcd001880ff79b3e0758c36f308f2418e7826e11a4e76d3650abe2362ee6a647` |
| `game.toml` | 1,690 | `3db22a90b24ed79c113ee762167d0d6ef97e9746feb18471291a079fb4a15a58` |
| `deko3d/gta4_deko3d_runtime.bin` (s56, unchanged) | 95,650,048 | `936453835ed7cebd04a9354aa9dc12fdf50f7180a05b4b2e6f51e65b08f2547f` |
| `deko3d/gta4_deko3d_glsl.bin` (s56, unchanged) | 41,538,489 | `6041580fc4eddc73ac237c90fb492abcbbcaf218ef13cb9ceb08afb0508e3d57` |

### AH5. Run 60: what to do, what to bring back

Standard clocks, no sys-clk / Horizon OC profile. Empty `sdmc:/switch/theft4/captures/` **and `sdmc:/switch/theft4/probe/`**.
Copy `theft4-dk-debug.nro`, `game.toml` and the whole `deko3d/` of `run60-build/`. The intro cut-scene to its end, then
**at least 15 minutes of gameplay** close to people (Roman, passers-by) and in a car; `+` held 1 s. Bring back `game.log`,
`game-crash.log`, `game-exit.log`, the whole `captures/` and the whole `probe/` into `out/console-runs/run60-logs/`.
The probes compile on the console's uam in the background (6-13 s each, up to 32): the first cycles may swap few shaders;
the phase lines say how many are ready.

Look at, in this order:
1. **The characters in the `probe shaders` captures against the `baseline` ones.** Whole in `probe shaders` = the package's
   compiled vertex shader is the cause (next: which difference, the offline compile or the pruned variant, by the same
   swap one at a time); in shards in both = the vertex shader binary is cleared too, and the next step is after the vertex
   shader (the `run 60 skinned G-buffer draw` states).
2. `run 60 probe shaders (totals)`: draws swapped > 0, probes ready.
3. `run 59 submit race (totals)` and the phase lines: "changed since submit" near 0, by part 0; `front end, run 59 (totals)`:
   copies at submit far below run 59's 6.4 M; draws/s against run 59's 20.5 k.
4. `run 60 rectangle list draw` lines: the HUD text's elements (which are copied instead of extrapolated).

Off switches: `nx_dk_ab = false` (no phases), `nx_dk_diagnostics = false` (no probes).

Not solved, not claimed: the characters (run 60 tests the last untested link before the rasterizer), the HUD letters,
the audio, 720p/30 fps. Expected only as estimates: fewer copies at submit and more draws/s.

## 2026-11-09: run 60 read and run 61 (AI): the game's shaders extracted offline, the package complete, the characters' depth test put to the test

### AI1. Run 60 read (`out/console-runs/run60-logs/`, 127 captures, 25 probes)

| | |
|---|---|
| Run | **2,289 s** at CPU 1020 / GPU 460.8 / memory 1331.2 (`clocks read` in every heartbeat), `+` exit (`exit(0)`), **0 faults**. 11 A/B cycles (baseline / probe shaders). |
| Probe swap (VERIFIED) | `run 60 probe shaders (totals)`: **615,067 skinned draws drawn with their probe's shader**, 7,379 without a ready one; probes started 26, ready 25; **24 vertex shaders swapped**, the characters' among them: `9A16E1CEAD4ABA0D` torso (3,463 vertices), `A2D8E2F3F87A09AE` head (2,816), `EF7ED6F38B1F9211` hair, `6A8A2868D092BFD7`, `9023A3B988FC0EB2`, plus their shadow-map draws (0 colour targets). The swap replaced the **vertex** shader only (pixel shader, inputs, constants, state unchanged), as designed. |
| Pictures (VERIFIED, captures paired by the phase lines) | baseline `dk-f005777`, `dk-f006148`, `dk-f006203` against probe shaders `dk-f006009`, `dk-f006032`, `dk-f006302`: **characters in shards in both phases**, faces with triangular holes, dark shards rising from heads; **scenery has the same shards** (the ship and the crane in `dk-f006148`/`dk-f006203`). |
| Verdict | The probe reached the characters' shaders, so the test is conclusive for what it tested: **the package's compiled vertex shader binary is cleared** (with run 55's GLSL probe: the positions are right and the binary that draws them is not the cause). The fault is after the vertex shader. |
| Draw state of the characters (`run 60 skinned G-buffer draw`, 14 lines) | depth test on, func 6 (GEQUAL), write on; stencil on, func ALWAYS, REPLACE; cull 2; no user clip plane; alpha test only on the hair (func 5, ref 0.392); viewport 1280x720, depth 0-1; colour write FFF. Nothing differs from the scenery's G-buffer draws; the device uses `DkDeviceFlags_DepthZeroToOne` (D3D clip space): no convention error. |
| Case A (missing shader) | **Excluded for run 60**: `drawn 29,022,176 (skipped by reason: none)`, front end `rejected 0` of 29,025,646; `shaders: 401 from the PC's DKSH, 0 compiled by the console's uam, 0 failed`. Every draw found both its shaders in the package. |
| Submit copies (run 60's hash fix, VERIFIED on the console) | `front end, run 59 (totals)`: **31,323 draws copied at submit** in the whole session (run 59: 6.4 M), promoted buffers 0, rewritten pages 0; `run 59 submit race`: 0 changed of the sampled draws in every window. |
| Speed | baseline 5.27 fps, 21,128 draws/s, 4,008 draws a frame, GPU busy 119.5 ms + idle 68.7 ms a frame; probe shaders 5.42 fps, 21,344 draws/s (scenes differ: not a gain). Run 59's baseline: 4.40 fps, 20,467 draws/s. |
| Audio | 58-61 % of real time, output dry 19-28 times a window, 1 ms waiting for a buffer: **as run 59; nothing new, not changed**. |

**Where the time goes (profiler after 600 s, `where.py --stacks`, VERIFIED):** the title's render thread (id 12) is
**73.5 % waiting in a condition variable inside `SubmitTitleCommand` called from the swap hook (`sub_829D5338`)**: the
frame backpressure, waiting for the recorder; the recorder is **54 % in `dk::Fence::wait` inside `Core::BeginWork` <-
`EnsureWork` <- `Draw`/`Resolve`**: one work per frame, 3 slots, so at the start of frame N it waits for frame N-3 on the
GPU. The GPU reports busy 119.5 ms and idle 68.7 ms a frame between its works' stamps; the idle part is outside the
stamped commands (the pre-list's uploads, the barriers' drains: **69 barriers a frame**, most around the **28 resolves a
frame** done on the copy engine with `DkBarrier_Full`). Where the GPU's idle time is spent is not measured yet.

### AI2. The shaders: audit and offline extraction (VERIFIED on the game's files)

**The path until run 60.** The package was translated from the **1,274 containers one PC session of 480 s created**
(`pc_shader_dump_dir`, M0): only what that session drew, the limit the Tekken 6 port's author described. The path:
`tools/pc-eu-base/native_hooks.cpp` (XXH3-64 of each container passed to `D3DDevice_CreateVertexShader/PixelShader`,
virtual + physical size) -> XenosRecomp (`main.cpp` scanner, `shader_recompiler.cpp`) -> DXC -> `gta4_deko3d_structure.py`
-> SPIRV-Cross -> banjo-nx's passes -> uam -> `gta4_deko3d_shaders.py` (DKSH cache) -> `gta4_deko3d_zero_init.py`,
`gta4_deko3d_vs_prune.py` -> `gta4_deko3d_runtime.py` (the card's package) -> console: `RegisterShaderCommand` (the
hash) -> `NativeFrontEnd` (`shader_hashes_`) -> `DkRecorder::Draw` -> `ShaderPackage::Find`; a hash not found rejects the
draw (reasons 10/11, one warning per reason, until now without the hash).

**Where the game keeps them (new tool `tools/deko3d/gta4_fxc_shaders.py`, readers in `gta4_xbox_data.py`):**
- `common.rpf`: RPF2 whose table of contents is AES-256 encrypted (16 passes, the user's `aes_key.bin`, never in the
  repository) and whose files are raw-deflate compressed (not LZX: libmspack is not needed here, as LRA's
  `gta4_rpf_extractor.cpp` already does): **89 `shaders/fxl_final/*.fxc`** (RAGE `rgxa` effects). `xbox360.rpf` (863
  files) and its nested `playerped.rpf` hold none.
- `default.xex`: XEX2, AES-128 CBC with the retail key (`xex_module.cpp`), **basic compression** (no LZX): RAGE's own
  effects compiled into the executable (`grcore` im/fastmipmap, `grpostfx`, `rmptfx`, `gpuptfx` simple render and
  update, `atmoscatt_clouds`, `bink`, `perlinnoise`).

A container is found by its magic at any byte offset (RAGE does not align them) and checked under **XenosRecomp's own
rule** (`main.cpp:274`: `flags & 0xFFFFFF00 == 0x102A1100`, sizes inside the file, `field1C == 0`, `field20 == 0`; a
container failing it is skipped by XenosRecomp **without a message**: the Tekken 6 kind of silent rejection). The tool
names every candidate and its verdict.

| Phase | Count |
|---|---|
| Sources | 89 `.fxc` (common.rpf) + 1 executable image |
| Containers found | **1,370** (common.rpf 1,267, default.xex 103) |
| Valid under XenosRecomp's rule | **1,370** |
| Rejected | **0** (every container: `fieldC` 36, `field1C` 0, `field20` 0: GTA IV uses one container version; the 34 other `0x102A....` words in the `.fxc` are parameter bytes, not containers) |
| Duplicates | 0 (each container carries its own debug path) |
| Vertex shaders / pixel shaders | **666 / 704** (fxc 639 / 628; xex 27 / 76) |
| In run 56-60's package (s56) | 1,274 of 1,370; **missing 96**: fxc 9 VS + 8 PS (all from `gta_normal_reflect_alpha.fxc`, an effect the 480 s session never drew), xex 21 VS + 58 PS |
| The runtime dump (1,274) | **1,274 of 1,274 found offline** (1,250 in the `.fxc`, 24 in the executable: the "24 European shaders" of M0 are RAGE's particles, bink and perlin noise) |
| Translated (XenosRecomp, DXC) | 96 of 96 -> **162 SPIR-V** (96 + 66 late alpha-test sources), 0 DXC failures; 3 state machines structured |
| Compiled (uam) | **1,218 of 1,218 variants**, 0 failed; 30 VS zero-initialised, 30 of 30 pruned variants checked |
| Packaged (s61) | **2,074 shaders (1,370 + 704 late), 14,670 variants, 0 without DKSH**, 7,445 DKSH, 100,311,296 bytes; validated by `gta4_deko3d_runtime_check.py` and the console's C++ reader (64 of 64 DKSH read back) |
| s56's 1,912 shaders in s61 | **1,912 identical** (records, inputs, texture types, every variant's DKSH: `gta4_deko3d_package_diff.py`) |
| Requested in run 60 | 401 DKSH variants (the log counts variants, not distinct hashes) |
| Missing in run 60 | **0** (no draw skipped) |

**Case A against case B.** Run 60 drew every draw with shaders the package had: the characters' shards, the black
geometry and the missing surfaces of runs 47-60 are **not** missing shaders (case A excluded for what was played). They
are case B (a shader present, found, compiled, and a wrong picture) or a fixed-function stage. The 96 shaders added now
cover what the 480 s session never reached (a whole `gta_normal_reflect_alpha` effect, RAGE's GPU particle renderer
`gpuptfx_simplerender` with its rain shaders, the fast-mipmap blur and depth downsample, RAGE's own post-processing and
sky): any later scene that uses them would have lost those draws silently, one warning in all.

### AI3. What run 61 changes

1. **The package completed** (`deko3d-out-s61`): the 96 shaders the game holds and the package lacked, through the same
   chain (new `tools/deko3d/gta4_deko3d_merge.py` adds them to s56 without recompiling its 13,392 variants). Run 61's
   `deko3d/` carries it; the 1,912 shaders runs 56-60 used are byte-identical.
2. **A missing shader is named** (`dk_recorder.cpp` `ShaderMissing`): `deko3d: run 61 shader not in the package: vertex|pixel
   shader <HASH> (draw N); its draws are skipped`, once per hash (64 at most), so a skipped draw leads to its container in
   `gta4_fxc_shaders.py`'s manifest.
3. **The characters' depth test under test** (`dk_ab.h` phase `skinned depth always`, bit 512; run 61's
   `nx_dk_ab_phases = 513`): in that phase every skinned draw into the G-buffer (3+ colour targets and a depth target) has
   its depth compare made ALWAYS; the test and the write stay on, nothing else changes. Characters whole in this phase
   and in shards in the baseline = their fragments are rejected by the depth test (an earlier depth write over them:
   step 8 of the pipeline); in shards in both = the fragments never reach it (rasterization or the pixel shader's kill:
   steps 6-7). Lines: the phase line names `skinned G-buffer depth compare always|the title's` and the draws changed.

Not changed, with the reason: the vertex shaders (cleared by runs 55 and 60); any draw state (no state differs between
the characters and the scenery: AI1); ZCULL (off since M2; HowToDeko3d 6 measured ~2 ms a frame in WWHD, but this
renderer copies and restores depth often, and a stale ZCULL would add holes while the holes are being investigated);
the tiled cache (deko3d 0.5.0 never enables it; HowToDeko3d 7: untested); the resolves on the copy engine (AI1's
barriers; banjo-nx draws them: next, measured first); the audio (AI1: nothing new).

### AI4. Offline tests

`python tests/deko3d/test_run61.py` (new; with `dk_run61_xxh3.cpp`; the `.py` files need `git add -f`): **OK**.

| Part | Result |
|---|---|
| XXH3 | `gta4_xbox_data.xxh3_64` against the repository's xxHash (clang): **41 lengths, 0 wrong** (every path: 0-16, 17-128, 129-240, one and several 1,024-byte blocks); and 1,274 of 1,274 runtime-dumped containers hash to their file names |
| AB | dk_ab.h: `skinned depth always` is bit 512; mask 513 alternates baseline and it (clang `-Wall -Werror`) |
| AES | FIPS-197 AES-128 and AES-256 blocks, SP 800-38A CBC-AES128 (two blocks, the chain carried): ok |
| CONTAINERS | unaligned containers found, a truncated one and one with a virtual size under the header reported with their reason; `field1C` != 0 named as XenosRecomp's skip |
| RPF2 | a synthetic archive (a directory, a stored file, a deflated file) read back |
| MERGE | GDK1 round trip |
| RECORDER | the missing shader is named at both rejection sites |
| EVIDENCE | run 60: 29,022,176 drawn, skipped none; rejected 0; 615,067 probe draws, 24 shaders swapped (torso and head among them); 401 DKSH, 0 failed |
| REAL (THEFT4_GAME_DIR, THEFT4_AES_KEY, THEFT4_SHADER_DUMP, THEFT4_PACKAGE) | 89 `.fxc` + the executable, 1,370 containers, 0 rejected; the dump all found offline; s61 lacks none |
| CONFIG | `run61-build/game.toml`: deko3d, anatomy off, A/B on, phases 513 |

Also: `test_runtime_package.py --real deko3d-out-s61 --expect-shaders 2074 --expect-blobs 7445`: **all package tests
passed**, the C++ reader opens s61; `test_run60.py`, `test_run59.py`, `test_run58.py`: OK.

### AI5. Build

`out/console-runs/run61-build/` (`build_variants.sh ... dk-debug`, `-j3`, `THEFT4_DK_PACKAGE=<private>/deko3d-out-s61`,
log `run61-build.log`, 0 errors, 0 compiler warnings; built from the commit that adds this section, its hash in
`COMMIT`, without `-dirty`; the code is `2b173ea`'s). Rebuilt:
`dk_recorder.o`, `dk_graphics_system.o`, `main.o`. The ELF holds `run 61 shader not in the package` and `skinned depth
always` (strings checked). Note for the build from the Bash tool: its profile makes `pwd` print `C:/...`, which CMake
then prefixes with `/c/...`; run the script under `bash --noprofile --norc`. `COMMIT`: `d36131a`. `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,282,373 | `be2c60ab9877986ea111e2d00cab95aaec95f9efef84309a4c594e7f9ae7db40` |
| `switch-game-dk-debug.elf` | 323,508,072 | `d7ae68b1c9455b25e7e10cdf1d3fb4d11d96716dfd6daf52c7a27d0806ffb986` |
| `game.toml` | 1,592 | `1a04daab427f77d1e42f01cd9ba125bf4835312043cd1a02537e759ab00964d3` |
| `deko3d/gta4_deko3d_runtime.bin` (s61) | 100,311,296 | `f4d9fdc4f41cf3ad49b8af8853d768cda386efe108ba3b15dc3856da9b01789f` |
| `deko3d/gta4_deko3d_glsl.bin` (s61) | 43,826,659 | `b7ce84705982d12176f30136045556b1bf2668642e6f09610bae5759db419494` |

### AI6. Run 61: what to do, what to bring back

Standard clocks, no sys-clk / Horizon OC profile. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`,
`game.toml` and **the whole `deko3d/`** of `run61-build/` (the package changed: 100,311,296 bytes). The intro cut-scene to
its end, then **at least 15 minutes of gameplay** close to people (Roman, passers-by) and in a car; `+` held 1 s. Bring
back `game.log`, `game-crash.log`, `game-exit.log` and the whole `captures/` into `out/console-runs/run61-logs/`.

Look at, in this order:
1. **The characters in the `skinned depth always` captures against the `baseline` ones** (the phase lines name the
   phase; one capture in each phase's last window). Whole (drawn over what is in front of them, which is expected in
   that phase) = the depth test rejects their fragments: next, which earlier depth write (a placement restore, a
   hand-off, a pass of the same mesh). Still in shards = the fragments never reach the depth test: next, the pixel
   shader's kill and the rasterizer.
2. `run 61 shader not in the package` lines: none expected; any line names a shader to look up in the manifest.
3. The A/B lines: draws/s and GPU time of both phases (the depth-always phase is not a speed test).

Off switch: `nx_dk_ab = false`.

### AI7. Optimisations: what the measurements point at, what was not done

The audit of this session was driven by run 60's profile (AI1), not by a list: the title waits for the recorder at the
Present, the recorder waits for the GPU's slot fences (931 s of the 2,289 s session; 0 in some windows, 79-150 us a
drawn draw in others), the GPU reports 119.5 ms busy and 68.7 ms idle a frame. What the other ports did on those points
(their code read in earlier sessions, tables AC3 and AD4; local clones of nfsmw-nx `266f943`, sa2-nx `adf3a4e`,
masseffect-nx `650e14e`; katamari-nx, bejeweled3-nx, renut-nx and banjo-nx were not fetched again this session):

| Technique | Source | Here | Status |
|---|---|---|---|
| ZCULL on | nfsmw-nx HowToDeko3d 6 (WWHD: ~2 ms GPU a frame once uam patch 7 is in) | queue created with ZCULL off since M2; patch 7 is in | not done: needs `DkInvalidateFlags_Zcull` after every depth copy/restore (placements, hand-offs, resolves); a stale ZCULL adds holes while holes are investigated. Next, behind its own switch, after the depth answer of run 61 |
| tiled cache | HowToDeko3d 7 (deko3d 0.5.0 never enables it; WWHD disables it; "untested") | disabled every work (`BeginWorkState`) | not done: no port measured it; a GPU fault would cost the session |
| resolves drawn instead of copy-engine copies | banjo-nx (`banjo_renut_tuning.cpp`, AD4) | 28 resolves a frame on the copy engine, `DkBarrier_Full` around them, 69 barriers a frame | next: measure first (stamp the pre-list and the barriers so the GPU's idle 68.7 ms is attributed) |
| no allocation per draw, arena, futex waits | sa2-nx, masseffect-nx, nfsmw-nx | in since run 56 (AD3) | in |
| geometry store, chain reuse of state | nfsmw-nx, reNut | in since run 52; emitted-state cache `e_` | in |
| VS outputs only where read | masseffect-nx S6, banjo-nx gate | in since run 56; the 96 new shaders pruned too (30 VS) | in |
| submit copies only for real changes | (ours) | run 60's hash fix verified: 31,323 copies in a session against 6.4 M | in, verified on the console |

No speed change ships in run 61: its one A/B is the characters' depth test, and a speed change in the same session
would change what the captures are compared against. 30 fps at standard clocks is far (5.3 fps measured); the next
measured step is the GPU's idle time.

## 2026-11-09: run 61 read and run 62 (AJ): the depth test cleared, every link up to the clipper cleared, the GPU's own stage counters around the characters' draws

### AJ1. Run 61 read (`out/console-runs/61-logs/`, 72 captures; the folder is `61-logs`, not `run61-logs`)

| | |
|---|---|
| Run | **1,123 s** at CPU 1020 / GPU 460.8 / memory 1331.2 (every heartbeat), `+` exit (mode D, `exit(0)`), **0 faults**; 931 MiB of heap headroom at the end. 12 A/B cycles (baseline / skinned depth always), started in the intro cut-scene (frame ~4,300) and run on into gameplay. |
| Package on the console (VERIFIED) | `gta4_deko3d_runtime.bin: 2074 shaders, 7445 DKSH ... (14670 variants, 0 without DKSH, 100311296 bytes)`: s61, the size of `run61-build`'s. **0 `run 61 shader not in the package` lines**; `drawn 19,969,364 (skipped by reason: none)`; `406 from the PC's DKSH, 0 compiled by the console's uam, 0 failed`. |
| Depth-always phase (VERIFIED) | It ran: the phase line counts the skinned G-buffer draws drawn with compare ALWAYS (6,687 after the first cycle, 55,757 before the last); 12 phase captures (`run 58 A/B: capture of phase "skinned depth always"`). The override is applied before the depth state is bound (`dk_recorder.cpp`, `ab_skinned_depth_always_`). |

**The decisive comparison: case B (VERIFIED by eye on the captures paired by the phase lines).** In every
`skinned depth always` capture the characters are still in shards: `dk-f006173` (Niko, cut-scene: torso and arms in
pieces, the background seen through the holes, a strip of hair standing off the head), `dk-f006518` (body in shards),
`dk-f006854` (head and body of a character on the right), `dk-f005779` (no character; the ship's hull with the same
dark triangular shards). The baseline captures between them (`dk-f005894`, `dk-f006307`, `dk-f009241`, `dk-f010979`)
show the same holes. Head, torso, arms and clothes are all affected; no region is whole in one phase and broken in the
other. **The depth test does not reject the characters' fragments; the cause is before it.** Not applied as a fix
(it would only have hidden fragments that do arrive).

**Every link up to the clipper, cleared (VERIFIED offline, `tests/deko3d/test_run62.py PROBE`).** Run 60's probe files
hold the GPU's own `gl_Position` of every vertex of the characters' meshes and the draw's indices. For the five character
vertex shaders (torso `9A16E1CEAD4ABA0D` 3,463 vertices, head `A2D8E2F3F87A09AE` 2,811, hair `EF7ED6F38B1F9211` 2,943,
`6A8A2868D092BFD7` 2,559, `9023A3B988FC0EB2` 1,264): **0 NaN, 0 vertices outside 0 <= z <= w, 0 indices outside the
mesh**. Rasterized on the PC with the title's culling (CCW kept on screen), the meshes are whole (a head and shoulders,
the inside of the mouth and eyes the culled side). With run 55 (VS arithmetic), run 58 (cull off), run 60 (VS binary)
and run 61 (depth): what is left is **the clipper/rasterizer, the pixel shader's kill and the stencil/sample tests on
the console**. The characters' pixel shaders (`A980D55015FD7DF2`, `DA6733D3DA5C3DB8`, `76E0F19DF71047E7`) have one
`discard` each, the alpha test, compiled out unless specialization bit 2 is set; the recorder sets it only for an
alpha-tested draw (`value = 0x2 | func << 8`), and the torso is drawn without alpha test (run 60's state line).

**Other findings of the read:**
- *Character shadows.* The 512x512 skinned shadow draw of run 60's probe `5E7C32C4EF6771C0` (2,811 vertices, 4,999
  triangles, viewport depth 1-0) has **every vertex at z/w 1.07-1.11**: the clipper removes the whole mesh (the
  2560x2560 cascades are inside the range). Whether the title asks for depth clamping there is not logged
  (`PA_CL_CLIP_CNTL` per draw is not in the probe), so it is **not changed** in run 62.
- *Exposure.* The two daytime captures just after leaving the safehouse are white (`dk-f010414`, `dk-f010531`); the
  daytime captures after them are plausibly exposed (`dk-f010979`, `dk-f011528`): an adaptation after the interior, not
  the run 56 chain (13 EDRAM materializations a frame, 0 refused). The flickering lights of the report are not visible
  in 20 s stills; no line measures them.
- *Speed* (A/B totals, 24 windows a phase, cut-scene and gameplay mixed): baseline **5.82 fps, 20,990 draws/s, 3,607
  draws a frame, GPU busy 113.9 ms + idle 57.1 ms a frame**; recorder **23.8 us a drawn draw** (totals). The title's
  render thread (profile from 600 s, `where.py --stacks`): **35.7 % waiting at the Present for the recorder, 21.4 %
  blocked on `front_mutex_` in `SubmitTitleCommand`**, `CaptureDraw`/`DiffRegisters` ~6 %; the recorder 47 % waiting for
  packets, 16 % in `BeginWork`'s fence wait; the ring of 4,096 packets is full 900+ times a window in gameplay while the
  recorder idles at other moments of the same window (bursts of draws, then the title's own work). Recorder and GPU
  cost about the same per frame (~110-150 ms each): no single dominant cost to remove without a measurement of each.
- *Stutter.* 0 shaders compiled on the console, 0 variants pending: no shader compile stutter. The XMA decoder and
  file reads (thread 7: 20 % in `fsdev_read`) are the I/O seen; the startup windows are loading. No per-frame time
  histogram exists to separate stutter from the steady cost (not measured).
- *Audio* (110 windows): **the mixer produces 53-68 % of real time in gameplay windows** (100 % in menus), the output
  runs dry 17-59 times a window, 1 ms a window waiting for a free buffer. The guest mixer thread (id 16) is busy in the
  title's recompiled DSP code (`sub_82185E38` -> `sub_8216A288` -> `sub_82173688`/`sub_821743C0`/`sub_821737C0`, guest
  128-bit vector loads and stores). **The crackle is starvation: too few samples, not a conversion or buffer error.** The
  fix is native code for those functions (as run 51 did for `sub_8216DBB0`); not done in run 62.

### AJ2. What run 62 changes

1. **The GPU's own stage counters around the characters' draws** (new `dk_stage_counters.h`, `DkRecorder::Draw`): one
   indexed G-buffer draw in four of each class (skinned = the characters, the rest = the scenery, the control), up to 32
   a work: `dkCmdBufResetCounter` of input primitives, clipper input, clipper output, fragment invocations and samples
   passed before the draw, `dkCmdBufReportCounter` after it, into the core's report block after the pass stamps (48,000
   bytes of stamps + 3 x 32 x 80 bytes); read when the slot's fence has been waited for. Lines: `run 62 stage counters,
   totals, <class>: ... input primitives N (x of expected), clipper input, clipper output, fragment invocations, samples
   passed` with a verdict (`StageReport::Verdict`), per A/B phase at each phase's end, the characters' vertex shaders one by
   one once a minute.
2. **A/B `nx_dk_ab_phases = 545`**: baseline, cull off, skinned depth always. With the counters, cull off says what the
   clipper removes besides culling (the probes say: nothing should be), depth always what is lost after the pixel shader
   besides the depth test.

What the answer leads to: assembly short = the index path on the GPU (bind, format, restart); clipper loss with cull off
= the clip state (the shadow finding above points the same way); fragments lost = the pixel shader's kill or the stencil
/sample state; no stage loses more for the characters than for the scenery = the triangles are written and something
later overwrites them in the targets (placements, resolves).

Not changed, with the reason: no renderer state (nothing proven against one: run 62 measures it); the shadow clip (the
title's request is not known); the audio (needs native DSP functions: a session of its own); ZCULL, tiled cache, drawn
resolves (no measurement of their share yet, AI7).

### AJ3. Offline tests

`python tests/deko3d/test_run62.py` (new; with `dk_run62_stage.cpp`): **OK** (CONFIG checked once the build folder
exists).

| Part | Result |
|---|---|
| STAGE | 14 checks, clang `-Wall -Werror`: layout fits the 64 KiB block after the stamps (32 a slot, aligned, distinct), sampler one in four within the room, the five verdicts and "not counted", totals and the report's value word, mask 545 = baseline, cull off, skinned depth always, cycling |
| EVIDENCE | run 61: the depth-always phase began 12 times and changed 55,757 draws before the last, 12 phase captures, drawn 19,969,364 skipped none, missing shaders 0, the s61 package (2,074 / 7,445 / 0 without DKSH / 100,311,296 bytes), audio 53-100 % of real time over 110 windows |
| PROBE | the five character meshes of run 60: 0 NaN, 0 outside 0 <= z <= w, 0 indices outside |
| RECORDER | the counters reset before the draw, reported after it, harvested at the slot's reopening |
| CONFIG | `run62-build/game.toml`: deko3d, anatomy off, A/B on, phases 545 |

### AJ4. Build

`out/console-runs/run62-build/` (`build_variants.sh ... dk-debug`, `-j3`, `THEFT4_DK_PACKAGE=<private>/deko3d-out-s61`,
log `run62-build.log`, 0 errors, 0 compiler warnings), built from commit `5be09cd` (in `COMMIT` and in the ELF, without
`-dirty`). Rebuilt: `dk_recorder.o`, `dk_graphics_system.o`, `main.o`. The ELF holds the `run 62 stage counters` lines
(strings checked). From the Bash tool the script needs absolute `/c/...` paths for itself and the destination, with
`PWD=/c/dev/Theft4-NX-clean` (otherwise CMake is given `/c/dev/.../C:/dev/...`). `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,298,757 | `93eb2163804d845ab684c3173b6df75c5aa82c63977ad27b441531d445835ce2` |
| `switch-game-dk-debug.elf` | 323,796,904 | `0fe974904a38dec29bff63494f40af7020c44257882b72ebb30a120cdf8c83f6` |
| `game.toml` | 1,631 | `13bca51d90ade52f7c2976771e98dc87fb98b2622de1ade127ea100931466e2b` |
| `deko3d/gta4_deko3d_runtime.bin` (s61, unchanged) | 100,311,296 | `f4d9fdc4f41cf3ad49b8af8853d768cda386efe108ba3b15dc3856da9b01789f` |
| `deko3d/gta4_deko3d_glsl.bin` (s61, unchanged) | 43,826,659 | `b7ce84705982d12176f30136045556b1bf2668642e6f09610bae5759db419494` |

### AJ5. Run 62: what to do, what to bring back

Standard clocks, no sys-clk / Horizon OC profile. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`,
`game.toml` and the whole `deko3d/` of `run62-build/` (package unchanged since run 61). The intro cut-scene to its end,
then **at least 15 minutes of gameplay** close to people (Roman, passers-by) and in a car; `+` held 1 s. Bring back
`game.log`, `game-crash.log`, `game-exit.log` and the whole `captures/` into `out/console-runs/run62-logs/`.

Look at, in this order:
1. `run 62 stage counters, phase "cull off"` and `phase "skinned depth always"`: the verdict, and the characters'
   ratios against the scenery's.
2. `run 62 stage counters, totals, skinned vertex shader 9A16E1CEAD4ABA0D` (and the other character shaders).
3. The A/B lines: draws/s and GPU time of the phases (the counters cost a report per sampled draw: compare with run 61).

Off switch: `nx_dk_ab = false` (the counters stay on: one draw in four of the G-buffer, 32 a frame at most).

## 2026-11-09: run 62 read and run 63 (AK): the characters' triangles reach the targets; what overwrites them next

### AK1. Run 62 read (`out/console-runs/run62-logs/`, 61 captures)

| | |
|---|---|
| Run | **1,024 s** at CPU 1020 / GPU 460.8 / memory 1331.2, `+` exit, **0 faults**, 934 MiB headroom. A/B baseline / cull off / skinned depth always, 7+ cycles. |
| Speed (A/B totals, 16 windows a phase) | baseline **5.66 fps, 22,919 draws/s, 4,050 draws a frame, GPU busy 123.3 + idle 53.5 ms a frame** (run 61: 5.82 fps, 20,990 draws/s in different shots: the stage counters cost no measurable throughput); recorder **20.0 us a drawn draw = textures 3.6 + vertices 5.9 (5.5 of it the geometry store re-hashing ranges every frame) + index range scan 1.8 + constants 2.2 + rest 6.5**. |
| Audio | 54-100 % of real time (gameplay 54-68 %): as run 61. |

**The stage counters (VERIFIED, `tests/deko3d/test_run63.py EVIDENCE`; per phase, all cycles):**

| Phase | class | draws sampled | assembled / expected | clipper out / in | fragments a triangle | samples passed / fragment |
|---|---|---|---|---|---|---|
| baseline | characters | 16,942 | **1.000** (0 short) | 0.329 | 8.5 | **0.989** |
| baseline | scenery | 90,376 | 1.000 | 0.296 | 57.4 | 1.000 |
| cull off | characters | 6,164 | 1.000 | 0.339 | 6.2 | 0.992 |
| cull off | scenery | 33,324 | 1.000 | 0.345 | 25.3 | 1.000 |
| depth always | characters | 5,870 | 1.000 | 0.328 | 5.1 | 1.000 |
| depth always | scenery | 30,682 | 1.000 | 0.351 | 20.5 | 1.000 |

Per vertex shader, the clipper's share matches the faces run 60's probe meshes show to the camera (head `A2D8E2F3F87A09AE`
0.326 against 0.28 counted offline, torso `9A16E1CEAD4ABA0D` 0.530 against 0.54); the hair (`EF7ED6F38B1F9211`, alpha
test) passes 0.941 of its fragments, the rest of the characters 1.000. The clipper's share does not move with "cull off"
in either class (0.329 -> 0.339, 0.296 -> 0.345): this counter is taken before face culling, so it measures frustum
clipping, which treats the characters as it treats the scenery.

**Verdict (VERIFIED for what the counters measure): every stage of the GPU writes the characters as it writes the
scenery.** Their triangles are assembled, clipped, shaded and written. With run 56's anatomy (holes in every G-buffer
target and in the resolved depth, the wall behind visible through them), the holes are made **after** the characters'
draws and before the G-buffer is resolved: something writes their surfaces again. The A/B phases of runs 58-62 could
not show it (none changes what comes after the draw). Excluded now: assembly, clipping, rasterization, the pixel
shader's kill, the stencil and sample tests.

Not done, with the reason: the audio (AJ1: the hot work is behind indirect calls of `sub_821737C0`'s dispatch, reached
through `REX_CALL_INDIRECT_FUNC`; the profiler's stacks do not name the callees, so which function to write natively is
not known yet; no speculative change); the diagnostic hashes (constant banks, submit race: about 1-2 % of the title's
render thread and of the recorder; the submit hash also drives the promotion of rewritten buffers, a safety net);
the geometry store's per-frame verification (5.5 us of 20 a draw: trusting ranges for several frames risks stale
geometry, so it needs a guest-write signal first); the shadow clip (the title's `PA_CL_CLIP_CNTL` per draw still not
logged).

### AK2. What run 63 changes

1. **What writes over the characters** (new `dk_overwrite.h`, `OverwriteTracker`): per frame, the guest surfaces
   (colour targets and depth) a skinned G-buffer draw wrote; until each one's resolve, every later operation on it is
   counted by kind: clears (the title's, a resolve's colour clear), **EDRAM placements materialized into it** (the copy
   or the conversion pass rewrites the whole image), depth hand-offs into it, resolve depth clears, **draws that do not
   test depth** (or test ALWAYS) and draws that test it. The first 32 that are not depth-tested draws are logged with the
   frame, the draw and its vertex shader. Also counted: the depth/colour-0 pairs the characters and the scenery draw with,
   and the character draws on a depth surface the scenery never uses (a character there hides nothing and is hidden by
   nothing, so later scenery overwrites it).
2. **`nx_dk_ab = false`**: no phases; the stage counters stay on.

What each answer fixes: placements > 0 = the materialization rewrites a target the characters are in (compare the
placement key of the G-buffer surfaces); draws without depth > 0 = those draws (the logged vertex shaders) and their
state; characters on their own depth = the surface translation (`Surface`, `BindTargets`) gives them a separate image;
only depth-tested draws = the characters' depth values lose to the scenery's (the next instrument: the depth they write).

### AK3. Offline tests

`python tests/deko3d/test_run63.py` (new; with `dk_run63_overwrite.cpp`): **OK**. OVERWRITE: 16 checks, clang
`-Wall -Werror` (nothing counted before a character or on another surface, the resolve ends the following, events listed
once, depth-tested draws counted not listed, a new frame forgets, own-depth draws found). EVIDENCE: run 62's three
phases as in the table. RECORDER: the six hooks. CONFIG: `run63-build/game.toml`. `test_run62.py`: OK.

### AK4. Build

`out/console-runs/run63-build/` (`build_variants.sh ... dk-debug`, `-j3`, package s61 unchanged, `run63-build.log`, 0
errors, 0 warnings), built from commit `fe696e8` (in `COMMIT` and in the ELF, without `-dirty`; the ELF holds the
`run 63 after the characters` line). `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,306,949 | `396917d3137e436c9d1f8c8f4ecb3cb89d40461ca1920067b9812cf7395a9884` |
| `switch-game-dk-debug.elf` | 324,058,960 | `bc13c86b476dd7574887c96cec3ffaeb978e9d4b2f884d8b6e6128e06544cc58` |
| `game.toml` | 1,345 | `3eabe13504e64193fb16a3235df6f39be5b2fafb39bc78047bac9b6107218749` |
| `deko3d/gta4_deko3d_runtime.bin` (s61) | 100,311,296 | `f4d9fdc4f41cf3ad49b8af8853d768cda386efe108ba3b15dc3856da9b01789f` |
| `deko3d/gta4_deko3d_glsl.bin` (s61) | 43,826,659 | `b7ce84705982d12176f30136045556b1bf2668642e6f09610bae5759db419494` |

### AK5. Run 63: what to do, what to bring back

Standard clocks. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`, `game.toml` and the whole `deko3d/`
of `run63-build/`. The intro cut-scene to its end, then **10 minutes of gameplay close to people** (Roman's flat, the
street) and one drive; `+` held 1 s. Bring back `game.log`, `game-crash.log`, `game-exit.log` and `captures/` into
`out/console-runs/run63-logs/`. Look at: `run 63 after the characters' draws` (which kinds are not 0), the `run 63
overwrite` lines (which surfaces, which vertex shaders), `run 63 surfaces`.

## 2026-11-09: run 63 read and run 64 (AL): no instrument; the store checked by write signal, the mixer's routing function native

### AL1. Run 63 read (`out/console-runs/run63-logs/`, 55 captures)

| | |
|---|---|
| Run | **1,140 s** at CPU 1020 / GPU 460.8 / memory 1331.2, `+` exit, **0 faults**, 931 MiB headroom, A/B off. |
| Recorder (all windows, 20.6 M draws) | **24.3 us a drawn draw; vertices 6.6, index range scan 2.0** (the two parts the geometry store's hashing is in). |
| Geometry store (totals) | 34,858,953 lookups, 34,620,033 hits; **11,181,751 trusted within their epoch (32 %), 23,677,202 hashed: 188,227 MiB (8,335 bytes each)**; **448 content changes** in the whole session. |
| Audio | 108 of 112 windows below 90 % of real time: **53-81 %**, the output dry 30 times a window on average (54-56 % and 43-44 times in the last windows). |

**The overwrite tracker (VERIFIED, `tests/deko3d/test_run64.py EVIDENCE`):** on the surfaces a character drew into, between
its draw and the surface's resolve, over the whole session: **clears 0, EDRAM placements materialized 0, depth hand-offs
0, resolve depth clears 0, character draws on a depth surface the scenery never uses 0**. Draws that do not test depth:
7,987 over 31,948 resolved surfaces, that is **one a frame, always the same**: vertex shader `2299F2D447D96CC6`, pixel
shader `A11692B372C58D98`, a rectangle list over the screen with **colour write mask `800`** (the alpha of the third
target only), depth test and write off. It cannot make the holes: they are in the albedo, the normals and the depth.
Draws that test depth: 4,466,451 (the scenery: 527 a frame, drawn after the first skinned draw).

**What that means for the characters: AK2's table has no fix for this answer.** Nothing rewrites the characters'
surfaces; AK2's last row ("only depth-tested draws = the characters' depth loses to the scenery's") names an instrument,
not a correction, and no state of this renderer is proven wrong by run 63. **No change was made to the characters'
path** (a change without a proven cause is what the rules forbid). What was checked again offline in this session, on
data already here:

- Run 60's probe meshes drawn as wireframes (the GPU's own `gl_Position`, the draw's indices): the head (`A2D8…`, w 2.2-2.5),
  the jacket near the camera (`9023…`, w 0.9-1.5) and the far torso (`9A16…`) are whole. A near mesh is right after the
  vertex shader.
- In every probe mesh `z = A w + B` with one (A, B) per draw (reverse Z, near 0.05 / 0.25 / 0.4 by camera): no vertex
  with a depth of its own.
- Run 56/57's anatomy captures of the G-buffer depth (`dk-f005603-a11`, `dk-f006207-a11`): the far pedestrian is whole
  with stencil `0x89`; the near characters keep their silhouette and lose about half of their triangles, whole, with
  straight edges; the pieces left carry the pedestrians' stencil, the holes carry the background's depth and stencil.
- The characters' pixel shaders (`ps_7D672A6B…`, `ps_652CFED6…`): `discard` only under the alpha-test variant (specialization
  bit 2), `gl_SampleMask` all ones unless alpha to mask is on (off for these draws), early fragment tests forced only
  in the variants without the alpha test. Not the cause.
- The vertex shaders write `gl_ClipDistance[0] = 0.0` when the title's clip plane is off (`g_DkShared[36].x`), and the
  recorder writes that word as 0: constant for every draw, characters and scenery alike.
- Not verified by any run so far: **the index bytes the GPU reads** (the probe file's indices are converted again on the
  CPU; the stage counters count triangles, not which vertices they join). This is the one input of the characters' draws
  between the vertex shader and the rasterizer that no run has compared.

### AL2. What run 64 changes

1. **The geometry store checks a range once a frame, and again when its buffer is written** (`dk_geometry_store.h/.cpp`
   `Options::trust_signals`, `Written`, `Find(..., owner, ...)`; cvar `nx_dk_geometry_signals`, default on).
   *Before:* a stored range was trusted until the end of its epoch, and an epoch ends at every clear, resolve, depth
   hand-off, resource event, texture lock, work and frame: a mesh drawn in the shadow pass and again in the G-buffer was
   hashed twice a frame, 8.3 KiB each time, and none of those boundaries is a write. *Now:* each range remembers the guest
   buffer object it is read through (`RecorderVertexStream::buffer`, `RecorderDraw::index_buffer`); the hooks already
   send the title's **unlock after a guest write** and the object's **release** in the command stream, in order with the
   draws (`RecorderResourceEvent`), and the recorder turns both into `Written(handle)`. A range checked in this frame
   whose object has had no such signal since is bound without hashing. That is the content at the draw's submit: a
   re-hash at record time could see bytes the title wrote after submitting the draw.
   *Never trusted this way:* the front end's submit copies (buffers rewritten after their first fill: mode bit 63, as
   before); a buffer **locked when the draw was submitted** (new `RecorderDraw::locked_buffers`, from the lock count the
   front end already reads: the recorder passes owner 0); a range met through two different buffer objects; a range not
   yet hashed in this frame. **Every range is still hashed once a frame**: the signals are not proven complete (a write
   that passes no Lock, a second object over the same memory), so a write without a signal is found by the next frame's
   hash, as it was, and counted apart.
   *Line:* `run 64 geometry store write signals (totals): hits without a hash ..., write signals ..., content changes
   without a signal N of M changes`. If N stays 0 over a session, trusting ranges across frames is the next step (the
   rest of the 6.6 + 2.0 us); not done now, because nothing measured says the signals are complete.
2. **The mixer's routing function `sub_8216D7E0` as native code** (`tools/switch-game/nx_audio_routes.h`, strong
   definition in `nx_audio_mixer.cpp`; cvar `nx_audio_native_routes`, default on).
   *Which function:* `tools/switch-game/guest_profile.py` (new) charges each sampled pc of the mixer thread to its
   outermost frame instead of the inlined NEON load `where.py --profile` names. Run 63, thread 16, 53,832 samples:
   **`sub_8216D7E0` 22.5 %**, `sub_8216DBB0` 8.4 % (run 51's NEON loop), `sub_82185E38` 8.0 %, `sub_828C6740` 5.9 %,
   waits 4.4 %, `sub_82188C28` 2.9 %. The stacks name what is behind the indirect calls: **`sub_821737C0` (vtable slot
   6) calls `sub_828C6740`, `sub_828C5A58`, `sub_828C2D28`** (and `sub_82178398`); **`sub_82185E38` calls
   `sub_82188C28`**, one of the 40 format-specific resamplers at `0x8217BDC0-0x8218B798` that no generated code calls
   directly.
   *What it is:* under the audio spin lock, for each route of a mix object (source channel, destination channel,
   current gain, target gain) a 256-sample gain ramp, then either the voice loop `sub_8216DBB0` (add) or a 64-vector
   loop inside the function (overwrite). *Before:* every vector through the stored PPC registers, the ramp written to the
   guest stack for the voice loop to load back. *Now:* the same guest loads, stores, scalar arithmetic, flush-mode
   switches and calls of the guest's other functions (format match, fill, copy, the lock), with the vectors in host
   registers. The generated code reaches the function only through the function table (0 direct calls), so the strong
   definition replaces the weak alias and **no generated file changes**. With either audio cvar off the generated
   function runs.
   *Line:* the `audio:` line adds `native routing calls N`; its `% of real time` and `output ran dry` are the measure.
3. `game.toml`: A/B off, profiler from 600 s (as run 63). No new instrument: the run 62 stage counters and the run 63
   tracker are untouched.

Not changed, with the reason: the characters (AL1: no proven cause); shadows and lighting (no evidence); the resamplers
and `sub_828C6740` (next native functions by share, each needs its own equivalence run against the generated body, as
below); trust of stored ranges across frames (needs `content changes without a signal` from a console run).

### AL3. Offline tests

`python tests/deko3d/test_run64.py` (new; `dk_run64_store.cpp`, `dk_run64_routes.cpp`): **OK** (CONFIG once the build
folder exists).

| Part | Result |
|---|---|
| SIGNALS | 108,000 draws over 48 buffers the title writes at any moment (through a Lock with its signal, while locked, through a submit copy, through a second object, with no Lock between frames): **0 ranges bound with other bytes than the draw's**, signals off and on; 4,894 lookups answered by signal; 0 changes without a signal when every write has one, 1,283 counted when 403 writes have none. Clang `-Wall -Wextra -Werror`. Mutation: with `Written` recording nothing the test fails (10 wrong ranges, 396 in RELEASE). |
| RELEASE | an object released and its memory reused by other bytes in the same frame: 2,333 bound, 0 wrong |
| BENCH (this PC, run 63's shape: 2,400 ranges of 1-15 KiB, twice in one epoch and once in another a frame, 24 unrelated signals a frame) | **446 -> 237 ns a lookup (-47 %), 5,354 -> 2,677 bytes hashed a lookup**, every lookup a hit in both. A PC measure of the same code; the console's is run 64's `recorder time per drawn draw` (vertices, index range scan) against run 63's 6.6 and 2.0 us. |
| ROUTES | `nx_audio_routes.h` against **the generated `sub_8216D7E0` and `sub_8216DBB0` themselves** (extracted by the test from the local `gta4_recomp.2.cpp`, compiled with the generated header): 6,000 random mix objects (4,025 with a route table, 2,710 overwriting, 2,013 with the source at or over the destination, 1,975 without a table; null descriptors, the lock free / held / held by another thread, a failing format match, gains from denormal to 1e30, random constants): **0 differences** in guest memory outside the dead stack frame, r1, r3, the flush mode and the logged calls (arguments and return addresses). Mutation: the ramp advanced by the wrong step fails 551 rounds. |
| SPEED (this PC, simde's NEON on SSE) | 8 routes: add 2,303 -> 711 ns (3.2x), overwrite 2,563 -> 964 ns (2.7x). Not the console. |
| AUDIO | run 63's log and ELF through `guest_profile.py`: the shares above; `sub_8216D7E0` first; the callees behind both indirect calls named |
| EVIDENCE | run 63's store, recorder and tracker numbers of AL1 |
| WIRING | owner passed unless locked (vertices, indices), unlock/release -> `Written`, the lock mask, both cvars, the audio counter |

`test_run53.py`, `test_run52.py`, `test_run51.py`: OK (the store's and the mixer's older tests).

### AL4. Build

`out/console-runs/run64-build/` (`build_variants.sh ... dk-debug`, `-j3`, package s61 unchanged, `out/run64-build.log`, 0
errors, 0 warnings), built from commit `50492e5` (in `COMMIT` and in the ELF, without `-dirty`). In the ELF: `sub_8216D7E0`
and `__imp__sub_8216D7E0` at different addresses (the native definition is the one in the function table), the `native
routing calls` and `run 64 geometry store write signals` lines. From the Bash tool the build needs a clean environment
with `TMP`, `TEMP` and `SYSTEMROOT` set (GCC otherwise fails with "Cannot create temporary file in C:\WINDOWS\").
`SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,319,237 | `ec7a8e93f77dcdbfdbdf12d37cef6fc52087b657f833df88c3278bee2d61a344` |
| `switch-game-dk-debug.elf` | 324,282,216 | `72cc282a0f0e86a81dd248a471ca560dbbe50f25fd55a6aa3e856dbd22e06c43` |
| `game.toml` | 1,477 | `447b2e957925078f8b1e5dd5f906bbb53ee3ed44ebed557234c998c75b3179e1` |
| `deko3d/gta4_deko3d_runtime.bin` (s61) | 100,311,296 | `f4d9fdc4f41cf3ad49b8af8853d768cda386efe108ba3b15dc3856da9b01789f` |
| `deko3d/gta4_deko3d_glsl.bin` (s61) | 43,826,659 | `b7ce84705982d12176f30136045556b1bf2668642e6f09610bae5759db419494` |

### AL5. Run 64: what to do, what to bring back

Standard clocks (CPU 1020 / GPU 460.8 / memory 1331.2). Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`,
`game.toml` and the whole `deko3d/` of `run64-build/`. The intro cut-scene to its end, then **at least 10 minutes of
gameplay** as run 63 (on foot close to people, one drive); `+` held 1 s. Bring back `game.log`, `game-crash.log`,
`game-exit.log` and `captures/` into `out/console-runs/run64-logs/`.

Neither change has run on the console yet: what is verified is the offline equivalence and the build. Look at:
1. `recorder time per drawn draw`: vertices and index range scan against run 63's 6.6 and 2.0 us.
2. `run 64 geometry store write signals`: hits without a hash; **content changes without a signal** (0 expected; any
   other number says which writes pass no Lock, and the per-frame hash caught them).
3. `audio:`: `% of real time` and `output ran dry` against run 63's 53-81 % and 30 a window; `native routing calls`
   above 0. The sound must be the same as before (same samples by construction).
4. `guest_profile.py game.log run64-build/switch-game-dk-debug.elf`: `sub_8216D7E0`'s share against 22.5 %.

Off switches: `nx_dk_geometry_signals = false`, `nx_audio_native_routes = false`.

## 2026-11-10: run 64 read and run 65 (AM): the store trusted across frames; the characters as drawn against as resolved

### AM1. Run 64 read on the console (`out/console-runs/run64-logs/`, 41 captures; `tests/deko3d/test_run65.py SWITCH`)

Console measures only (CPU 1020 / GPU 460.8 / memory 1331.2). The two sessions are different play (867 s against
1,140 s), so small differences are not attributed.

| | Run 63 | Run 64 |
|---|---|---|
| Stability | 1,140 s, 0 faults, 0 draws skipped | **867 s, 0 faults, 0 draws skipped**, `+` exit, 935 MiB headroom |
| Recorder, us a drawn draw | 24.26 (vertices 6.55, index range scan 1.97) | **23.59 (vertices 6.49, index range scan 1.71)** |
| Store: lookups hashed | 23.68 M of 34.86 M (68 %), 5,661 bytes a lookup | **14.42 M of 25.57 M (56 %), 5,250 bytes a lookup**; 3.04 M answered by write signal |
| Content changes | 448 | 371, **0 without a write signal**; 5,336 signals |
| Audio (windows below 90 %) | 61.4 % of real time, output dry 29.8 times a window (108 windows) | **66.3 %, 24.0 times** (79 windows) |
| Mixer thread, routing function + voice loop | **30.9 %** (`sub_8216D7E0` 22.5 + `sub_8216DBB0` 8.4) | **8.6 %** (one function now); 31.6 M native routing calls |
| Mixer thread, next | `sub_82185E38` 8.0, `sub_828C6740` 5.9, `sub_82188C28` 2.9 | `sub_82185E38` 8.2, `sub_828C6740` 6.6, `sub_82188C28` 3.2 |

What that says:
- **The write-signal check works and is safe, and gains little by itself.** 12 % of the lookups were answered by signal,
  the recorder's vertex and index parts moved by 0.06 and 0.26 us (-3 % of the recorder's time, inside the difference
  between two sessions). The PC benchmark of run 64 (-47 %) assumed every range drawn in two epochs a frame; on the
  console most ranges are looked up in one epoch a frame, so the per-frame hash is what is left. **The PC figure was
  not a prediction of the console.** The useful result is the count: **0 of 371 changes without a signal**.
- **The native routing function does on the console what the equivalence test said**: its share of the mixer thread
  fell from 30.9 % to 8.6 %. The audio is still short (66 % of real time, 24 dry outputs a window): the thread stays at
  95 % of a core, the freed time went to mixing more, not to idling. No regression heard or logged. The picture is as
  run 63's (characters in shards).

### AM2. The characters: the index path, and where the fault can still be

The path was read end to end: guest index bytes (`TranslateVirtual`, the same view the title writes) ->
`ConvertIndices` (big-endian 16 or 32 bit to host, lo/hi) -> the geometry store's block or the upload ring (both
CPU-uncached, written once, before the work is submitted) -> `dkCmdBufBindIdxBuffer(format, address)` ->
`dkCmdBufDrawIndexed(count, firstIndex 0, vertexOffset -lo)` with each stream bound at `base vertex + lo`.

**What existing console data says about the indices the GPU reads (VERIFIED, `test_run65.py INDICES`):** run 60's probe
draws the same draw once more with the same index binding and a vertex shader that writes `gl_Position` into a block
pre-filled with ones, at `gl_VertexID`. In all 24 probed skinned meshes **no vertex slot is left unwritten** (0 of
their vertices), and the draw's own indices name every vertex: the GPU's index stream reached every vertex of each
mesh, the head (4,999 triangles) and the near jacket among them. An index buffer that was truncated, stale, shifted or
read in the wrong format leaves slots unwritten or writes outside the range; none did. This is not a byte comparison of
the index memory, but with run 55/60's positions (right) and run 62's counters (every triangle assembled) it leaves no
room for a wrong index stream in the probed draws.

Also closed in this session (offline, on the installed library and the package):
- **User clip distances are off in deko3d**: the disassembly of the installed `libdeko3d.a` has no write of the
  clip-distance enable register (Maxwell 3D 0x544) in its queue set-up or anywhere else; the `gl_ClipDistance[0]` every
  vertex shader of the package writes is ignored (and it is the constant 0 while the title's plane is off).
- The pixel shaders' `gl_SampleMask` is all ones unless alpha to mask is on; `discard` only in the alpha-test variants.
- The holes follow triangle edges, not a 16-pixel grid (`dk-f005603-a11` enlarged): not ZCULL or a tile artefact.
- Run 57's anatomy resolves the first colour target twice in a frame with equal images: no earlier state of it exists
  in any capture.

**Not proven, so not changed:** the cause. Everything measured so far holds for the draw as the probe repeats it; what
no run has is the same frame's character **as drawn** next to **as resolved**. Run 62's counters say the triangles are
written, run 63's tracker says nothing rewrites them, and the pictures say they are gone: one of the three is wrong for
the near characters, and only a picture taken right after the draw says which.

### AM3. What run 65 changes

1. **The geometry store trusts a range across frames** (`dk_geometry_store.h/.cpp` `Options::trust_frames`, cvar
   `nx_dk_geometry_trust_frames`, default 8). *Why now:* AM1's count (0 of 371 changes without a signal on the console)
   is the measure run 64 said it needed. *Before (run 64):* a range was hashed once a frame. *Now:* a range checked
   through its buffer object within the last 8 frames, the object without a write signal since, is bound without
   hashing, and hashed again every 8 frames. *Guard:* a change found without a signal sends that object back to a check
   every frame; the eighth such change in a session ends the trust across frames for every range (`trust_frames` 1, run
   64's behaviour). The exclusions of run 64 stay (submit copies, buffers locked at the draw, ranges met through two
   objects). *Exposure, stated:* if a write passes no Lock, the old range can be drawn for at most 7 frames, once per
   object, 8 objects at most; run 64 met no such write.
   *Line:* `run 64 geometry store write signals (totals) ...; run 65: of those hits N from a check in an earlier frame
   (nx_dk_geometry_trust_frames 8, now 8), M buffer objects back to a check a frame`.
2. **The characters as drawn and as resolved, in the same frame** (`nx_dk_character_snapshot`, on in run 65's
   `game.toml`): on every second capture frame (eight in a run) the G-buffer's first colour target and its depth are
   saved right after each of the first three skinned draws of 1,500 vertices or more, and again when the title
   resolves those surfaces. Files `dk-f<frame>-a<n>-character-drawn-colour|depth-<surface>-<draw>.png` and
   `...-character-resolved-colour|depth-...png`. This is a read-back, not a counter: it uses run 56's capture path, stalls
   those eight frames only, and changes no draw. **It is the one measurement in this run; it is there because no fix for
   the characters is proven and this picture decides between the two halves left**: whole when drawn = something later
   removes them (then the draws between are the suspects, with their state); already in shards when drawn = the
   rasterizer's inputs differ from the probe's repeat of the draw.
3. Nothing else: the audio code is run 64's (attribution: one audio change measured per run). `sub_82185E38` (8.2 % of
   the mixer thread, with its resampler `sub_82188C28` 3.2 %) is the next function to write natively, then
   `sub_828C6740` (6.6 %): each needs its own run against the generated body, as `sub_8216D7E0` had. Not done in run 65:
   `sub_82185E38` is a dispatcher over ~40 resamplers reached through a table, so the first step is which of them the
   title uses (the profile names one), and that is a session of its own.

### AM4. Offline tests

`python tests/deko3d/test_run65.py` (new; `dk_run65_store.cpp`): **OK** (CONFIG once the build folder exists).

| Part | Result |
|---|---|
| FRAMES | 162,000 draws, every write with its signal: **0 ranges bound with other bytes** at `trust_frames` 1 and 8; 34,934 lookups answered from an earlier frame's check; bytes hashed 79,850 -> 34,733 KiB |
| RAW | 164 writes with no Lock: 1 old range bound (in the frame after the write), 8 objects sent back to a check a frame, the trust across frames ended (`trust_frames` 1) and **0 old ranges bound after that** |
| BENCH (this PC, not the console) | run 63's shape: 258 -> 64 ns a lookup, 2,677 -> 334 bytes hashed a lookup |
| SWITCH | AM1's table from the two logs and ELFs |
| INDICES | 24 probed skinned meshes of run 60: 0 vertices the GPU left unwritten, 0 vertices the indices never name |
| WIRING | `trust_frames` to the store; the snapshot last in the draw and at the resolve; the cvars |

`test_run64.py` (and through it `test_run53.py`, `test_run52.py`, `test_run51.py`): OK.

### AM5. Build

`out/console-runs/run65-build/` (`build_variants.sh ... dk-debug`, `-j3`, package s61 unchanged, `out/run65-build.log`, 0
errors, 0 warnings), built from commit `136ae66` (in `COMMIT` and in the ELF, without `-dirty`; the ELF holds the
`character-drawn-colour` name). `SHA256SUMS.txt`:

| File | Bytes | SHA-256 |
|---|---|---|
| `theft4-dk-debug.nro` | 47,323,333 | `6d93f08271a3d3ff404678de7486c3a1f54b27cbb772c6259718a69c7f684433` |
| `switch-game-dk-debug.elf` | 324,406,080 | `40fbc73167733076397232624f2f1c623c2cd9954e0fa46d53db38b7def45626` |
| `game.toml` | 1,456 | `f3afb6d16f4e3eb6a55a604825af71209cfb61e297df4cff1ad3828476b94f52` |
| `deko3d/gta4_deko3d_runtime.bin` (s61) | 100,311,296 | `f4d9fdc4f41cf3ad49b8af8853d768cda386efe108ba3b15dc3856da9b01789f` |
| `deko3d/gta4_deko3d_glsl.bin` (s61) | 43,826,659 | `b7ce84705982d12176f30136045556b1bf2668642e6f09610bae5759db419494` |

### AM6. Run 65: a short run, what to bring back

Standard clocks. Empty `sdmc:/switch/theft4/captures/`. Copy `theft4-dk-debug.nro`, `game.toml` and the whole `deko3d/`
of `run65-build/`. **6-8 minutes:** the intro cut-scene to its end, then **2-3 minutes on foot with the camera close to
people** (Roman, passers-by: the snapshot needs a skinned draw of 1,500 vertices or more, a near character), a short
drive, listen to the audio; `+` held 1 s. Bring back `game.log`, `game-crash.log`, `game-exit.log` and `captures/` into
`out/console-runs/run65-logs/`.

Neither change of run 65 has run on the console. Look at, in this order:
1. **`dk-f<frame>-a<n>-character-drawn-colour-*.png` against `...-character-resolved-colour-*.png` of the same frame**
   (and the two depth images): the near character whole when drawn, or already in shards.
2. `recorder time per drawn draw`: vertices and index range scan against run 64's 6.49 and 1.71 us.
3. `run 64 geometry store write signals ... run 65`: hits from an earlier frame; content changes without a signal and
   buffer objects back to a check a frame (0 and 0 expected; `now 1` instead of `now 8` means the trust ended).
4. `audio:`: as run 64 (no audio change in this run).

Off switches: `nx_dk_geometry_trust_frames = 1`, `nx_dk_character_snapshot = false`.
