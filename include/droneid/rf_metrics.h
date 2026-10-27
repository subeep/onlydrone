#pragma once

#include <cstdint>
#include <vector>

#include "droneid/types.h"

namespace droneid {

struct RfMetrics {
  double rssi_dbfs = -300;
  double snr_db = -300;
  double bandwidth_hz = 0;
  double spectral_center_hz = 0;
  double clipped_fraction = 0;
};

RfMetrics measure_rf(const std::vector<cf32>& samples, double sample_rate, double noise_power);

struct RfBurst {
  uint64_t start_sample = 0;
  std::vector<cf32> samples;
  bool truncated = false;
};

class EnergyBurstDetector {
 public:
  EnergyBurstDetector(double sample_rate, double noise_dbfs, double threshold_db = 10,
                      double min_seconds = 50e-6, double max_seconds = 0.01);
  std::vector<RfBurst> feed(const cf32* samples, std::size_t count, bool final = false);
  double noise_power() const { return noise_power_; }

 private:
  void consume(std::vector<RfBurst>& output);
  void finish(std::vector<RfBurst>& output, bool truncated);
  double noise_power_, threshold_;
  std::size_t min_samples_, max_samples_, quiet_limit_, quiet_samples_ = 0;
  uint64_t received_ = 0;
  std::vector<cf32> block_;
  RfBurst current_;
  bool active_ = false, continuation_ = false, finished_ = false;
};

}
