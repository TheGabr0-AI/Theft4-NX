// gta4_native: the packets' way from the title's threads to the recorder thread, a ring of slots made once
// (docs/switch-port/16-deko3d-devlog.md, run 46). Header only, no Switch headers: tests/deko3d/test_packet_ring.py runs it on
// the PC with two threads.
//
// Run 45 (std::deque, one lock for push and pop): a push cost 150-450 us in play against 40-50 us in run 44, the recorder
// spent 190-760 us per draw outside its measured parts, and both threads were off a CPU for most of that time. The deque
// allocated a node per packet on the title's thread and freed it on the recorder's, up to 64 frees under the queue lock (run
// 45's batch pop). Here the slots exist from the start: the producer moves a packet into a free slot, the consumer reads the
// slots in place and gives them back. Head and tail are atomics; the mutex is taken only to sleep and to wake a sleeper.
//
// One producer at a time (the caller serialises: Deko3dGraphicsSystem holds its front-end lock) and one consumer thread.

#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rex::graphics::gta4_native {

template <class T>
class PacketRing {
 public:
  explicit PacketRing(size_t slots) : slots_(std::bit_ceil(slots < 2 ? size_t(2) : slots)), mask_(slots_.size() - 1) {}

  size_t capacity() const { return slots_.size(); }
  size_t depth() const { return size_t(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire)); }
  bool stopped() const { return stopped_.load(std::memory_order_acquire); }
  uint64_t full_waits() const { return full_waits_.load(std::memory_order_relaxed); }

  // Producer. Moves `item` into the next slot, waiting while every slot is taken; false once stopped (the item stays with
  // the caller). A sleeping consumer is woken when `wake` or when `wake_batch` packets wait; otherwise it finds them at its
  // timeout.
  bool Push(T&& item, bool wake, size_t wake_batch) {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_.load(std::memory_order_acquire) >= slots_.size()) {
      full_waits_.fetch_add(1, std::memory_order_relaxed);
      std::unique_lock<std::mutex> lock(mutex_);
      producer_sleeping_.store(true, std::memory_order_seq_cst);
      while (!stopped_.load(std::memory_order_acquire) && t - head_.load(std::memory_order_seq_cst) >= slots_.size()) {
        space_.wait_for(lock, std::chrono::milliseconds(10));
      }
      producer_sleeping_.store(false, std::memory_order_relaxed);
    }
    if (stopped_.load(std::memory_order_acquire)) {
      return false;
    }
    slots_[t & mask_] = std::move(item);
    tail_.store(t + 1, std::memory_order_seq_cst);
    // Seen sleeping after the tail moved: it is in its wait (it held the mutex from its last look at the tail to the wait),
    // so taking the mutex once orders the notify after that wait began. Not seen: its next look finds the packet.
    if ((wake || t + 1 - head_.load(std::memory_order_relaxed) >= wake_batch) &&
        consumer_sleeping_.load(std::memory_order_seq_cst)) {
      { std::lock_guard<std::mutex> lock(mutex_); }
      ready_.notify_one();
    }
    return true;
  }

  // Consumer. The number of packets ready (at most `max`), waiting up to `timeout` when there are none; 0 at the timeout or
  // once stopped with nothing left.
  size_t Wait(std::chrono::nanoseconds timeout, size_t max) {
    size_t ready = Ready(max);
    if (ready || stopped()) {
      return ready;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    consumer_sleeping_.store(true, std::memory_order_seq_cst);
    if (tail_.load(std::memory_order_seq_cst) == head_.load(std::memory_order_relaxed) && !stopped()) {
      ready_.wait_for(lock, timeout);
    }
    consumer_sleeping_.store(false, std::memory_order_relaxed);
    lock.unlock();
    return Ready(max);
  }

  // The i-th ready packet (i < what Wait returned), read and changed in place.
  T& Peek(size_t i) { return slots_[(head_.load(std::memory_order_relaxed) + i) & mask_]; }

  // Gives the first n ready slots back to the producer.
  void Release(size_t n) {
    head_.store(head_.load(std::memory_order_relaxed) + n, std::memory_order_seq_cst);
    if (producer_sleeping_.load(std::memory_order_seq_cst)) {
      { std::lock_guard<std::mutex> lock(mutex_); }
      space_.notify_one();
    }
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_.store(true, std::memory_order_release);
    }
    ready_.notify_all();
    space_.notify_all();
  }

 private:
  size_t Ready(size_t max) const {
    const uint64_t n = tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_relaxed);
    return size_t(n < max ? n : max);
  }

  std::vector<T> slots_;
  const size_t mask_;
  alignas(64) std::atomic<uint64_t> head_{0};  // the consumer's: the next slot to read
  alignas(64) std::atomic<uint64_t> tail_{0};  // the producer's: the next slot to fill
  std::atomic<bool> consumer_sleeping_{false};
  std::atomic<bool> producer_sleeping_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<uint64_t> full_waits_{0};
  std::mutex mutex_;
  std::condition_variable ready_;  // the consumer sleeps here
  std::condition_variable space_;  // the producer sleeps here when every slot is taken
};

}  // namespace rex::graphics::gta4_native
