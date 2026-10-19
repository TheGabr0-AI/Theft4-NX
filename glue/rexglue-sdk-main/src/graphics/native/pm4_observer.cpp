// Native renderer, step N1: the PM4 ring reader in "observe" mode
// (pm4_observer.h).

#include "pm4_observer.h"

#include <cinttypes>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/types.h>

REXCVAR_DEFINE_INT32(native_observe_frames, 0, "GPU/Native",
                     "N1: frames the PM4 reader observes (counts beside the emulated renderer's, "
                     "one CSV row per frame); 0 = off; read once, at the first ring execution");
REXCVAR_DEFINE_STRING(native_observe_csv, "native-observe.csv", "GPU/Native",
                      "N1: the per-frame CSV of the PM4 reader against the emulated renderer");

namespace rex::graphics::native {

namespace {

constexpr uint32_t kRegisterCount = 0x10000;
constexpr uint32_t kRbSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
constexpr uint32_t kRbColorInfo = 0x2001;    // RB_COLOR_INFO, then DEPTH, COLOR1-3 to 0x2005
constexpr uint32_t kRbColorMask = 0x2104;
constexpr uint32_t kRbModeControl = 0x2208;
constexpr uint32_t kEdramModeCopy = uint32_t(xenos::EdramMode::kCopy);
constexpr int kMaxIndirectDepth = 4;

}  // namespace

Pm4Observer* Pm4Observer::Get() {
  static Pm4Observer* instance = [] {
    const int32_t frames = REXCVAR_GET(native_observe_frames);
    return frames > 0 ? new Pm4Observer(uint32_t(frames), REXCVAR_GET(native_observe_csv))
                      : nullptr;
  }();
  return instance && !instance->finished_ ? instance : nullptr;
}

Pm4Observer::Pm4Observer(uint32_t frames, std::string csv_path)
    : frames_wanted_(frames), csv_path_(std::move(csv_path)), registers_(kRegisterCount, 0) {
  csv_ = std::fopen(csv_path_.c_str(), "w");
  if (csv_) {
    std::fputs("frame,reader_draws,emulated_draws,reader_resolves,emulated_resolves,"
               "reader_rt_changes,emulated_rt_changes,reader_predicated,reader_indirect_buffers,"
               "reader_packets,equal\n",
               csv_);
  }
  REXGPU_INFO("[NativeObserve] PM4 reader in observe mode for {} frames; CSV {} ({})", frames,
              csv_path_, csv_ ? "open" : "NOT opened");
}

void Pm4Observer::ObservePrimary(memory::Memory* memory, uint32_t primary_ptr,
                                 uint32_t primary_size, uint32_t read_index,
                                 uint32_t write_index) {
  const auto* words = memory->TranslatePhysical<const uint32_t*>(primary_ptr);
  DecodeBuffer(memory, words, primary_size - 1, primary_size, read_index * 4, write_index * 4, 0);
}

// words: host pointer of the buffer (big-endian dwords); offsets in bytes,
// wrapping at size_bytes for the ring (a power of two), linear for an
// indirect buffer (write_offset = its size).
void Pm4Observer::DecodeBuffer(memory::Memory* memory, const uint32_t* words,
                               uint32_t /*base_bytes_mask*/, uint32_t size_bytes,
                               uint32_t read_offset, uint32_t write_offset, int depth) {
  const uint32_t size_words = size_bytes / 4;
  if (!size_words) return;
  uint32_t at = (read_offset / 4) % size_words;
  const uint32_t end = (write_offset / 4) % size_words;
  auto take = [&]() {
    const uint32_t v = rex::byte_swap(words[at]);
    at = (at + 1) % size_words;
    return v;
  };
  auto remaining = [&]() { return (end + size_words - at) % size_words; };
  // A linear indirect buffer ends exactly at its size: there end == 0 == at
  // only at the end, and remaining() counts the words up to the wrap.
  const bool linear = write_offset == size_bytes && read_offset == 0;
  uint32_t left = linear ? size_words : remaining();
  while (left) {
    const uint32_t packet = take();
    --left;
    if (packet == 0) continue;
    const uint32_t type = packet >> 30;
    if (type == 0) {
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (count > left) return;
      const uint32_t base = packet & 0x7FFF;
      const bool one = (packet >> 15) & 1;
      for (uint32_t m = 0; m < count; ++m) WriteRegister(one ? base : base + m, take());
      left -= count;
      continue;
    }
    if (type == 1) {
      if (left < 2) return;
      const uint32_t a = take(), b = take();
      left -= 2;
      WriteRegister(packet & 0x7FF, a);
      WriteRegister((packet >> 11) & 0x7FF, b);
      continue;
    }
    if (type == 2) continue;
    // Type 3.
    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (count > left) return;
    ++reader_now_.packets;
    left -= count;
    // The data words of this packet, read now (the cursor ends after them).
    uint32_t data[8] = {};
    const uint32_t keep = count < 8 ? count : 8;
    const uint32_t data_at = at;
    for (uint32_t i = 0; i < keep; ++i) data[i] = take();
    at = (data_at + count) % size_words;
    if (packet & 1) {
      // Predicated: the emulated processor skips it unless a bin passes, and
      // always skips a predicated swap.
      if ((bin_select_ & bin_mask_) == 0 || opcode == xenos::PM4_XE_SWAP) {
        if (opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2) {
          ++reader_now_.predicated;
        }
        continue;
      }
    }
    switch (opcode) {
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2:
        ReaderDraw();
        break;
      case xenos::PM4_XE_SWAP:
        ReaderSwap();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        const uint32_t ptr = xenos::CpuToGpu(data[0]);
        const uint32_t length = data[1] & 0xFFFFF;
        ++reader_now_.indirect_buffers;
        if (depth < kMaxIndirectDepth && length) {
          DecodeBuffer(memory, memory->TranslatePhysical<const uint32_t*>(ptr), 0, length * 4, 0,
                       length * 4, depth + 1);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        const uint32_t type_index = data[0];
        if (((type_index >> 16) & 0xFF) == 4) {  // registers, base 0x2000
          // Re-read the values: only the first few were kept in data[].
          uint32_t v_at = (data_at + 1) % size_words;
          for (uint32_t i = 0; i + 1 < count; ++i) {
            WriteRegister(0x2000 + (type_index & 0x7FF) + i, rex::byte_swap(words[v_at]));
            v_at = (v_at + 1) % size_words;
          }
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        uint32_t v_at = (data_at + 1) % size_words;
        for (uint32_t i = 0; i + 1 < count; ++i) {
          WriteRegister((data[0] & 0xFFFF) + i, rex::byte_swap(words[v_at]));
          v_at = (v_at + 1) % size_words;
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        const uint32_t type_index = data[1];
        if (((type_index >> 16) & 0xFF) == 4) {
          const auto* src = memory->TranslatePhysical<const uint32_t*>(data[0] & 0x3FFFFFFF);
          const uint32_t n = data[2] & 0xFFF;
          for (uint32_t i = 0; i < n; ++i) {
            WriteRegister(0x2000 + (type_index & 0x7FF) + i, rex::byte_swap(src[i]));
          }
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        const uint32_t info = data[0];
        uint32_t value = registers_[info & 0x1FFF];
        value &= (info >> 31) & 1 ? registers_[data[1] & 0x1FFF] : data[1];
        value |= (info >> 30) & 1 ? registers_[data[2] & 0x1FFF] : data[2];
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data[0];
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data[0];
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK:
        bin_mask_ = (uint64_t(data[0]) << 32) | data[1];
        break;
      case xenos::PM4_SET_BIN_SELECT:
        bin_select_ = (uint64_t(data[0]) << 32) | data[1];
        break;
      default:
        break;
    }
    if (finished_) return;
  }
}

void Pm4Observer::WriteRegister(uint32_t index, uint32_t value) {
  if (index < kRegisterCount) registers_[index] = value;
}

void Pm4Observer::ReaderDraw() {
  const uint32_t mode = registers_[kRbModeControl] & 0x7;
  if (mode == kEdramModeCopy) {
    ++reader_now_.resolves;
    return;
  }
  ++reader_now_.draws;
  // The render target set: surface info, colour 0-3 and depth info, the
  // colour mask (which targets are written) and the EDRAM mode.
  const uint64_t key[7] = {registers_[kRbSurfaceInfo],     registers_[kRbColorInfo],
                           registers_[kRbColorInfo + 1],  registers_[kRbColorInfo + 2],
                           registers_[kRbColorInfo + 3],  registers_[kRbColorInfo + 4],
                           (uint64_t(registers_[kRbColorMask]) << 32) | mode};
  if (!have_rt_key_ || std::memcmp(key, last_rt_key_, sizeof(key)) != 0) {
    if (have_rt_key_) ++reader_now_.rt_changes;
    std::memcpy(last_rt_key_, key, sizeof(key));
    have_rt_key_ = true;
  }
}

void Pm4Observer::ReaderSwap() {
  reader_frames_.push_back(reader_now_);
  reader_now_ = FrameCounts{};
  Flush();
}

void Pm4Observer::EmulatedDraw(bool resolve) {
  if (resolve) {
    ++emulated_now_.resolves;
  } else {
    ++emulated_now_.draws;
  }
}

void Pm4Observer::EmulatedRenderTargetChange() { ++emulated_now_.rt_changes; }

void Pm4Observer::EmulatedSwap() {
  emulated_frames_.push_back(emulated_now_);
  emulated_now_ = FrameCounts{};
  Flush();
}

void Pm4Observer::Flush() {
  while (!finished_ && !reader_frames_.empty() && !emulated_frames_.empty()) {
    const FrameCounts r = reader_frames_.front(), e = emulated_frames_.front();
    reader_frames_.pop_front();
    emulated_frames_.pop_front();
    const bool equal = r.draws == e.draws && r.resolves == e.resolves;
    rows_equal_ += equal;
    reader_total_.draws += r.draws;
    reader_total_.resolves += r.resolves;
    reader_total_.rt_changes += r.rt_changes;
    reader_total_.predicated += r.predicated;
    emulated_total_.draws += e.draws;
    emulated_total_.resolves += e.resolves;
    emulated_total_.rt_changes += e.rt_changes;
    if (csv_) {
      std::fprintf(csv_,
                   "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                   ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d\n",
                   written_, r.draws, e.draws, r.resolves, e.resolves, r.rt_changes,
                   e.rt_changes, r.predicated, r.indirect_buffers, r.packets, int(equal));
    }
    if (++written_ >= frames_wanted_) {
      finished_ = true;
      if (csv_) std::fclose(csv_);
      csv_ = nullptr;
      REXGPU_INFO(
          "[NativeObserve] summary: {} frames; draws reader {} / emulated {}; resolves {} / {}; "
          "render target changes {} / {}; predicated draws (reader) {}; frames with equal draws "
          "and resolves {} of {}; swaps {} / {}",
          written_, reader_total_.draws, emulated_total_.draws, reader_total_.resolves,
          emulated_total_.resolves, reader_total_.rt_changes, emulated_total_.rt_changes,
          reader_total_.predicated, rows_equal_, written_, written_, written_);
    }
  }
}

}  // namespace rex::graphics::native
