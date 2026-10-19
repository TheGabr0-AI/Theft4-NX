#!/bin/sh
# Builds xsh_to_spirv.exe (PC, Windows) from the SDK's translator sources and
# the libraries of the PC build (tools/pc-eu-base/build.bat must have run:
# glue/rexglue-sdk-main/out/win-amd64/*.lib). Visual Studio's clang, flags of
# the rexgpu-xenos target. Usage: build.sh <Visual Studio folder> <out dir>
set -e
VS=${1:?Visual Studio folder}
OUT=${2:?output folder}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$ROOT/glue/rexglue-sdk-main
LIB=$SDK/out/win-amd64
PCB=$ROOT/out/build/pc-eu-base
CXX="$VS/VC/Tools/Llvm/x64/bin/clang++.exe"
mkdir -p "$OUT"
DEFS="-DNOMINMAX -DREX_HAS_VULKAN=1 -DREX_PLATFORM_WINDOWS=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DWIN32_LEAN_AND_MEAN"
FLAGS="-march=x86-64-v3 -O2 -DNDEBUG -std=c++23 -D_DLL -D_MT -Xclang --dependent-lib=msvcrt -fno-strict-aliasing -ffp-model=strict -fno-char8_t -w"
INC="-I$SDK/include -I$SDK/thirdparty/renderdoc -I$SDK -I$PCB/rexglue-sdk/include -I$SDK/thirdparty/simde -I$SDK/thirdparty/fmt/include -I$SDK/thirdparty/spdlog/include -I$SDK/thirdparty/vulkan-headers/include -I$SDK/thirdparty/volk -I$SDK/thirdparty/xxHash -I$SDK/thirdparty/glslang -I$SDK/thirdparty/snappy -I$PCB/rexglue-sdk/thirdparty/snappy -I$PCB/include -isystem $SDK/thirdparty/tomlplusplus/include"
SRC="$ROOT/tools/shader-probe/xsh_to_spirv.cpp"
for f in flags.cpp format/ucode.cpp pipeline/shader/shader.cpp pipeline/shader/translator.cpp \
         pipeline/shader/translator_disasm.cpp pipeline/shader/spirv_builder.cpp \
         pipeline/shader/spirv.cpp pipeline/shader/spirv_translator.cpp \
         pipeline/shader/spirv_translator_alu.cpp pipeline/shader/spirv_translator_fetch.cpp \
         pipeline/shader/spirv_translator_memexport.cpp pipeline/shader/spirv_translator_rb.cpp \
         util/draw.cpp registers.cpp trace_writer.cpp; do
  SRC="$SRC $SDK/src/graphics/$f"
done
# clang (not GCC) finds addDecoration(id, SpecId, kFsiMsaaSpecializationId)
# ambiguous (int or const char*): a copy with an explicit int, FSI path only.
sed 's/kFsiMsaaSpecializationId);/int(kFsiMsaaSpecializationId));/'   "$SDK/src/graphics/pipeline/shader/spirv_translator_rb.cpp" > "$OUT/spirv_translator_rb_copy.cpp"
SRC=$(echo "$SRC" | sed "s|$SDK/src/graphics/pipeline/shader/spirv_translator_rb.cpp|$OUT/spirv_translator_rb_copy.cpp|")
objs=""
for s in $SRC; do
  o="$OUT/$(echo "$s" | sed 's|.*/src/graphics/||; s|.*/||; s|/|_|g').o"
  "$CXX" $DEFS $FLAGS $INC -c "$s" -o "$o"
  objs="$objs $o"
done
"$CXX" -fuse-ld=lld-link $objs -o "$OUT/xsh_to_spirv.exe" \
  "$LIB/rexruntime.lib" "$LIB/xxhash.lib" "$LIB/SPIRV.lib" "$LIB/glslang.lib" \
  "$LIB/MachineIndependent.lib" "$LIB/GenericCodeGen.lib" "$LIB/OSDependent.lib" \
  "$LIB/OGLCompiler.lib" "$LIB/spdlog.lib" "$LIB/fmt.lib" "$LIB/SPIRV-Tools.lib" "$LIB/snappy.lib"
cp "$LIB/rexruntime.dll" "$OUT/" 2>/dev/null || true
echo "built $OUT/xsh_to_spirv.exe"
