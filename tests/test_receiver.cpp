#include "droneid/aoa.h"
#include "droneid/hop.h"
#include "droneid/test/microtest.h"

#include <cmath>
#include <limits>
#include <stdexcept>

template <typename Action> bool rejects(Action action) {
  try { action(); } catch (const std::exception&) { return true; }
  return false;
}

TEST(Hop, TimingAndCoverageModel) {
  const auto channels = droneid::HopSchedule::channels_for_band(58);
  EXPECT_EQ(channels.size(), std::size_t(3));
  EXPECT_TRUE(channels[0] > 4294967295.0);
  const double dwell = droneid::HopSchedule::dwell_for_probability(0.64, 0.00065, 0.95);
  droneid::HopSchedule scheduler(channels, dwell, 0.05);
  EXPECT_NEAR(scheduler.catch_probability(0.64, 0.00065), 0.95, 1e-10);
  const auto visit = scheduler.visit(4, 12);
  EXPECT_EQ(visit.center_hz, channels[1]);
  EXPECT_NEAR(visit.tune_seconds, 12 + 4 * (dwell + 0.05), 1e-10);
  EXPECT_NEAR(visit.listen_seconds - visit.tune_seconds, 0.05, 1e-10);
  EXPECT_NEAR(visit.end_seconds - visit.listen_seconds, dwell, 1e-10);
  EXPECT_NEAR(scheduler.cycle_seconds(), 3 * (dwell + 0.05), 1e-10);
  EXPECT_EQ(droneid::HopSchedule(channels, 0.0001, 0).catch_probability(0.64, 0.00065), 0.0);
  EXPECT_EQ(droneid::HopSchedule(channels, 1, 0).catch_probability(0.64, 0.00065), 1.0);
}

TEST(Hop, RejectInvalidInputs) {
  EXPECT_TRUE(rejects([] { droneid::HopSchedule({}, 1, 0); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule({2.4e9}, -1, 0); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule({7e9}, 1, 0); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::channels_for_band(5); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::dwell_for_probability(0, 1, 0.5); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::dwell_for_probability(1, 0.1, 1.1); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::dwell_for_probability(1, 2, 0.5); }));
}

TEST(Direction, CalibratedAnglesAndRejections) {
  droneid::PhaseCal calibration;
  calibration.phase_rad = 2.7;
  calibration.center_hz = 2459.5e6;
  calibration.sample_rate = 15.36e6;
  calibration.spacing_m = 0.05;
  calibration.measured_unix_ns = 1000000000;
  calibration.max_age_seconds = 60;
  const double pi = std::acos(-1.0);
  std::vector<droneid::cf32> first(4096), second(4096);
  for (double angle : {-55.0, -30.0, 0.0, 30.0, 55.0}) {
    const double phase = calibration.phase_rad + 2 * pi * calibration.spacing_m *
        calibration.center_hz / 299792458.0 * std::sin(angle * pi / 180);
    for (std::size_t index = 0; index < first.size(); ++index) {
      first[index] = std::polar(0.4f, static_cast<float>(index * 0.013));
      second[index] = first[index] * std::polar(0.6f, static_cast<float>(phase));
    }
    const auto result = droneid::estimate_bearing(first, second, 123, 123, calibration.center_hz,
        calibration.sample_rate, calibration, 2000000000);
    EXPECT_TRUE(result.valid);
    EXPECT_NEAR(result.broadside_deg, angle, 1e-4);
    EXPECT_NEAR(result.coherence, 1, 1e-6);
    EXPECT_TRUE(result.ambiguous_front_back);
  }
  auto estimate = [&](int64_t timestamp, double center, const droneid::PhaseCal& supplied, int64_t now) {
    return droneid::estimate_bearing(first, second, 123, timestamp, center,
                                    calibration.sample_rate, supplied, now);
  };
  EXPECT_FALSE(estimate(124, calibration.center_hz, calibration, 2000000000).valid);
  EXPECT_FALSE(estimate(123, calibration.center_hz + 10, calibration, 2000000000).valid);
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 62000000000).valid);
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 0).valid);
  auto invalid = calibration;
  invalid.spacing_m = 1;
  EXPECT_FALSE(estimate(123, calibration.center_hz, invalid, 2000000000).valid);
  invalid = calibration;
  invalid.phase_rad = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(estimate(123, calibration.center_hz, invalid, 2000000000).valid);
  for (std::size_t index = 0; index < second.size(); ++index)
    second[index] = std::polar(0.5f, static_cast<float>(index * 0.2));
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 2000000000).valid);
  second.assign(first.size(), {});
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 2000000000).valid);
  second.pop_back();
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 2000000000).valid);
}

MICROTEST_MAIN
