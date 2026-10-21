// gta4_native deko3d back end: the game's shaders on the SD card. See dk_shader_package.h.

#include "dk_shader_package.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>

namespace rex::graphics::gta4_native::dk {
namespace {

template <typename T>
T Get(const std::vector<uint8_t>& d, size_t at) {
  T v{};
  if (at + sizeof(T) <= d.size()) {
    std::memcpy(&v, d.data() + at, sizeof(T));
  }
  return v;
}

}  // namespace

ShaderPackage::~ShaderPackage() {
  if (runtime_) {
    std::fclose(runtime_);
  }
  if (glsl_) {
    std::fclose(glsl_);
  }
}

bool ShaderPackage::ReadAt(FILE* f, uint64_t offset, void* out, size_t bytes) {
  return f && std::fseek(f, long(offset), SEEK_SET) == 0 && std::fread(out, 1, bytes, f) == bytes;
}

bool ShaderPackage::Open(const std::string& dir) {
  const std::string runtime_path = dir + "/gta4_deko3d_runtime.bin";
  runtime_ = std::fopen(runtime_path.c_str(), "rb");
  uint32_t head[5] = {};
  if (!ReadAt(runtime_, 0, head, sizeof(head)) || std::memcmp(head, "GDKR", 4) != 0 || head[1] != 1) {
    REXLOG_ERROR("deko3d: no shader package {} (tools/deko3d/gta4_deko3d_runtime.py): the game's draws cannot "
                 "be drawn",
                 runtime_path);
    if (runtime_) {
      std::fclose(runtime_);
      runtime_ = nullptr;
    }
    return false;
  }
  const uint32_t shader_count = head[2], blob_count = head[3], tag_bytes = head[4];
  tag_.assign(tag_bytes, '\0');
  ReadAt(runtime_, 20, tag_.data(), tag_bytes);
  const uint64_t table_at = (20 + uint64_t(tag_bytes) + 7) & ~uint64_t(7);
  std::vector<uint8_t> table(size_t(shader_count) * 48);
  if (!ReadAt(runtime_, table_at, table.data(), table.size())) {
    REXLOG_ERROR("deko3d: {}: its shader table could not be read", runtime_path);
    return false;
  }
  // The variable parts follow the table and end before the blob table: read them in one go.
  uint64_t extra_end = table_at + table.size();
  for (uint32_t i = 0; i < shader_count; ++i) {
    const size_t e = size_t(i) * 48;
    const uint32_t inputs_at = Get<uint32_t>(table, e + 24), variants_at = Get<uint32_t>(table, e + 28),
                   types_at = Get<uint32_t>(table, e + 32);
    extra_end = std::max<uint64_t>({extra_end, inputs_at + 4ull * table[e + 10], variants_at + 8ull * table[e + 11],
                                    types_at + 32ull});
  }
  const uint64_t extra_at = table_at + table.size();
  std::vector<uint8_t> extra(size_t(extra_end - extra_at));
  if (!extra.empty() && !ReadAt(runtime_, extra_at, extra.data(), extra.size())) {
    REXLOG_ERROR("deko3d: {}: its shader details could not be read", runtime_path);
    return false;
  }
  shaders_.resize(shader_count);
  for (uint32_t i = 0; i < shader_count; ++i) {
    const size_t e = size_t(i) * 48;
    PackageShader& s = shaders_[i];
    s.hash = Get<uint64_t>(table, e);
    s.pixel = table[e + 8] == 1;
    s.late = table[e + 9] != 0;
    const uint32_t inputs = table[e + 10], variants = table[e + 11];
    s.used_texture_mask = Get<uint32_t>(table, e + 12);
    s.specialization_mask = Get<uint32_t>(table, e + 16);
    s.glsl_index = Get<uint32_t>(table, e + 20);
    const size_t inputs_at = Get<uint32_t>(table, e + 24) - extra_at;
    const size_t variants_at = Get<uint32_t>(table, e + 28) - extra_at;
    const size_t types_at = Get<uint32_t>(table, e + 32) - extra_at;
    s.outputs = Get<uint32_t>(table, e + 36);
    for (uint32_t k = 0; k < inputs; ++k) {
      const size_t at = inputs_at + k * 4;
      s.inputs.push_back({extra[at], extra[at + 1], extra[at + 2], extra[at + 3]});
    }
    for (uint32_t k = 0; k < variants; ++k) {
      s.variants.push_back({Get<uint32_t>(extra, variants_at + k * 8), Get<uint32_t>(extra, variants_at + k * 8 + 4)});
    }
    std::memcpy(s.texture_types, extra.data() + types_at, 32);
  }
  // The blob table is at the next 256-byte boundary after the variable parts.
  const uint64_t blobs_at = (extra_end + 255) & ~uint64_t(255);
  std::vector<uint8_t> blob_table(size_t(blob_count) * 8);
  if (!ReadAt(runtime_, blobs_at, blob_table.data(), blob_table.size())) {
    REXLOG_ERROR("deko3d: {}: its DKSH table could not be read", runtime_path);
    return false;
  }
  blobs_.resize(blob_count);
  for (uint32_t i = 0; i < blob_count; ++i) {
    blobs_[i] = {Get<uint32_t>(blob_table, i * 8), Get<uint32_t>(blob_table, i * 8 + 4)};
  }
  REXLOG_INFO("deko3d: shader package {}: {} shaders, {} DKSH, tag '{}'", runtime_path, shader_count, blob_count,
              tag_);

  // The GLSL package, for what the PC could not compile (optional).
  const std::string glsl_path = dir + "/gta4_deko3d_glsl.bin";
  glsl_ = std::fopen(glsl_path.c_str(), "rb");
  uint32_t glsl_head[3] = {};
  if (ReadAt(glsl_, 0, glsl_head, sizeof(glsl_head)) && std::memcmp(glsl_head, "GDKG", 4) == 0 &&
      glsl_head[1] == 1) {
    std::vector<uint8_t> entries(size_t(glsl_head[2]) * 20);
    if (ReadAt(glsl_, 12, entries.data(), entries.size())) {
      glsl_texts_.resize(glsl_head[2]);
      for (uint32_t i = 0; i < glsl_head[2]; ++i) {
        glsl_texts_[i] = {Get<uint32_t>(entries, i * 20 + 4), Get<uint32_t>(entries, i * 20 + 8)};
      }
    }
  } else if (glsl_) {
    std::fclose(glsl_);
    glsl_ = nullptr;
  }
  return true;
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
  out.resize(blobs_[blob].second);
  std::lock_guard<std::mutex> lock(file_mutex_);
  if (!ReadAt(runtime_, blobs_[blob].first, out.data(), out.size())) {
    out.clear();
  }
  return out;
}

std::string ShaderPackage::VariantText(const PackageShader& shader, uint32_t value) {
  if (shader.glsl_index >= glsl_texts_.size()) {
    return {};
  }
  std::string text(glsl_texts_[shader.glsl_index].second, '\0');
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!ReadAt(glsl_, glsl_texts_[shader.glsl_index].first, text.data(), text.size())) {
      return {};
    }
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
