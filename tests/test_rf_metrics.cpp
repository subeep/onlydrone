#include "droneid/rf_metrics.h"
#include "droneid/test/microtest.h"

#include <algorithm>
#include <cmath>

TEST(Rf, ToneMetrics) {
  std::vector<droneid::cf32> samples(4096);
  for (std::size_t index = 0; index < samples.size(); ++index)
    samples[index] = std::polar(0.1f, static_cast<float>(2 * std::acos(-1.0) * 0.125 * index));
  const auto metrics = droneid::measure_rf(samples, 1e6, 0.0001);
  EXPECT_NEAR(metrics.rssi_dbfs, -20, 1e-4);
  EXPECT_NEAR(metrics.snr_db, 10 * std::log10(99), 1e-4);
  EXPECT_NEAR(metrics.spectral_center_hz, 125000, 10);
  EXPECT_TRUE(metrics.bandwidth_hz < 5000);
  EXPECT_EQ(metrics.clipped_fraction, 0.0);
}

TEST(Rf, ChunkIndependentAndBounded) {
  std::vector<droneid::cf32> samples(4096);
  std::fill(samples.begin() + 320, samples.begin() + 960, droneid::cf32(0.1, 0));
  std::fill(samples.begin() + 1920, samples.begin() + 2560, droneid::cf32(0.1, 0));
  droneid::EnergyBurstDetector whole(1e6, -60);
  const auto expected = whole.feed(samples.data(), samples.size(), true);
  EXPECT_EQ(expected.size(), std::size_t(2));
  for (std::size_t chunk : {std::size_t(1), std::size_t(33), std::size_t(257)}) {
    droneid::EnergyBurstDetector detector(1e6, -60);
    std::vector<droneid::RfBurst> actual;
    for (std::size_t start = 0; start < samples.size(); start += chunk) {
      const auto size = std::min(chunk, samples.size() - start);
      const auto events = detector.feed(samples.data() + start, size, start + size == samples.size());
      actual.insert(actual.end(), events.begin(), events.end());
    }
    EXPECT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < std::min(actual.size(), expected.size()); ++index) {
      EXPECT_EQ(actual[index].start_sample, expected[index].start_sample);
      EXPECT_TRUE(actual[index].samples == expected[index].samples);
      EXPECT_FALSE(actual[index].truncated);
    }
  }
  droneid::EnergyBurstDetector bounded(1e6, -60, 10, 50e-6, 0.001);
  std::fill(samples.begin(), samples.end(), droneid::cf32(0.1, 0));
  const auto events = bounded.feed(samples.data(), samples.size(), true);
  EXPECT_TRUE(events.size() >= 4);
  for (const auto& event : events) {
    EXPECT_TRUE(event.truncated);
    EXPECT_TRUE(event.samples.size() <= 1031);
  }
}

TEST(Rf, SilenceAndInvalidConfiguration) {
  droneid::EnergyBurstDetector detector(1e6, -60);
  const std::vector<droneid::cf32> silence(10000);
  EXPECT_TRUE(detector.feed(silence.data(), silence.size(), true).empty());
  bool rejected = false;
  try { droneid::EnergyBurstDetector invalid(0, -60); }
  catch (const std::exception&) { rejected = true; }
  EXPECT_TRUE(rejected);
  rejected = false;
  try { detector.feed(nullptr, 0); }
  catch (const std::exception&) { rejected = true; }
  EXPECT_TRUE(rejected);
}

MICROTEST_MAIN
