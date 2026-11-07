// Host probe for the console's shader package reader (dk_shader_package.cpp, compiled unchanged against the stub
// logging header), driven by tests/deko3d/test_runtime_package.py.
//
//   shader_package_probe <dir> [<hash-hex>:<late 0|1> ...]
//
// Opens the package in <dir>, prints what the reader logged, then
//   RESULT open=<0|1> shaders=<n> blobs=<n> blob_reads=<ok>/<tried> blob_bytes=<n>
// and for each hash asked for
//   FIND <hash>:<late> found=<0|1> pixel=<0|1> inputs=<n> variants=<n> first_blob=<index|-1> text_types=<csv>
// The first 64 DKSH blobs are read back, as the recorder does when a draw first needs a variant.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dk_shader_package.h"

using rex::graphics::gta4_native::dk::PackageShader;
using rex::graphics::gta4_native::dk::ShaderPackage;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: shader_package_probe <dir> [<hash-hex>:<late> ...]\n");
    return 2;
  }
  ShaderPackage package;
  const bool opened = package.Open(argv[1]);
  size_t tried = 0, ok = 0, bytes = 0;
  if (opened) {
    for (uint32_t blob = 0; blob < package.blob_count() && blob < 64; ++blob) {
      ++tried;
      const std::vector<uint8_t> data = package.ReadBlob(blob);
      ok += !data.empty();
      bytes += data.size();
    }
  }
  std::printf("RESULT open=%d opened()=%d shaders=%zu blobs=%zu blob_reads=%zu/%zu blob_bytes=%zu\n", opened ? 1 : 0,
              package.opened() ? 1 : 0, package.shader_count(), package.blob_count(), ok, tried, bytes);
  for (int a = 2; a < argc; ++a) {
    const std::string arg = argv[a];
    const size_t colon = arg.find(':');
    const uint64_t hash = std::strtoull(arg.substr(0, colon).c_str(), nullptr, 16);
    const bool late = colon != std::string::npos && arg.substr(colon + 1) == "1";
    const PackageShader* s = package.Find(hash, late);
    std::printf("FIND %s found=%d", arg.c_str(), s ? 1 : 0);
    if (s) {
      long first_blob = -1;
      for (const PackageShader::Variant& v : s->variants) {
        if (v.blob != 0xFFFFFFFFu) {
          first_blob = long(v.blob);
          break;
        }
      }
      std::printf(" pixel=%d inputs=%zu variants=%zu first_blob=%ld", s->pixel ? 1 : 0, s->inputs.size(),
                  s->variants.size(), first_blob);
      // The GLSL of the shader with value 5 (empty without a GLSL package or without a glsl index).
      const std::string text = package.VariantText(*s, 5);
      std::printf(" glsl_len=%zu glsl_define=%d", text.size(),
                  text.find("#define SPIRV_CROSS_CONSTANT_ID_0 5u") != std::string::npos ? 1 : 0);
    }
    std::printf("\n");
  }
  return opened ? 0 : 3;
}
