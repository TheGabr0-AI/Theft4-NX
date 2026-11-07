// Host probe for dk_dksh.h (what the console reads from a DKSH before loading it), driven by
// tests/deko3d/test_dksh_scratch.py.
//
//   dksh_scratch_probe synthetic          parser cases built in memory
//   dksh_scratch_probe <package dir>      every DKSH blob of the package (dk_shader_package.cpp, unchanged)
//
// Prints one line per synthetic case ("CASE <name> ok=<0|1> scratch=<n> code=<n>") or
//   PACKAGE blobs=<n> parsed=<n> failed=<n> over_2048=<n> over_queue=<n> max=<n> queue=<n>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dk_dksh.h"
#include "dk_shader_package.h"

using namespace rex::graphics::gta4_native::dk;

namespace {

// A DKSH with one program: header (24 bytes) + program header (32 bytes, scratch at +20) padded to 256, then code.
std::vector<uint8_t> Make(uint32_t scratch, uint32_t programs = 1, uint32_t programs_offset = 24, uint32_t control = 256,
                          uint32_t code = 256, uint32_t magic = 0x48534B44u, size_t total = 0) {
  std::vector<uint8_t> b(total ? total : size_t(control) + code, 0);
  const uint32_t h[6] = {magic, 24, control, code, programs_offset, programs};
  std::memcpy(b.data(), h, sizeof(h));
  if (size_t(programs_offset) + 24 <= b.size()) {
    std::memcpy(b.data() + programs_offset + 20, &scratch, 4);
  }
  return b;
}

void Case(const char* name, const std::vector<uint8_t>& dksh) {
  DkshProgram p;
  const bool ok = ParseDksh(dksh, p);
  std::printf("CASE %s ok=%d scratch=%u code=%u programs=%u\n", name, ok ? 1 : 0, ok ? p.per_warp_scratch : 0,
              ok ? p.code_bytes : 0, ok ? p.programs : 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dksh_scratch_probe synthetic|<package dir>\n");
    return 2;
  }
  if (std::string(argv[1]) == "synthetic") {
    Case("zero", Make(0));
    Case("default_queue", Make(2048));
    Case("largest_seen", Make(92672));
    Case("exact_queue", Make(kQueueScratchBytes));
    Case("over_queue", Make(kQueueScratchBytes + 512));
    Case("two_programs", Make(512, 2));
    Case("no_programs", Make(0, 0));
    Case("bad_magic", Make(0, 1, 24, 256, 256, 0x12345678u));
    Case("truncated_header", std::vector<uint8_t>(20, 0));
    Case("empty", std::vector<uint8_t>());
    Case("field_ends_at_control_end", Make(7, 1, 232));  // 232 + 20 + 4 == 256: the last word of the control section
    Case("field_past_control_end", Make(7, 1, 240));
    Case("programs_off_huge", Make(0, 1, 0xFFFFFFF0u));
    Case("control_past_file", Make(0, 1, 24, 4096, 4096, 0x48534B44u, 512));
    return 0;
  }
  ShaderPackage package;
  if (!package.Open(argv[1])) {
    std::printf("PACKAGE open failed\n");
    return 3;
  }
  size_t parsed = 0, failed = 0, over_2048 = 0, over_queue = 0;
  uint32_t max = 0;
  for (uint32_t blob = 0; blob < package.blob_count(); ++blob) {
    const std::vector<uint8_t> data = package.ReadBlob(blob);
    DkshProgram p;
    if (data.empty() || !ParseDksh(data, p)) {
      ++failed;
      continue;
    }
    ++parsed;
    over_2048 += p.per_warp_scratch > 2048;
    over_queue += p.per_warp_scratch > kQueueScratchBytes;
    if (p.per_warp_scratch > max) {
      max = p.per_warp_scratch;
    }
  }
  std::printf("PACKAGE blobs=%zu parsed=%zu failed=%zu over_2048=%zu over_queue=%zu max=%u queue=%u\n",
              size_t(package.blob_count()), parsed, failed, over_2048, over_queue, max, kQueueScratchBytes);
  return 0;
}
