# Third-party notices (deko3d work)

The deko3d work (docs/switch-port/16-deko3d-devlog.md) takes code from the projects below. Their licences apply
to their code; `patches/` holds every change made to it (`patches/README.md`). No game code or game data is in
any of it: the game's shaders are translated on the user's machine from the user's own copy and stay there.

| Project | Where here | Used for | Licence |
|---|---|---|---|
| [Liberty Recompiled](https://github.com/vaduur/LibertyRecompAndroid) (LibertyRecompAndroid `7c97c58`) | `glue/rexglue-sdk-main/src/graphics/gta4_native`, `include/rex/graphics/gta4_native`, the native hooks in `gta4-recomp/src`, parts of the SDK's Vulkan device and presenter, `patches/xenosrecomp-lra-7c97c58.patch` | the native GTA IV renderer | GPL-3.0 |
| [plume](https://github.com/renderbag/plume) by renderbag and contributors, with banjo-nx's deko3d back end (`plume_deko3d*`), as sa2-nx (ChanseyIsTheBest) carries it at `adf3a4e` | `thirdparty/plume-deko3d` (the repository's `thirdparty/plume` submodule, UnleashedRecomp's, is not touched; our change in `patches/plume-deko3d-sa2-nx-adf3a4e-teardown.patch`) | the deko3d device, swapchain, shaders and command lists of `nx_gpu = deko3d` | MIT (`thirdparty/plume-deko3d/LICENSE`) |
| [reNut](https://github.com/masterspike52/reNut) by masterspike52, as banjo-nx and sa2-nx (ChanseyIsTheBest) carry it, sa2-nx `adf3a4e` | `thirdparty/renut/renut_engine` | the Direct3D-level renderer of `gta4_renderer = renut` (`docs/switch-port/18-renut-migration.md`) | reNut has no licence file: reused with masterspike52's consent and ChanseyIsTheBest's authorisation (`thirdparty/renut/README.md`). banjo-nx's and sa2-nx's parts: GPL-3.0 |
| [plume](https://github.com/renderbag/plume), the whole library as sa2-nx carries it at `adf3a4e` (Vulkan, D3D12 and deko3d back ends), with volk, Vulkan-Headers, VulkanMemoryAllocator and D3D12MemoryAllocator | `thirdparty/renut/plume` | the graphics API layer ReNut's renderer draws through | MIT (`thirdparty/renut/plume/LICENSE`); `contrib/`: MIT and Apache-2.0, each folder's licence file |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (reblue fork) with reNut's and banjo-nx's changes, as sa2-nx carries it at `adf3a4e`, with fmt, xxHash, zstd, smol-v and the DXC headers | `thirdparty/renut/reblue-XenosRecomp` | the translator ReNut's shader cache is built with (a host build tool) | MIT (`thirdparty/renut/reblue-XenosRecomp/LICENSE.md`); fmt MIT, xxHash BSD-2-Clause, zstd BSD-3-Clause, smol-v MIT / public domain, DXC headers MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.5 by Omar Cornut, as sa2-nx carries it | `thirdparty/renut/imgui` | ReNut's overlay on the PC | MIT (`thirdparty/renut/imgui/LICENSE.txt`) |
| [uam](https://github.com/devkitPro/uam) by fincs, with SwitchWakerHD's and banjo-nx's patches, as sa2-nx carries it | `thirdparty/uam` | the deko3d shader compiler, in the NRO and as a PC tool | zlib (uam), MIT (Mesa's files in `mesa-imported/`), MPL-2.0 (SwitchWakerHD's patches) |
| [XenosRecomp](https://github.com/sonicnext-dev/XenosRecomp) | `tools/XenosRecomp` (submodule) and `patches/xenosrecomp-*` | Xenos microcode to HLSL | MIT |
| banjo-nx's deko3d shader tools, as sa2-nx carries them | `tools/deko3d/deko3d_glsl.py`, `deko3d_glsl_opt.py`, `deko3d_shader_stats.py`, `deko3d_build_tools.sh` | GLSL passes for uam and their check | GPL-3.0 |
| nfsmw-nx by StevensND and contributors (`deko3d-implementation`, `266f943`) | `printDeko3dConstants` (an idea, in our XenosRecomp patch); ported into `glue/rexglue-sdk-main/src/graphics/gta4_native/deko3d/`: `dk_core.*` (`nfsmw_dk_nucleo.*`), `dk_compiler.*` (`nfsmw_dk_compilador.*`), `dk_xenos.h` (`nfsmw_nativo_xenos.h`) and parts of `dk_recorder.cpp` (`nfsmw_dk_destinos.cpp`, `nfsmw_dk_dibujos.cpp`, `nfsmw_dk_texturas.cpp`) | the deko3d recorder of `gta4_native` (M3) | GPL-3.0 |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (`vulkan-sdk-1.4.304.0`) | not stored: a PC tool | SPIR-V to GLSL | Apache-2.0 |
| [deko3d](https://github.com/devkitPro/deko3d) by fincs | devkitPro's package (`libdeko3d.a`, `libdeko3dd.a`) | the Switch GPU API | zlib |

Design followed, no file taken: `tools/renut-gta4` (the game's Direct3D on a ring sink, with the device created
through the library's no-hardware path) follows the design of [reNut](https://github.com/masterspike52/reNut) by
masterspike52 as [renut-nx](https://github.com/ChanseyIsTheBest/renut-nx) and
[sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) (ChanseyIsTheBest) carry it, written here for GTA IV's Direct3D
from GTA IV's own code (`docs/switch-port/18-renut-migration.md`, section 7: what must be settled before reNut's own
files are put in this repository).

Not taken: Renut's renderer (sa2-nx `app/src/renut_engine`), which has no licence file; LibertyRecompAndroid's
`*_guest.inc` files and kabba536's `switch_audio_fast.cpp`, which are translated game code.
