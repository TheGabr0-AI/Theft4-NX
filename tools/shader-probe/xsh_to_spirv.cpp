// xsh_to_spirv: the guest shaders of a ReXGlue shader storage file (.xsh,
// guest Xenos microcode, the same on every platform) translated to SPIR-V by
// the SDK's SpirvShaderTranslator, as the Vulkan backend does on the Switch,
// one .spv per shader. Probe for docs/switch-port/13-deko3d-spike.md (D.3):
// the .spv files go to SPIRV-Cross (GLSL) and then to uam (DKSH).
//
// Usage: xsh_to_spirv <file.xsh> <output dir> [max shaders]
//
// The modification is the translator's default with the registers of a
// common draw (interpolators: all those the vertex shader writes; pixel
// shaders: all 16). Device features: those of NVK on the Tegra X1 (run 16,
// docs/switch-port/11-vulkan-probe.md), float controls off. One line per
// shader on stdout: index, type, hash, ucode dwords, result, SPIR-V bytes,
// translation ms.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <SPIRV/spirv.hpp>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>
#include <xxhash.h>

namespace {

#pragma pack(push, 1)
struct StoredHeader {
  uint64_t ucode_data_hash;
  uint32_t ucode_dword_count : 31;
  uint32_t type : 1;
};
#pragma pack(pop)

}  // namespace

int main(int argc, char** argv) {
  using namespace rex::graphics;
  if (argc < 3) {
    std::fprintf(stderr, "usage: xsh_to_spirv <file.xsh> <output dir> [max shaders]\n");
    return 1;
  }
  const size_t max_shaders = argc > 3 ? size_t(std::strtoul(argv[3], nullptr, 10)) : SIZE_MAX;
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  uint32_t file_header[2];
  if (std::fread(file_header, sizeof(file_header), 1, f) != 1 || file_header[0] != 0x48534558) {
    std::fprintf(stderr, "not a shader storage file (magic)\n");
    return 1;
  }

  SpirvShaderTranslator::Features features(false);
  features.spirv_version = spv::Spv_1_5;
  features.max_storage_buffer_range = UINT32_MAX;
  features.full_draw_index_uint32 = true;
  features.vertex_pipeline_stores_and_atomics = true;
  features.fragment_stores_and_atomics = true;
  features.clip_distance = true;
  features.cull_distance = true;
  features.image_view_format_swizzle = true;
  features.demote_to_helper_invocation = true;
  features.sample_rate_shading = true;
  // Host render targets (no FSI on NVK), no 2x MSAA emulation, scale 1.
  SpirvShaderTranslator translator(features, true, true, false);

  rex::string::StringBuffer disasm;
  std::vector<uint32_t> ucode;
  size_t index = 0, ok = 0, failed = 0;
  double total_ms = 0.0;
  StoredHeader header;
  while (index < max_shaders && std::fread(&header, sizeof(header), 1, f) == 1) {
    ucode.resize(header.ucode_dword_count);
    if (header.ucode_dword_count &&
        std::fread(ucode.data(), header.ucode_dword_count * 4, 1, f) != 1) {
      std::fprintf(stderr, "truncated record %zu\n", index);
      break;
    }
    if (XXH3_64bits(ucode.data(), ucode.size() * 4) != header.ucode_data_hash) {
      std::fprintf(stderr, "hash mismatch at record %zu\n", index);
      break;
    }
    const auto type = header.type ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex;
    Shader shader(type, header.ucode_data_hash, ucode.data(), ucode.size());
    const auto start = std::chrono::steady_clock::now();
    shader.AnalyzeUcode(disasm);
    SpirvShaderTranslator::Modification modification(0);
    if (type == xenos::ShaderType::kVertex) {
      modification = SpirvShaderTranslator::Modification(
          translator.GetDefaultVertexShaderModification(
              shader.GetDynamicAddressableRegisterCount(63)));
      modification.vertex.interpolator_mask = shader.writes_interpolators();
    } else {
      modification = SpirvShaderTranslator::Modification(
          translator.GetDefaultPixelShaderModification(
              shader.GetDynamicAddressableRegisterCount(63)));
      modification.pixel.interpolator_mask = 0xFFFF;
    }
    Shader::Translation* translation = shader.GetOrCreateTranslation(modification.value);
    const bool translated = translator.TranslateAnalyzedShader(*translation);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    total_ms += ms;
    const std::vector<uint8_t>& binary = translation->translated_binary();
    const bool good = translated && translation->is_valid() && !binary.empty();
    if (good) {
      ++ok;
      std::string path = std::string(argv[2]) + "/" +
                         std::to_string(index) + (type == xenos::ShaderType::kVertex ? ".vert"
                                                                                       : ".frag") +
                         ".spv";
      if (FILE* out = std::fopen(path.c_str(), "wb")) {
        std::fwrite(binary.data(), 1, binary.size(), out);
        std::fclose(out);
      }
    } else {
      ++failed;
    }
    std::printf("%zu %s %016llX %u %s %zu %.3f\n", index,
                type == xenos::ShaderType::kVertex ? "vert" : "frag",
                (unsigned long long)header.ucode_data_hash, unsigned(header.ucode_dword_count),
                good ? "ok" : "failed", binary.size(), ms);
    ++index;
  }
  std::fclose(f);
  std::printf("total %zu shaders: %zu translated, %zu failed, %.1f ms (%.3f ms each)\n", index,
              ok, failed, total_ms, index ? total_ms / double(index) : 0.0);
  return failed ? 2 : 0;
}
