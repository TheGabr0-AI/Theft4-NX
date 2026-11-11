#!/bin/bash
# Builds the NRO variants of the game and puts each one, named, with its ELF, in one folder.
#
#   tools/switch-game/build_variants.sh <dest-dir> <variant> [<variant> ...]
#
#   variant   NRO                 build dir             what
#   dk-debug  theft4-dk-debug.nro out/build/switch-game-dk   deko3d renderer, libdeko3dd (checks every call)
#   dk        theft4-dk.nro       out/build/switch-game-dk   deko3d renderer, libdeko3d
#   nvk       theft4.nro          out/build/switch-game      the Vulkan/NVK build (the one up to run 34)
#
# There is no "theft4-debug.nro": the NVK configuration has no debug variant (REXGLUE_SWITCH_DEKO3D_DEBUG only
# changes the deko3d library). Each variant sets its NACP name (the homebrew menu shows it) and its NRO path
# through THEFT4_SWITCH_APP_NAME / THEFT4_SWITCH_NRO_OUTPUT; the two deko3d variants share one build dir and are
# built one after the other (the ELF is copied out after each). At most -j3 and never two builds at once
# (docs/switch-port/09-rules.md section 7).
set -euo pipefail

export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64  # not the inherited C:/devkitPro: CMake 4 misreads it
# The NVK build dir is configured with tools/switch-game/native_python.sh as Python3 (SPIRV-Tools needs a native
# one; it reads this variable, and cmake AND ninja must see it).
export THEFT4_NATIVE_PYTHON=${THEFT4_NATIVE_PYTHON:-/d/Python/python.exe}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
dest=${1:?usage: build_variants.sh <dest-dir> <variant>...}; shift
[ $# -gt 0 ] || { echo "no variant given"; exit 2; }
mkdir -p "$dest"
dest=$(cd "$dest" && pwd)

if ps -ef | grep -E '[n]inja|[c]c1plus' >/dev/null; then
  echo "another ninja/cc1plus is running: not starting a build"; exit 3
fi

for variant in "$@"; do
  case $variant in
    dk-debug) dir=switch-game-dk; nro=theft4-dk-debug.nro; name="Theft4 DK (debug)"
              opts="-DREXGLUE_SWITCH_NO_NVK=ON -DREXGLUE_SWITCH_DEKO3D=ON -DREXGLUE_SWITCH_DEKO3D_DEBUG=ON" ;;
    dk)       dir=switch-game-dk; nro=theft4-dk.nro; name="Theft4 DK"
              opts="-DREXGLUE_SWITCH_NO_NVK=ON -DREXGLUE_SWITCH_DEKO3D=ON -DREXGLUE_SWITCH_DEKO3D_DEBUG=OFF" ;;
    nvk)      dir=switch-game; nro=theft4.nro; name="Theft4"
              opts="" ;;
    # Run 51: NVK + Liberty Recompiled's own renderer (nx_gpu = lra). Game-derived inputs from the environment (never
    # written in the repository): THEFT4_LRA_PRIVATE_ROOT (LibertyRecompLib: shader/shader_cache.cpp, font_atlases),
    # THEFT4_LRA_OVERRIDE_CPP (shader_override_cache.cpp of the PC build), THEFT4_LRA_SMOLV_ROOT (smol-v's source/).
    # NVK is the portlibs libvulkan.a with the NAK flip fix (nak_flip_patch.py), made here.
    lra)      dir=switch-game-lra; nro=theft4-lra.nro; name="Theft4 LRA"
              lra_build="$ROOT/out/build/$dir"
              mkdir -p "$lra_build/nvk"
              "$THEFT4_NATIVE_PYTHON" -I "$ROOT/tools/switch-game/nak_flip_patch.py" \
                  "$DEVKITPRO/portlibs/switch/lib/libvulkan.a" "$lra_build/nvk/libvulkan.a"
              opts="-DTHEFT4_SWITCH_LRA=ON -DTHEFT4_LRA_PRIVATE_ROOT=${THEFT4_LRA_PRIVATE_ROOT:?} \
                    -DTHEFT4_LRA_OVERRIDE_CPP=${THEFT4_LRA_OVERRIDE_CPP:?} -DTHEFT4_LRA_SMOLV_ROOT=${THEFT4_LRA_SMOLV_ROOT:?} \
                    -DTHEFT4_SWITCH_VULKAN_LIBRARIES=$lra_build/nvk/libvulkan.a;expat;zstd;z;nx \
                    -DREXGLUE_OUTPUT_DIR=$lra_build/rexglue-out \
                    -DPython3_EXECUTABLE=$ROOT/tools/switch-game/native_python.sh -DTHEFT4_HOST_PYTHON=/usr/bin/python3" ;;
    # ReNut's renderer on plume's deko3d back end (18-renut-migration.md, step R5): no Vulkan driver, not the old
    # deko3d renderer (both hook the same Direct3D functions). Its game shaders come from the PC build
    # (tools/renut-gta4/renut_switch_shaders.ps1 -> out/renut-switch, game data), embedded in the NRO.
    renut)    dir=switch-game-renut; nro=theft4-renut.nro; name="Theft4 ReNut"
              opts="-DREXGLUE_SWITCH_NO_NVK=ON -DREXGLUE_SWITCH_DEKO3D=OFF -DTHEFT4_SWITCH_RENUT=ON" ;;
    *) echo "unknown variant $variant (dk-debug, dk, nvk, lra, renut)"; exit 2 ;;
  esac
  build="$ROOT/out/build/$dir"
  echo "=== $variant: configure $(date +%T)"
  cmake -S "$ROOT/tools/switch-game" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchains/switch-libnx.cmake" -DCMAKE_BUILD_TYPE=Release $opts \
        -DTHEFT4_SWITCH_APP_NAME="$name" -DTHEFT4_SWITCH_NRO_OUTPUT="$build/$nro"
  echo "=== $variant: build $(date +%T)"
  # /usr/bin/ninja, not devkitPro's first in PATH (it cannot re-run /usr/bin/cmake.exe)
  /usr/bin/ninja -C "$build" -j3 switch-game-commit   # the commit stamp first (a build after a new commit needs two runs)
  /usr/bin/ninja -C "$build" -j3 switch-game_nro
  [ -s "$build/$nro" ] || { echo "$variant: $build/$nro missing or empty"; exit 4; }
  cp -f "$build/$nro" "$dest/$nro"
  cp -f "$build/switch-game.elf" "$dest/switch-game-$variant.elf"
  git -C "$ROOT" describe --always --dirty 2>/dev/null > "$dest/COMMIT" || true
  echo "=== $variant: done $(date +%T): $dest/$nro"
