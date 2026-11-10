# thirdparty/renut: ReNut's Direct3D-level renderer and what it is built with

The new renderer of this port (`docs/switch-port/18-renut-migration.md`). Everything here was taken from
**sa2-nx** (ChanseyIsTheBest's Sonic Adventure 2 port), commit `adf3a4e` (version 1.0.3.1), the commit this
repository's `thirdparty/plume-deko3d` was taken from. The first commit of this folder holds the files unchanged, so
`git log -p thirdparty/renut` shows every change made for GTA IV after it.

| Folder | From (sa2-nx `adf3a4e`) | What it is | Licence |
|---|---|---|---|
| `renut_engine/` | `app/src/renut_engine` | reNut's renderer (masterspike52) as banjo-nx and sa2-nx carry it: the Direct3D hooks, the device snapshot, the render thread and queue, draws, targets, textures, the shader pipeline, present; banjo-nx's tuning (`banjo_renut_*`, `display_sync.cpp`, `perf/`), sa2-nx's retarget and pacing | reNut has no licence file: see "Permission" below. banjo-nx's and sa2-nx's parts: GPL-3.0 |
| `plume/` | `app/thirdparty/plume` | plume (renderbag and contributors) with banjo-nx's deko3d back end (`plume_deko3d*`), UnleashedRecomp-NX's Switch changes and sa2-nx's changes. `contrib/` trimmed to what the build reads: volk, Vulkan-Headers `include/`, VulkanMemoryAllocator `include/`, D3D12MemoryAllocator `include/` and `src/` | MIT (`plume/LICENSE`); contrib: each folder's licence file |
| `reblue-XenosRecomp/` | `app/thirdparty/reblue-XenosRecomp` | XenosRecomp (hedge-dev; reblue fork) with reNut's and banjo-nx's changes: the Xenos microcode to HLSL translator the shader cache is built with. A host build tool. `thirdparty/` trimmed: fmt, xxHash, zstd `lib/` and `build/cmake/`, smol-v `source/`, the DXC headers. **The DXC binaries are not here** (they are not in sa2-nx's repository either): the build takes them from `tools/XenosRecomp/thirdparty/dxc-bin` | MIT (`reblue-XenosRecomp/LICENSE.md`); thirdparty: each folder's licence file |
| `imgui/` | `app/thirdparty/imgui` | Dear ImGui 1.92.5, the copy reNut's overlay compiles into the PC executable | MIT (`imgui/LICENSE.txt`) |

Not copied: `app/thirdparty/uam` (this repository has its own `thirdparty/uam`, which differs from sa2-nx's in five
files; the Switch build decides which one), plume's `examples/`, the test and documentation folders of the
translator's libraries.

## Permission

reNut (https://github.com/masterspike52/reNut) carries no licence file. ChanseyIsTheBest's ports reuse it with its
author's permission. For this project the owner has confirmed both: ChanseyIsTheBest's authorisation to reuse
renut-nx / banjo-nx / sa2-nx code here, and masterspike52's consent to the reuse of reNut's code (recorded on
2026-11-10, `docs/switch-port/18-renut-migration.md` section 7). Their names stay on the code: do not remove the
`banjo-nx` / `sa2-nx` / reNut comments when changing a file.

## Game data

Nothing here is game data. The shader cache the translator writes from the game's shader containers, and anything
captured from a running game, stay in the build tree (`out/`), never in the repository.
