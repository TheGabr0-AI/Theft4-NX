# shader-probe

PC tools of the deko3d feasibility probe (docs/switch-port/13-deko3d-spike.md,
D.3): the guest shaders of a shader storage file through the chain
microcode → SPIR-V → GLSL → DKSH. Nothing here contains game data; the
storage file stays where the PC build wrote it.

1. `build.sh <Visual Studio folder> <out dir>`: builds `xsh_to_spirv.exe` from
   the SDK's translator sources and the libraries of the PC build
   (`tools/pc-eu-base/build.bat` must have run).
2. `xsh_to_spirv <545407F2.xsh> <spv dir>`: one `N.vert.spv` / `N.frag.spv`
   per shader, one line per shader with the translation time.
3. SPIRV-Cross (Khronos, built separately):
   `spirv-cross --version 460 --vulkan-semantics N.vert.spv --output N.vert.glsl`.
4. `vk_glsl_to_deko.py N.vert.glsl N.vert.deko.glsl`: the six rewrites listed
   in its docstring (each one a construct the translator would have to emit
   differently for deko3d).
5. devkitPro's uam: `uam -s vert -o N.vert.dksh N.vert.deko.glsl`.

The probe measures whether the shaders compile, not whether they render the
same image.
