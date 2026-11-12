# ultimate/: renut_nx_ultimate_base, the new foundation of the port

The port's new base (`docs/switch-port/20-ultimate-base-migration.md`): ChanseyIsTheBest's
[renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base), commit
`8de5d51a66d14c9bceb90811a818c6ee3906057f` (2026-11-13, "Ports 123-128 and an index for docs/"), the commit named in
`tools/ultimate/BASE_COMMIT`.

The first commit of this folder holds the base's files **unchanged** (byte for byte, as `git archive` of that commit
gives them), so `git log -p ultimate` shows every change made for GTA IV after it. Two parts of the base are not in
the repository:

| Not here | Why | How to get it |
|---|---|---|
| `sdk/thirdparty/` (FFmpeg, simde, SDL3, glslang, SPIRV-Tools, Vulkan headers and loader, tracy, imgui, fmt, spdlog, ...; 372 MB) | size; unchanged third-party code | `powershell -File tools/ultimate/fetch_sdk_thirdparty.ps1` (from the same pinned commit; ignored by git) |
| `pgo/`, `pgo-inplace/` | Beautiful Katamari's PGO profiles: they describe Katamari's code, not GTA IV's (the porting guide removes them) | GTA IV's own profiles, trained on the console, go there later |

Everything else (the app with Renut's renderer, plume, reblue-XenosRecomp, uam, SMAA; the SDK's own code; the tools;
the docs; the test settings) is as the base has it, Beautiful Katamari's game layer included until it is replaced by
GTA IV's (phase U3).

## Credits and licences

As the base's [README](README.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) state: **ChanseyIsTheBest** (the
ports and the base); **Tom Clay** (ReXGlue SDK) and the **Xenia** team; **reNut and masterspike52** (reNut's
Direct3D-level renderer, reused with its author's permission: for this project the owner confirmed both authors'
permission on 2026-11-10, `docs/switch-port/18-renut-migration.md` §7); **StevensND** (nfsmw-nx: the SDK's Switch
layer); **nebadasSwifty** (masseffect-nx); **hedge-dev** (XenosRecomp); **plume** (renderbag); **deko3d** and **uam**
(fincs); **devkitPro** and **switchbrew**; KhronosGroup (SPIRV-Cross); and every port whose fixes the base carries
(its README lists them). The base's `LICENSE` and the licence of each third-party folder apply to their files. Do not
remove the authors' names or comments when changing a file.

Nothing here is game data. Code generated from GTA IV, shader packages, caches, NRO/ELF files and logs stay out of the
repository (the base's own `.gitignore` in this folder already says so).
