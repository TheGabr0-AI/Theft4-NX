// gta4_native: the front end of a recorder (native_recorder.h), backend-neutral (docs/switch-port/16-deko3d-devlog.md,
// M3 B).
//
// It reads Liberty Recompiled's title commands the way Gta4NativeGraphicsSystem's producer does
// (ValidateAndCopyCommand, CaptureSurfaceDescriptor, the guest device layout in core/draw_state.h): at each draw
// and clear the guest device's packed state is decoded (core::DecodeFixedState), the float constants are compared
// with what the previous draw of the device had and only the registers that changed travel, the fetch constants
// and texture objects of the slots the shaders sample are read from the device, the vertex streams and the index
// buffer from their guest objects, the render targets and the depth target from the device's surface pointers.
// Nothing here creates or records GPU work.
//
// Thread: Submit is called by the title's threads under one lock (the caller's); the packets go to the recorder's
// thread in order.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "native_recorder.h"

namespace rex::memory {
class Memory;
}

namespace rex::graphics::gta4_native {

using FrontEndPacket = std::variant<RecorderDraw, RecorderClear, RecorderResolve, RecorderPresent, RecorderResourceEvent,
                                    RecorderDepthHandoff, RecorderTextureLock>;

class NativeFrontEnd {
 public:
  // The texture slots a shader samples (its cache entry's usedTextureMask); UINT32_MAX when unknown (every slot is
  // captured).
  using UsedTextureMask = std::function<uint32_t(uint64_t hash)>;

  NativeFrontEnd(memory::Memory* memory, UsedTextureMask used_texture_mask);

  // One title command, after its envelope (GpuPassEnvelope) is removed. False: rejected (logged a few times).
  bool Submit(const void* command, size_t size, std::vector<FrontEndPacket>& out);

  struct Stats {
    uint64_t commands = 0, draws = 0, clears = 0, resolves = 0, presents = 0, rejected = 0;
    uint64_t constant_registers = 0;  // 16-byte registers that travelled
    uint64_t up_bytes = 0;            // DrawPrimitiveUp vertices copied
    // Title commands this front end drops (they need a recorder step that does not exist yet): virtual resources
    // (the packed-depth alias texture), and the rest. The depth hand-off from the deferred to the forward phase was
    // dropped until run 43 (`ignored_depth_handoff`: now only the malformed ones); `depth_handoffs` are passed on.
    uint64_t ignored_depth_handoff = 0, ignored_virtual_resource = 0, ignored_other = 0;
    uint64_t depth_handoffs = 0;
    uint64_t packed_depth_aliases = 0;  // virtual resource registrations that carry a packed depth source (passed on)
  };
  const Stats& stats() const { return stats_; }

  // Run 49 (native_draw_snapshot.h): the recorder found this buffer's bytes changed between submit and record; its draws
  // are copied at submit from the next draw on. Any thread.
  void PromoteDynamicBuffer(uint32_t handle);
  // Run 58 (the A/B's "copies off" phase): false = no draw is copied at submit (the sampled hashes and the promotions go
  // on). Any thread.
  void SetSubmitCopies(bool on) { submit_copies_.store(on, std::memory_order_relaxed); }
  // Run 59 (native_draw_snapshot.h VolatilePages): copies for the draws whose range touches a page the title rewrote
  // under a waiting draw, and copies for the buffer objects the recorder promoted; the A/B switches each. Any thread.
  void SetVolatilePages(bool on) { volatile_pages_.store(on, std::memory_order_relaxed); }
  void SetHandlePromotions(bool on) { handle_promotions_.store(on, std::memory_order_relaxed); }

  // Why a draw's data was copied at submit (RecorderDraw::copy_reason; 0: not copied).
  enum CopyReason : uint8_t { kCopyNone = 0, kCopyLocked, kCopyRewritten, kCopyPromoted, kCopyPages };

  struct DataStats {
    uint64_t copied_draws = 0, copied_bytes = 0;  // draws whose vertex/index range was copied at submit
    uint64_t copied_locked = 0;                   // of which because a buffer was still locked
    uint64_t hashed_draws = 0;                    // sampled draws hashed at submit for the recorder to compare
    uint64_t dynamic_buffers = 0;                 // buffers copied at submit (second write unlock or promoted)
    uint64_t promoted_buffers = 0;                // of which promoted by the recorder
    // Run 59: copied draws by reason (the first that applies: locked, rewritten after its first fill, promoted, pages),
    // and the draws a marked page made the front end look at (their range then decided).
    uint64_t copied_rewritten = 0, copied_promoted = 0, copied_pages = 0;
    uint64_t page_suspects = 0;
  };
  DataStats data_stats() const {
    DataStats s = data_stats_;
    s.dynamic_buffers = dynamic_buffers_.size() + promoted_buffers_.size();
    return s;
  }

 private:
  struct DeviceState {
    uint32_t vertex_shader = 0, pixel_shader = 0, declaration = 0, index_buffer = 0;
    std::array<RecorderVertexStream, kVertexStreamCount> streams{};
    bool constants_valid = false;
    // The constant banks as the guest wrote them (big-endian words, raw): compared 64 bytes at a time (native_constant_diff.h)
    std::array<uint32_t, kVertexConstantRegisters * 4> vertex_constants{};
    std::array<uint32_t, kPixelConstantRegisters * 4> pixel_constants{};
  };

  bool Reject(uint32_t type, const char* reason);
  SurfaceDescriptor CaptureSurface(uint32_t handle) const;
  // The data of a guest vertex or index buffer object (DecodeNativeBufferMetadata); false if it has none.
  bool BufferData(uint32_t handle, uint32_t& flags, uint32_t& address, uint32_t& size) const;
  void CaptureConstants(DeviceState& state, const uint8_t* device, ConstantDelta& vertex, ConstantDelta& pixel);
  bool CaptureDraw(uint32_t device, RecorderDraw& draw);
  // Copies or hashes the draw's vertex and index range (run 49).
  void CaptureDrawData(RecorderDraw& draw, const std::array<bool, kVertexStreamCount + 1>& locked);
  void TakePromotions();

  memory::Memory* memory_;
  UsedTextureMask used_texture_mask_;
  std::unordered_map<uint32_t, uint64_t> shader_hashes_;  // shader object -> hash
  std::unordered_map<uint32_t, std::shared_ptr<const RecorderVertexDeclaration>> declarations_;
  std::unordered_map<uint32_t, DeviceState> devices_;
  uint64_t declaration_generation_ = 0;
  RenderPhase phase_ = RenderPhase::kUnknown;
  Stats stats_;
  // Run 49: guest-write unlocks per buffer object; a buffer with two or more (rewritten after its first fill) is dynamic.
  std::unordered_map<uint32_t, uint32_t> unlocks_;
  std::unordered_set<uint32_t> dynamic_buffers_;
  std::unordered_set<uint32_t> promoted_buffers_;  // run 59: the recorder's promotions, apart (the A/B switches them)
  DataStats data_stats_;
  std::mutex promote_mutex_;
  std::vector<uint32_t> promoted_;
  std::atomic<bool> promote_pending_{false};
  std::atomic<bool> submit_copies_{true};
  std::atomic<bool> volatile_pages_{true};
  std::atomic<bool> handle_promotions_{true};
};

}  // namespace rex::graphics::gta4_native
