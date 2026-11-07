// gta4_native deko3d back end: the game's shaders on the SD card. See dk_shader_package.h.
//
// The reader trusts nothing in the file: every count, size and offset is checked against the size of the file
// (and against the limits below) before anything is allocated or indexed. A file that does not pass is refused
// whole, with the first reason logged, and the package stays empty. The same rules are in
// tools/deko3d/gta4_deko3d_runtime_check.py, which the writer runs on what it wrote.

#include "dk_shader_package.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

namespace rex::graphics::gta4_native::dk {
namespace {

// gta4_deko3d_runtime.bin, version 2 (the layout is in gta4_deko3d_runtime_check.py).
constexpr uint32_t kVersion = 2;
constexpr uint32_t kHeaderBytes = 32;
constexpr uint32_t kRecordBytes = 48;
constexpr uint32_t kBlobEntryBytes = 8;
constexpr uint32_t kNoBlob = 0xFFFFFFFFu;
constexpr uint32_t kMaxTag = 4096;
constexpr uint32_t kMaxShaders = 1u << 20;
constexpr uint32_t kMaxBlobs = 1u << 22;
constexpr uint32_t kMaxExtra = 64u << 20;     // the real block is ~170 KB
constexpr uint32_t kMaxBlobBytes = 32u << 20;  // one DKSH, read whole when a draw first needs it (the real max: 0.7 MB)
constexpr uint32_t kMaxGlslTextBytes = 16u << 20;  // one GLSL text (the real max: 1.2 MB)
constexpr uint64_t kMaxFileBytes = 0x7FFFFFFF;  // fseek's long, and the blob offsets are u32

template <typename T>
T Get(const uint8_t* d, size_t at) {
  T v{};
  std::memcpy(&v, d + at, sizeof(T));
  return v;
}

uint64_t Align(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

uint64_t FileSize(FILE* f) {
  if (!f || std::fseek(f, 0, SEEK_END) != 0) {
    return 0;
  }
  const long end = std::ftell(f);
  return end > 0 ? uint64_t(end) : 0;
}

}  // namespace

ShaderPackage::~ShaderPackage() { Reset(); }

void ShaderPackage::Reset() {
  if (runtime_) {
    std::fclose(runtime_);
    runtime_ = nullptr;
  }
  if (glsl_) {
    std::fclose(glsl_);
    glsl_ = nullptr;
  }
  tag_.clear();
  shaders_.clear();
  blobs_.clear();
  glsl_texts_.clear();
}

bool ShaderPackage::ReadAt(FILE* f, uint64_t offset, void* out, size_t bytes) {
  return f && offset <= kMaxFileBytes && std::fseek(f, long(offset), SEEK_SET) == 0 &&
         std::fread(out, 1, bytes, f) == bytes;
}

bool ShaderPackage::Open(const std::string& dir) {
  Reset();
  const std::string runtime_path = dir + "/gta4_deko3d_runtime.bin";
  bool ok = false;
  try {
    ok = OpenRuntime(runtime_path);
  } catch (const std::exception& e) {
    REXLOG_ERROR("deko3d: shader package {}: {} while reading it", runtime_path, e.what());
  }
  if (!ok) {
    Reset();  // nothing partial stays: no table, no handle
    return false;
  }
  try {
    OpenGlsl(dir + "/gta4_deko3d_glsl.bin");
  } catch (const std::exception& e) {
    REXLOG_ERROR("deko3d: GLSL package: {} while reading it; it is not used", e.what());
    if (glsl_) {
      std::fclose(glsl_);
      glsl_ = nullptr;
    }
    glsl_texts_.clear();
  }
  return true;
}

bool ShaderPackage::OpenRuntime(const std::string& path) {
  runtime_ = std::fopen(path.c_str(), "rb");
  if (!runtime_) {
    REXLOG_ERROR("deko3d: no shader package {} (tools/deko3d/gta4_deko3d_runtime.py): the game's draws cannot "
                 "be drawn",
                 path);
    return false;
  }
  const uint64_t file_size = FileSize(runtime_);
  if (file_size < kHeaderBytes) {
    REXLOG_ERROR("deko3d: shader package {}: {} bytes, shorter than its {}-byte header (empty or truncated copy)", path,
                 file_size, kHeaderBytes);
    return false;
  }
  if (file_size > kMaxFileBytes) {
    REXLOG_ERROR("deko3d: shader package {}: {} bytes, more than the {} the reader takes", path, file_size,
                 kMaxFileBytes);
    return false;
  }
  uint8_t head[kHeaderBytes];
  if (!ReadAt(runtime_, 0, head, sizeof(head))) {
    REXLOG_ERROR("deko3d: shader package {}: its header could not be read", path);
    return false;
  }
  if (std::memcmp(head, "GDKR", 4) != 0) {
    REXLOG_ERROR("deko3d: shader package {}: not a GDKR file (tools/deko3d/gta4_deko3d_runtime.py)", path);
    return false;
  }
  const uint32_t version = Get<uint32_t>(head, 4), shader_count = Get<uint32_t>(head, 8),
                 blob_count = Get<uint32_t>(head, 12), tag_bytes = Get<uint32_t>(head, 16),
                 record_bytes = Get<uint32_t>(head, 20);
  const uint64_t declared_size = Get<uint64_t>(head, 24);
  if (version != kVersion) {
    REXLOG_ERROR("deko3d: shader package {}: version {}, this reader takes {}{}", path, version, kVersion,
                 version == 1 ? " (version 1 files have 44-byte records under 48-byte offsets: regenerate with "
                                "tools/deko3d/gta4_deko3d_runtime.py)"
                              : "");
    return false;
  }
  if (record_bytes != kRecordBytes) {
    REXLOG_ERROR("deko3d: shader package {}: record size {}, this reader takes {}", path, record_bytes, kRecordBytes);
    return false;
  }
  if (declared_size != file_size) {
    REXLOG_ERROR("deko3d: shader package {}: the file is {} bytes, its header says {} (truncated or wrong copy)", path,
                 file_size, declared_size);
    return false;
  }
  if (tag_bytes > kMaxTag || shader_count > kMaxShaders || blob_count > kMaxBlobs) {
    REXLOG_ERROR("deko3d: shader package {}: {} shaders, {} DKSH, tag of {} bytes: over the limits ({}, {}, {})", path,
                 shader_count, blob_count, tag_bytes, kMaxShaders, kMaxBlobs, kMaxTag);
    return false;
  }
  const uint64_t table_at = Align(kHeaderBytes + uint64_t(tag_bytes), 8);
  const uint64_t extra_at = table_at + uint64_t(kRecordBytes) * shader_count;
  if (extra_at > file_size) {
    REXLOG_ERROR("deko3d: shader package {}: the shader table ends at {}, past the end of the file ({})", path, extra_at,
                 file_size);
    return false;
  }
  tag_.assign(tag_bytes, '\0');
  if (tag_bytes && !ReadAt(runtime_, kHeaderBytes, tag_.data(), tag_bytes)) {
    REXLOG_ERROR("deko3d: shader package {}: its tag could not be read", path);
    return false;
  }
  std::vector<uint8_t> table(size_t(kRecordBytes) * shader_count);
  if (!table.empty() && !ReadAt(runtime_, table_at, table.data(), table.size())) {
    REXLOG_ERROR("deko3d: shader package {}: its shader table could not be read", path);
    return false;
  }

  // First pass: what each record claims, against the file, before the extra block is sized.
  uint64_t extra_end = extra_at;
  uint64_t prev_hash = 0;
  bool prev_late = false, have_prev = false;
  for (uint32_t i = 0; i < shader_count; ++i) {
    const uint8_t* r = table.data() + size_t(i) * kRecordBytes;
    const uint64_t hash = Get<uint64_t>(r, 0);
    const uint8_t stage = r[8], late = r[9], inputs = r[10], variants = r[11];
    if (stage > 1 || late > 1) {
      REXLOG_ERROR("deko3d: shader package {}: shader {}: stage {}, late {}", path, i, stage, late);
      return false;
    }
    // Run 55: the first reserved word carries the shader's flags (PackageShader::kFlag*); nothing else may be set.
    // Run 56: the last word is the io mask, only with kFlagIoMask.
    const uint32_t record_flags = Get<uint32_t>(r, 40);
    if ((record_flags & ~PackageShader::kFlagsKnown) != 0 ||
        (Get<uint32_t>(r, 44) != 0 && !(record_flags & PackageShader::kFlagIoMask))) {
      REXLOG_ERROR("deko3d: shader package {}: shader {}: reserved fields are not zero", path, i);
      return false;
    }
    if (have_prev && !(hash != prev_hash ? hash > prev_hash : (late != 0) > prev_late)) {
      REXLOG_ERROR("deko3d: shader package {}: shader {}: the table is not strictly ascending by (hash, late)", path, i);
      return false;
    }
    prev_hash = hash;
    prev_late = late != 0;
    have_prev = true;
    const struct {
      const char* what;
      uint64_t at, size;
    } parts[] = {{"inputs", Get<uint32_t>(r, 24), 4ull * inputs},
                 {"variants", Get<uint32_t>(r, 28), 8ull * variants},
                 {"texture types", Get<uint32_t>(r, 32), 32}};
    for (const auto& part : parts) {
      if (part.at < extra_at || part.at + part.size > file_size) {
        REXLOG_ERROR("deko3d: shader package {}: shader {}: {} at {} + {} bytes, outside [{}, {}]", path, i, part.what,
                     part.at, part.size, extra_at, file_size);
        return false;
      }
      extra_end = std::max(extra_end, part.at + part.size);
    }
  }
  if (extra_end - extra_at > kMaxExtra) {
    REXLOG_ERROR("deko3d: shader package {}: its details span {} bytes, more than the {} the reader takes", path,
                 extra_end - extra_at, kMaxExtra);
    return false;
  }
  std::vector<uint8_t> extra(size_t(extra_end - extra_at));
  if (!extra.empty() && !ReadAt(runtime_, extra_at, extra.data(), extra.size())) {
    REXLOG_ERROR("deko3d: shader package {}: its shader details could not be read", path);
    return false;
  }

  // The blob table first, so the variants can be checked against it.
  const uint64_t blobs_at = Align(extra_end, 256);
  const uint64_t data_at = blobs_at + uint64_t(kBlobEntryBytes) * blob_count;
  if (data_at > file_size) {
    REXLOG_ERROR("deko3d: shader package {}: the DKSH table ({} entries at {}) ends past the file ({})", path,
                 blob_count, blobs_at, file_size);
    return false;
  }
  std::vector<uint8_t> blob_table(size_t(kBlobEntryBytes) * blob_count);
  if (!blob_table.empty() && !ReadAt(runtime_, blobs_at, blob_table.data(), blob_table.size())) {
    REXLOG_ERROR("deko3d: shader package {}: its DKSH table could not be read", path);
    return false;
  }
  blobs_.resize(blob_count);
  for (uint32_t i = 0; i < blob_count; ++i) {
    const uint64_t offset = Get<uint32_t>(blob_table.data(), size_t(i) * kBlobEntryBytes);
    const uint64_t size = Get<uint32_t>(blob_table.data(), size_t(i) * kBlobEntryBytes + 4);
    if (size == 0 || size > kMaxBlobBytes || offset < data_at || offset % 256 != 0 || offset + size > file_size) {
      REXLOG_ERROR("deko3d: shader package {}: DKSH {}: offset {} size {} (1 to {} bytes, data starts at {}, "
                   "256-byte aligned, inside the {} bytes of the file)",
                   path, i, offset, size, kMaxBlobBytes, data_at, file_size);
      return false;
    }
    blobs_[i] = {uint32_t(offset), uint32_t(size)};
  }

  shaders_.resize(shader_count);
  uint64_t total_variants = 0, without_dksh = 0;
  for (uint32_t i = 0; i < shader_count; ++i) {
    const uint8_t* r = table.data() + size_t(i) * kRecordBytes;
    PackageShader& s = shaders_[i];
    s.hash = Get<uint64_t>(r, 0);
    s.pixel = r[8] == 1;
    s.late = r[9] != 0;
    const uint32_t inputs = r[10], variants = r[11];
    s.used_texture_mask = Get<uint32_t>(r, 12);
    s.specialization_mask = Get<uint32_t>(r, 16);
    s.glsl_index = Get<uint32_t>(r, 20);
    // In range: the first pass checked every offset against extra_at and the file, and extra covers up to extra_end.
    const size_t inputs_at = Get<uint32_t>(r, 24) - extra_at;
    const size_t variants_at = Get<uint32_t>(r, 28) - extra_at;
    const size_t types_at = Get<uint32_t>(r, 32) - extra_at;
    s.outputs = Get<uint32_t>(r, 36);
    s.flags = Get<uint32_t>(r, 40);
    s.io_mask = (s.flags & PackageShader::kFlagIoMask) ? Get<uint32_t>(r, 44) : 0;
    for (uint32_t k = 0; k < inputs; ++k) {
      const uint8_t* in = extra.data() + inputs_at + k * 4;
      if (in[0] > 39 || in[1] > 15 || in[2] > 2 || in[3] < 1 || in[3] > 4) {
        REXLOG_ERROR("deko3d: shader package {}: shader {} input {}: xenos {} uam {} numeric {} components {}", path, i,
                     k, in[0], in[1], in[2], in[3]);
        return false;
      }
      s.inputs.push_back({in[0], in[1], in[2], in[3]});
    }
    for (uint32_t k = 0; k < variants; ++k) {
      const uint8_t* v = extra.data() + variants_at + k * 8;
      const uint32_t blob = Get<uint32_t>(v, 4);
      if (blob != kNoBlob && blob >= blob_count) {
        REXLOG_ERROR("deko3d: shader package {}: shader {} variant {}: DKSH {} of {}", path, i, k, blob, blob_count);
        return false;
      }
      without_dksh += blob == kNoBlob;
      s.variants.push_back({Get<uint32_t>(v, 0), blob});
    }
    total_variants += variants;
    for (uint32_t k = 0; k < 32; ++k) {
      const uint8_t t = extra[types_at + k];
      if (t > 4) {
        REXLOG_ERROR("deko3d: shader package {}: shader {}: texture type {} for slot {}", path, i, t, k);
        return false;
      }
      s.texture_types[k] = t;
    }
  }
  REXLOG_INFO("deko3d: shader package {}: {} shaders, {} DKSH, tag '{}' ({} variants, {} without DKSH, {} bytes)", path,
              shader_count, blob_count, tag_, total_variants, without_dksh, file_size);
  return true;
}

void ShaderPackage::OpenGlsl(const std::string& path) {
  // The GLSL package, for what the PC could not compile (optional: absent is fine, malformed is refused whole).
  glsl_ = std::fopen(path.c_str(), "rb");
  if (!glsl_) {
    return;
  }
  const uint64_t file_size = FileSize(glsl_);
  uint8_t head[12] = {};
  const bool header = file_size >= sizeof(head) && file_size <= kMaxFileBytes && ReadAt(glsl_, 0, head, sizeof(head)) &&
                      std::memcmp(head, "GDKG", 4) == 0 && Get<uint32_t>(head, 4) == 1;
  const uint32_t count = header ? Get<uint32_t>(head, 8) : 0;
  if (!header || count > kMaxBlobs || sizeof(head) + 20ull * count > file_size) {
    REXLOG_ERROR("deko3d: GLSL package {}: not a usable GDKG file ({} bytes, {} texts); it is not used", path, file_size,
                 count);
    std::fclose(glsl_);
    glsl_ = nullptr;
    return;
  }
  std::vector<uint8_t> entries(size_t(count) * 20);
  if (!entries.empty() && !ReadAt(glsl_, 12, entries.data(), entries.size())) {
    REXLOG_ERROR("deko3d: GLSL package {}: its table could not be read; it is not used", path);
    std::fclose(glsl_);
    glsl_ = nullptr;
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> texts(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t offset = Get<uint32_t>(entries.data(), size_t(i) * 20 + 4);
    const uint64_t size = Get<uint32_t>(entries.data(), size_t(i) * 20 + 8);
    if (offset + size > file_size || size > kMaxGlslTextBytes) {
      REXLOG_ERROR("deko3d: GLSL package {}: text {} at {} + {} bytes, past the end of the file ({}) or over {} bytes; "
                   "it is not used",
                   path, i, offset, size, file_size, kMaxGlslTextBytes);
      std::fclose(glsl_);
      glsl_ = nullptr;
      return;
    }
    texts[i] = {uint32_t(offset), uint32_t(size)};
  }
  glsl_texts_ = std::move(texts);
  REXLOG_INFO("deko3d: GLSL package {}: {} texts ({} bytes)", path, count, file_size);
}

const PackageShader* ShaderPackage::Find(uint64_t hash, bool late) const {
  auto it = std::lower_bound(shaders_.begin(), shaders_.end(), std::make_pair(hash, late),
                             [](const PackageShader& s, const std::pair<uint64_t, bool>& key) {
                               return s.hash != key.first ? s.hash < key.first : s.late < key.second;
                             });
  return it != shaders_.end() && it->hash == hash && it->late == late ? &*it : nullptr;
}

std::vector<uint8_t> ShaderPackage::ReadBlob(uint32_t blob) {
  std::vector<uint8_t> out;
  if (blob >= blobs_.size()) {
    return out;
  }
  try {
    out.resize(blobs_[blob].second);  // at most kMaxBlobBytes (Open checked it)
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!ReadAt(runtime_, blobs_[blob].first, out.data(), out.size())) {
      out.clear();
    }
  } catch (const std::exception&) {
    out.clear();  // no memory for it: the variant counts as "without DKSH", not as a dead recorder thread
  }
  return out;
}

std::string ShaderPackage::VariantText(const PackageShader& shader, uint32_t value) {
  if (shader.glsl_index >= glsl_texts_.size()) {
    return {};
  }
  std::string text;
  try {
    text.assign(glsl_texts_[shader.glsl_index].second, '\0');  // at most kMaxGlslTextBytes (OpenGlsl checked it)
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!ReadAt(glsl_, glsl_texts_[shader.glsl_index].first, text.data(), text.size())) {
      return {};
    }
  } catch (const std::exception&) {
    return {};
  }
  // tools/deko3d/gta4_deko3d_shaders.py variant_text: the define right after the #version line.
  const std::string define = "#define SPIRV_CROSS_CONSTANT_ID_0 " + std::to_string(value) + "u";
  if (text.rfind("#version", 0) == 0) {
    const size_t nl = text.find('\n');
    if (nl != std::string::npos) {
      return text.substr(0, nl + 1) + define + "\n" + text.substr(nl + 1);
    }
    return text + "\n" + define;
  }
  return define + "\n" + text;
}

}  // namespace rex::graphics::gta4_native::dk
