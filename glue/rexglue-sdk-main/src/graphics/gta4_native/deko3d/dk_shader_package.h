// gta4_native deko3d back end: the game's shaders on the SD card (docs/switch-port/16-deko3d-devlog.md, M3 B3).
//
// gta4_deko3d_runtime.bin (tools/deko3d/gta4_deko3d_runtime.py, format there): every shader of the game by its
// hash, with what binding it needs (its vertex inputs and their uam locations, the texture type of each fetch
// slot, its colour outputs) and the PC's DKSH of each specialization variant; gta4_deko3d_glsl.bin
// (gta4_deko3d_package.py) has the GLSL of the variants uam could not compile on the PC, for the console's uam.
// Both are made on the PC from the user's own game and copied to sdmc:/switch/theft4/deko3d/: game data, never
// in the repository or the NRO. Only the tables are read at start; a DKSH or a text is read from the card when
// a draw first needs it.

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace rex::graphics::gta4_native::dk {

struct PackageShader {
  struct Input {
    uint8_t xenos_location = 0;  // core::ConvertVertexUsageToLocation
    uint8_t uam_location = 0;
    uint8_t numeric = 0;  // 0 float, 1 int, 2 uint
    uint8_t components = 4;
  };
  struct Variant {
    uint32_t value = 0;
    uint32_t blob = 0xFFFFFFFFu;  // 0xFFFFFFFF: no DKSH from the PC
  };
  uint64_t hash = 0;
  bool pixel = false;
  bool late = false;
  uint32_t used_texture_mask = 0;
  uint32_t specialization_mask = 0;
  uint32_t glsl_index = 0xFFFFFFFFu;
  uint32_t outputs = 0;
  std::vector<Input> inputs;
  std::vector<Variant> variants;
  uint8_t texture_types[32] = {};  // 0 none, 1 2D, 2 2D array, 3 3D, 4 cube
};

class ShaderPackage {
 public:
  ~ShaderPackage();
  // Reads the tables of both files in `dir`. False (logged) without the runtime package.
  bool Open(const std::string& dir);
  bool opened() const { return runtime_ != nullptr; }
  size_t shader_count() const { return shaders_.size(); }
  size_t blob_count() const { return blobs_.size(); }
  const std::string& tag() const { return tag_; }

  const PackageShader* Find(uint64_t hash, bool late) const;
  // The PC's DKSH of that variant (read from the card); empty if none.
  std::vector<uint8_t> ReadBlob(uint32_t blob);
  // The GLSL of a shader with SPIRV_CROSS_CONSTANT_ID_0 defined to `value` (the PC tool's variant text);
  // empty if none.
  std::string VariantText(const PackageShader& shader, uint32_t value);

 private:
  bool ReadAt(FILE* f, uint64_t offset, void* out, size_t bytes);

  std::mutex file_mutex_;
  FILE* runtime_ = nullptr;
  FILE* glsl_ = nullptr;
  std::string tag_;
  std::vector<PackageShader> shaders_;  // sorted by (hash, late)
  std::vector<std::pair<uint32_t, uint32_t>> blobs_;  // offset, size
  std::vector<std::pair<uint32_t, uint32_t>> glsl_texts_;  // offset, size
};

}  // namespace rex::graphics::gta4_native::dk
