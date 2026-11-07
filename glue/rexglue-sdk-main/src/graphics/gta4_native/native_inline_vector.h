// gta4_native: a vector that keeps its first N elements inside the object (docs/switch-port/16-deko3d-devlog.md, run 46).
// Header only, no Switch headers: tests/deko3d/test_packet_ring.py compiles it on the PC.
//
// Run 45 measured the title's thread and the recorder blocked (off a CPU) for most of their time while the process used about
// 130 % of its three cores; every packet cost a deque node and up to four constant vectors (malloc on the title's thread, free
// on the recorder's), all under newlib's one malloc lock. A draw changes about three registers on average (run 45: 1,727,046
// registers over 543,817 draws), so a few inline registers carry almost every delta without the heap; the first draw of a
// device (the whole bank) and the skinned draws spill into an ordinary vector.

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::graphics::gta4_native {

template <class T, size_t N>
class InlineVector {
 public:
  InlineVector() = default;
  InlineVector(const InlineVector& other) { *this = other; }
  InlineVector(InlineVector&& other) noexcept { *this = std::move(other); }

  InlineVector& operator=(const InlineVector& other) {
    if (this == &other) {
      return *this;
    }
    if (other.spilled_) {
      heap_ = other.heap_;
      spilled_ = true;
      size_ = 0;
    } else {
      Unspill();
      for (uint32_t i = 0; i < other.size_; ++i) {
        inline_[i] = other.inline_[i];
      }
      size_ = other.size_;
    }
    return *this;
  }

  // Only the elements in use are copied; a spilled source hands its storage over.
  InlineVector& operator=(InlineVector&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (other.spilled_) {
      heap_ = std::move(other.heap_);
      spilled_ = true;
      size_ = 0;
    } else {
      Unspill();
      for (uint32_t i = 0; i < other.size_; ++i) {
        inline_[i] = other.inline_[i];
      }
      size_ = other.size_;
    }
    other.heap_.clear();
    other.spilled_ = false;
    other.size_ = 0;
    return *this;
  }

  size_t size() const { return spilled_ ? heap_.size() : size_; }
  bool empty() const { return size() == 0; }
  size_t capacity() const { return spilled_ ? heap_.capacity() : N; }
  bool spilled() const { return spilled_; }
  T* data() { return spilled_ ? heap_.data() : inline_.data(); }
  const T* data() const { return spilled_ ? heap_.data() : inline_.data(); }
  T* begin() { return data(); }
  T* end() { return data() + size(); }
  const T* begin() const { return data(); }
  const T* end() const { return data() + size(); }
  T& operator[](size_t i) { return data()[i]; }
  const T& operator[](size_t i) const { return data()[i]; }
  T& back() { return data()[size() - 1]; }
  const T& back() const { return data()[size() - 1]; }

  void push_back(const T& value) {
    if (!spilled_ && size_ == N) {
      Spill(2 * N);
    }
    if (spilled_) {
      heap_.push_back(value);
    } else {
      inline_[size_++] = value;
    }
  }

  // At the end only (what the constant diff's reference loop does).
  void insert(const T* position, const T* first, const T* last) {
    assert(position == end());
    (void)position;
    for (; first != last; ++first) {
      push_back(*first);
    }
  }

  void reserve(size_t n) {
    if (n > capacity()) {
      if (spilled_) {
        heap_.reserve(n);
      } else {
        Spill(n);
      }
    }
  }

  void clear() {
    heap_.clear();
    spilled_ = false;
    size_ = 0;
  }

 private:
  void Spill(size_t reserve) {
    heap_.clear();
    heap_.reserve(reserve);
    heap_.assign(inline_.begin(), inline_.begin() + size_);
    spilled_ = true;
    size_ = 0;
  }
  // Back to the inline storage; a heap block left from a spill is given back (a ring slot must not keep one for ever).
  void Unspill() {
    if (spilled_ || heap_.capacity()) {
      std::vector<T>().swap(heap_);
    }
    spilled_ = false;
  }

  std::array<T, N> inline_;  // not initialised: only [0, size_) is ever read
  std::vector<T> heap_;
  uint32_t size_ = 0;
  bool spilled_ = false;
};

}  // namespace rex::graphics::gta4_native
