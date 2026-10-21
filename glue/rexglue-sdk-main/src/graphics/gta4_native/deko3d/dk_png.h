// gta4_native deko3d back end: an uncompressed PNG writer for the captures (nx_dk_capture_s). Stored deflate blocks,
// so no compression library is needed; an RGB 1280x720 capture is 2.7 MB.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace rex::graphics::gta4_native::dk {

inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

// Writes `rgb` (width * height * 3 bytes, rows top to bottom) as a PNG file. False if the file cannot be written.
inline bool WritePng(const char* path, const uint8_t* rgb, uint32_t width, uint32_t height) {
  auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
  };
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, Crc32(out.data() + start, out.size() - start));
  };
  std::vector<uint8_t> ihdr;
  be32(ihdr, width);
  be32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB, deflate, no filter method, no interlace
  chunk("IHDR", ihdr);
  // The raw scanlines (filter byte 0), in stored deflate blocks of at most 65535 bytes, then Adler-32.
  const size_t row = size_t(width) * 3 + 1;
  std::vector<uint8_t> raw(row * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw[y * row] = 0;
    std::copy(rgb + size_t(y) * width * 3, rgb + size_t(y + 1) * width * 3, raw.begin() + y * row + 1);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t at = 0; at < raw.size() || raw.empty();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last) {
      break;
    }
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  be32(z, (b << 16) | a);
  chunk("IDAT", z);
  chunk("IEND", {});
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    return false;
  }
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace rex::graphics::gta4_native::dk
