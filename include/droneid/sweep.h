#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace droneid {
enum class SweepMode { DroneId, O4, WifiRid };
const char* to_string(SweepMode mode);
struct SweepStep {
  double center_hz = 0;
  double dwell_seconds = 0;
  SweepMode mode = SweepMode::DroneId;
};
// Engineering starting profiles, not measured detection guarantees.
class SweepPlan {
 public:
  SweepPlan(const std::string& profile = "balanced", bool include_52 = false,
            bool all_wifi = false, double settle_seconds = .05, double jitter_seconds = .04);
  SweepStep step(std::uint64_t index) const;
  std::size_t size() const { return steps_.size(); }
  double settle_seconds() const { return settle_; }
  double minimum_cycle_seconds() const;
  double maximum_cycle_seconds_before_processing() const;
  static constexpr double sample_rate = 20e6;
  static constexpr double master_clock_rate = 200e6;
  static constexpr double command_lead_seconds = .1;
 private:
  std::vector<SweepStep> steps_;
  double settle_, jitter_;
};

// Reject whole dwells if timestamps or overflows make their continuity uncertain.
class DwellContinuity {
 public:
  DwellContinuity(double rate, double expected_start, std::uint64_t overflow_count);
  void add(std::size_t samples, std::int64_t timestamp_ns, std::uint64_t overflow_count);
  bool valid(std::size_t expected_samples) const;
  std::uint64_t received() const { return received_; }
 private:
  double rate_, start_;
  std::uint64_t received_ = 0, initial_overflows_;
  std::int64_t first_ = 0;
  bool continuous_ = true;
};
}
