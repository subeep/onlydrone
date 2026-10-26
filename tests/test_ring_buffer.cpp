// tests/test_ring_buffer.cpp
// GPL-3.0-or-later.
#include <atomic>
#include <thread>
#include <vector>

#include "droneid/ring_buffer.h"
#include "droneid/test/microtest.h"

using droneid::SpscRingBuffer;

TEST(RingBuffer, RoundsCapacityToPowerOfTwo) {
  SpscRingBuffer<int> rb(100);
  EXPECT_EQ(rb.capacity(), static_cast<std::size_t>(128));
}

TEST(RingBuffer, WriteThenRead) {
  SpscRingBuffer<int> rb(16);
  int src[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  EXPECT_EQ(rb.write(src, 8), static_cast<std::size_t>(8));
  EXPECT_EQ(rb.size(), static_cast<std::size_t>(8));
  int dst[8] = {0};
  EXPECT_EQ(rb.read(dst, 8), static_cast<std::size_t>(8));
  for (int i = 0; i < 8; ++i) EXPECT_EQ(dst[i], i);
  EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, WriteClampsWhenFull) {
  SpscRingBuffer<int> rb(4);  // capacity 4
  int src[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  EXPECT_EQ(rb.write(src, 8), static_cast<std::size_t>(4));  // only 4 fit
}

TEST(RingBuffer, WrapAroundPreservesOrder) {
  SpscRingBuffer<int> rb(8);
  int out[8];
  int counter = 0;
  // Push/drain across the wrap boundary many times.
  for (int round = 0; round < 100; ++round) {
    int src[5];
    for (int i = 0; i < 5; ++i) src[i] = counter++;
    EXPECT_EQ(rb.write(src, 5), static_cast<std::size_t>(5));
    EXPECT_EQ(rb.read(out, 5), static_cast<std::size_t>(5));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(out[i], src[i]);
  }
}

TEST(RingBuffer, SpscThreadedNoLoss) {
  SpscRingBuffer<int> rb(1024);
  const int N = 200000;
  std::vector<int> got;
  got.reserve(N);
  std::atomic<bool> done{false};

  std::thread consumer([&] {
    int buf[256];
    while (!done.load(std::memory_order_acquire) || !rb.empty()) {
      std::size_t r = rb.read(buf, 256);
      for (std::size_t i = 0; i < r; ++i) got.push_back(buf[i]);
    }
  });

  for (int i = 0; i < N;) {
    int buf[64];
    int m = 0;
    while (m < 64 && i < N) buf[m++] = i++;
    int off = 0;
    while (off < m) off += static_cast<int>(rb.write(buf + off, m - off));
  }
  done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_EQ(got.size(), static_cast<std::size_t>(N));
  bool ordered = true;
  for (int i = 0; i < N; ++i)
    if (got[static_cast<std::size_t>(i)] != i) { ordered = false; break; }
  EXPECT_TRUE(ordered);
}

MICROTEST_MAIN
