// Native renderer, step N1: a PM4 ring reader in "observe" mode.
//
// It decodes the ring on its own (packet types 0-3, its own copy of the
// registers, indirect buffers followed by reading guest memory itself), before
// the emulated command processor executes the same words, and counts per
// frame: draws, resolves (draws in EDRAM copy mode), swaps and render target
// changes. The emulated renderer reports the same four events as it executes
// them; both series are written side by side, one CSV row per frame, for the
// first native_observe_frames frames, then one summary line. Nothing is
// drawn and nothing is changed: it only reads.

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace rex::graphics::native {

// Per-frame counts of one source (the reader or the emulated renderer).
struct FrameCounts {
  uint64_t draws = 0;            // draws that are not EDRAM copies
  uint64_t resolves = 0;         // draws in EDRAM copy mode (RB_MODECONTROL = kCopy)
  uint64_t rt_changes = 0;       // render target set different from the previous draw's
  uint64_t predicated = 0;       // reader only: draws skipped by the bin mask/select
  uint64_t indirect_buffers = 0; // reader only: indirect buffers followed
  uint64_t packets = 0;          // reader only: type-3 packets decoded
};

class Pm4Observer {
 public:
  // Null unless cvar native_observe_frames > 0; created on first use.
  static Pm4Observer* Get();

  // The reader: the primary ring words [read_index, write_index) of the ring
  // at guest physical primary_ptr (size in bytes), decoded before execution.
  void ObservePrimary(memory::Memory* memory, uint32_t primary_ptr, uint32_t primary_size,
                      uint32_t read_index, uint32_t write_index);

  // The emulated renderer (same thread, in execution order).
  void EmulatedDraw(bool resolve);
  void EmulatedRenderTargetChange();
  void EmulatedSwap();

  bool finished() const { return finished_; }

 private:
  Pm4Observer(uint32_t frames, std::string csv_path);
  void DecodeBuffer(memory::Memory* memory, const uint32_t* words, uint32_t base_bytes_mask,
                    uint32_t size_bytes, uint32_t read_offset, uint32_t write_offset, int depth);
  void WriteRegister(uint32_t index, uint32_t value);
  void ReaderDraw();
  void ReaderSwap();
  void Flush();

  uint32_t frames_wanted_;
  std::string csv_path_;
  std::FILE* csv_ = nullptr;
  bool finished_ = false;
  std::vector<uint32_t> registers_;
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;
  uint64_t last_rt_key_[7] = {};
  bool have_rt_key_ = false;
  FrameCounts reader_now_, emulated_now_;
  std::deque<FrameCounts> reader_frames_, emulated_frames_;
  uint64_t written_ = 0;
  FrameCounts reader_total_, emulated_total_;
  uint64_t rows_equal_ = 0;
};

}  // namespace rex::graphics::native
