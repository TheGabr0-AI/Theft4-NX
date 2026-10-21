# Changes to third-party code

Every change this port makes to code taken from another project, as a diff against the copy it came
from (docs/switch-port/16-deko3d-devlog.md). The changed files themselves are in the repository where
they are built from (`thirdparty/`, `tools/deko3d/`); the XenosRecomp ones are applied to a clone.

| Patch | Base | What |
|---|---|---|
| `xenosrecomp-lra-7c97c58.patch` | [XenosRecomp](https://github.com/sonicnext-dev/XenosRecomp) `fb32631` (MIT; `tools/XenosRecomp`, the submodule) | LibertyRecompAndroid `7c97c58`'s `cmake/dependency-patches/xenosrecomp.patch`, as it is (GPL-3.0, LRA): the GTA IV shader interface (`XENOS_RECOMP_GTA4`) |
| `xenosrecomp-gta4-deko3d.patch` | the tree above with LRA's patch | Theft4: the deko3d mode (`GTA4_DEKO3D`: constants in three uniform buffers, each fetch slot one texture at binding = slot, no push constants or 64-bit integers, no `-fvk-invert-y`), `XENOS_RECOMP_DEKO3D_SPIRV_DIR`. After nfsmw-nx's `printDeko3dConstants` (StevensND, GPL-3.0) |
| `uam-sa2-nx-adf3a4e-windows.patch` | sa2-nx `adf3a4e` `app/thirdparty/uam` (uam 1.1.0 by fincs with SwitchWakerHD's and banjo-nx's patches; zlib, MIT, MPL-2.0) | Theft4: the PC tool built with clang-cl: SRW locks for `c11/threads.h`, MSVC's `signbit`, `NOMINMAX`/`strtok_r`, an enum type in `tgsi_ureg.c`, short options without getopt, a 16 MB stack for the tool. The Switch library is unchanged |
| `deko3d_glsl_opt-sa2-nx-adf3a4e.patch` | sa2-nx `adf3a4e` `tools/deko3d_glsl_opt.py` (banjo-nx's, GPL-3.0) | Theft4: its place in `tools/deko3d`, `--uam`, the `_late` shader names, GTA IV's alpha-test variants in `--check` |

Taken without changes: `thirdparty/plume-deko3d` (plume by renderbag and contributors with banjo-nx's deko3d back end,
MIT; sa2-nx `adf3a4e`, only the deko3d back end and the render interface headers), `tools/deko3d/deko3d_glsl.py`,
`deko3d_shader_stats.py`, `deko3d_build_tools.sh` (banjo-nx's, GPL-3.0).

To apply the XenosRecomp ones: clone `tools/XenosRecomp` at `fb32631`, `git apply` the LRA patch, then the
deko3d one; build with `-DXENOS_RECOMP_GTA4=ON`.
