// gta4_native deko3d back end, run 63: what writes over the characters after they are drawn
// (docs/switch-port/16-deko3d-devlog.md, section AK).
//
// Run 62's stage counters (section AJ) showed the characters' triangles assembled, clipped and written like the
// scenery's: 1.000 of the expected triangles assembled, the clipper's share equal to the probes' visible faces, 0.988-1.000
// of the fragments written. Their holes are therefore made after the draw, by something that writes the same targets
// later in the frame. Per frame this tracker remembers the surfaces (guest handles, colour and depth) the skinned
// G-buffer draws wrote, and counts each later operation on one of them by kind: a clear, an EDRAM placement
// materialized into it (a copy or the conversion pass over the whole image), a depth hand-off into it, a resolve's depth
// clear, a draw that does not test depth (or tests ALWAYS) and a draw that tests it. It also counts which depth surface
// and first colour target the characters and the scenery draw with: a character on a depth surface of its own is not
// hidden by the scenery's depth, nor does it hide it.
// No Switch headers: tests/deko3d/test_run63.py drives it on the PC.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rex::graphics::gta4_native::dk {

enum class OverwriteKind : uint32_t {
  kClear,
  kPlacement,
  kDepthHandoff,
  kResolveDepthClear,
  kDrawWithoutDepth,
  kDrawWithDepth,
  kCount
};
constexpr uint32_t kOverwriteKinds = uint32_t(OverwriteKind::kCount);

inline const char* OverwriteKindName(OverwriteKind k) {
  switch (k) {
    case OverwriteKind::kClear: return "clears";
    case OverwriteKind::kPlacement: return "EDRAM placements materialized";
    case OverwriteKind::kDepthHandoff: return "depth hand-offs";
    case OverwriteKind::kResolveDepthClear: return "resolve depth clears";
    case OverwriteKind::kDrawWithoutDepth: return "draws not testing depth";
    case OverwriteKind::kDrawWithDepth: return "draws testing depth";
    default: return "?";
  }
}

class OverwriteTracker {
 public:
  struct Event {
    OverwriteKind kind = OverwriteKind::kClear;
    uint32_t handle = 0;
    uint64_t frame = 0, sequence = 0, vertex_shader = 0;
  };
  // (depth surface, colour target 0) and its draws, for one class.
  struct Pair {
    uint32_t depth = 0, colour = 0;
    uint64_t draws = 0;
  };

  void NewFrame(uint64_t frame) {
    frame_ = frame;
    written_.clear();
  }

  // A skinned G-buffer draw: its surfaces join the frame's set.
  void SkinnedDraw(const uint32_t* colours, size_t count, uint32_t depth) {
    for (size_t i = 0; i < count; ++i) {
      if (colours[i]) {
        written_.insert(colours[i]);
      }
    }
    if (depth) {
      written_.insert(depth);
    }
    AddPair(characters_, depth, count ? colours[0] : 0);
  }
  void SceneryDraw(uint32_t depth, uint32_t colour0) { AddPair(scenery_, depth, colour0); }

  bool Touches(uint32_t handle) const { return handle && written_.count(handle) != 0; }

  // A resolve copied the surface out: what the lighting reads is captured, later writes are the next pass's. The
  // surface is followed again from the next skinned draw into it.
  void Resolved(uint32_t handle) {
    if (written_.erase(handle)) {
      ++resolved_;
    }
  }
  uint64_t resolved() const { return resolved_; }

  // An operation on `handle` after the frame's first skinned draw; counted (and the first 32 kept) when the handle is
  // one the characters wrote. True when counted.
  bool Note(OverwriteKind kind, uint32_t handle, uint64_t sequence = 0, uint64_t vertex_shader = 0) {
    if (!Touches(handle)) {
      return false;
    }
    ++interval_[uint32_t(kind)];
    ++totals_[uint32_t(kind)];
    if (events_.size() < 32 && kind != OverwriteKind::kDrawWithDepth) {
      events_.push_back({kind, handle, frame_, sequence, vertex_shader});
    }
    return true;
  }

  const std::array<uint64_t, kOverwriteKinds>& interval() const { return interval_; }
  const std::array<uint64_t, kOverwriteKinds>& totals() const { return totals_; }
  void ResetInterval() { interval_.fill(0); }
  // The events not logged yet (each is logged once).
  std::vector<Event> TakeNewEvents() {
    std::vector<Event> out(events_.begin() + std::ptrdiff_t(logged_), events_.end());
    logged_ = events_.size();
    return out;
  }
  const std::vector<Pair>& characters() const { return characters_; }
  const std::vector<Pair>& scenery() const { return scenery_; }

  // Characters drawn with a depth surface the scenery never uses (the pairs met; at most 16 of each class).
  uint64_t CharacterDrawsOnOwnDepth() const {
    uint64_t n = 0;
    for (const Pair& c : characters_) {
      bool shared = false;
      for (const Pair& s : scenery_) {
        shared = shared || s.depth == c.depth;
      }
      n += shared ? 0 : c.draws;
    }
    return n;
  }

  static std::string PairsText(const std::vector<Pair>& pairs) {
    std::string out;
    char buffer[64];
    for (const Pair& p : pairs) {
      std::snprintf(buffer, sizeof(buffer), "%s%08X/%08X x%llu", out.empty() ? "" : ", ", p.depth, p.colour,
                    static_cast<unsigned long long>(p.draws));
      out += buffer;
    }
    return out.empty() ? std::string("none") : out;
  }

 private:
  static void AddPair(std::vector<Pair>& pairs, uint32_t depth, uint32_t colour) {
    for (Pair& p : pairs) {
      if (p.depth == depth && p.colour == colour) {
        ++p.draws;
        return;
      }
    }
    if (pairs.size() < 16) {
      pairs.push_back({depth, colour, 1});
    }
  }

  uint64_t frame_ = 0, resolved_ = 0;
  std::unordered_set<uint32_t> written_;
  std::array<uint64_t, kOverwriteKinds> interval_{}, totals_{};
  std::vector<Event> events_;
  size_t logged_ = 0;
  std::vector<Pair> characters_, scenery_;
};

}  // namespace rex::graphics::gta4_native::dk