done
# Run 48: the deko3d shader package goes with the NRO, in deko3d/ as on the card (sdmc:/switch/theft4/deko3d/), so the
# folder is copied to the SD as it is. THEFT4_DK_PACKAGE picks the package (default: run 47's structured one).
case " $* " in
  *" dk-debug "*|*" dk "*)
    pkg=${THEFT4_DK_PACKAGE:-/c/dev/Theft4/eu-private/deko3d-out-s47}
    mkdir -p "$dest/deko3d"
    for f in gta4_deko3d_runtime.bin gta4_deko3d_glsl.bin; do
      [ -s "$pkg/$f" ] || { echo "deko3d package: $pkg/$f missing"; exit 5; }
      cp -f "$pkg/$f" "$dest/deko3d/$f"
    done
    echo "$pkg" > "$dest/deko3d/PACKAGE"
    ( cd "$dest/deko3d" && sha256sum *.bin ) ;;
esac
# Run 51: the LRA renderer's font atlases (game-derived, optional: without them the stock font texture stays), in
# lra/font_atlases as on the card (sdmc:/switch/theft4/lra/font_atlases, THEFT4_LRA_FONT_ROOT).
case " $* " in
  *" lra "*)
    mkdir -p "$dest/lra"
    cp -rf "$THEFT4_LRA_PRIVATE_ROOT/font_atlases" "$dest/lra/" ;;
esac
( cd "$dest" && sha256sum *.nro )
