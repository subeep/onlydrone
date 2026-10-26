// droneid/ring_buffer.h — single-producer / single-consumer lock-free ring buffer.
// The RX thread writes; a DSP thread reads. Never blocks either side.
// GPL-3.0-or-later.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace droneid {

// Capacity is rounded up to a power of two. Safe for exactly one producer thread
// and one consumer thread. Monotonic head/tail counters with a bitmask index.
template <typename T>
class SpscRingBuffer {
 public:
  explicit SpscRingBuffer(std::size_t min_capacity) {
    std::size_t cap = 1;
    while (cap < min_capacity) cap <<= 1;
    buf_.resize(cap);
    mask_ = cap - 1;
  }

  std::size_t capacity() const { return buf_.size(); }

  // Producer side. Returns the number of elements actually written (may be < n if full).
  std::size_t write(const T* data, std::size_t n) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t free_space = buf_.size() - (head - tail);
    const std::size_t m = n < free_space ? n : free_space;
    for (std::size_t i = 0; i < m; ++i) buf_[(head + i) & mask_] = data[i];
    head_.store(head + m, std::memory_order_release);
    return m;
  }

  // Consumer side. Returns the number of elements actually read (may be < n if empty).
  std::size_t read(T* out, std::size_t n) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t avail = head - tail;
    const std::size_t m = n < avail ? n : avail;
    for (std::size_t i = 0; i < m; ++i) out[i] = buf_[(tail + i) & mask_];
    tail_.store(tail + m, std::memory_order_release);
    return m;
  }

  // Approximate occupancy (exact when called from producer or consumer alone).
  std::size_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  bool empty() const { return size() == 0; }

 private:
  std::vector<T> buf_;
  std::size_t mask_ = 0;
  std::atomic<std::size_t> head_{0};  // written by producer
  std::atomic<std::size_t> tail_{0};  // written by consumer
};

}  // namespace droneid
