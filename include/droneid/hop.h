#pragma once

#include <cstddef>
#include <vector>

namespace droneid {

struct HopVisit {
  double center_hz;
  double tune_seconds;
  double listen_seconds;
  double end_seconds;
};

class HopSchedule {
 public:
  HopSchedule(std::vector<double> channels, double dwell_seconds, double settle_seconds);
  HopVisit visit(std::size_t index, double epoch_seconds = 0) const;
  double cycle_seconds() const;
  double catch_probability(double period_seconds, double burst_seconds) const;
  static double dwell_for_probability(double period_seconds, double burst_seconds, double probability);
  static std::vector<double> channels_for_band(int band);

 private:
  std::vector<double> channels_;
  double dwell_, settle_;
};

}
