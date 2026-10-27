#include "droneid/hop.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace droneid {

HopSchedule::HopSchedule(std::vector<double> channels, double dwell, double settle)
    : channels_(std::move(channels)), dwell_(dwell), settle_(settle) {
  if (channels_.empty() || !std::isfinite(dwell) || dwell <= 0 || dwell > 3600 ||
      !std::isfinite(settle) || settle < 0 || settle > 60)
    throw std::invalid_argument("invalid hop schedule");
  for (double center : channels_)
    if (!std::isfinite(center) || center < 1e6 || center > 6e9)
      throw std::invalid_argument("hop center must be between 1 MHz and 6 GHz");
}

HopVisit HopSchedule::visit(std::size_t index, double epoch) const {
  const double tune = epoch + index * (dwell_ + settle_);
  if (!std::isfinite(tune)) throw std::invalid_argument("invalid hop epoch");
  return {channels_[index % channels_.size()], tune, tune + settle_, tune + settle_ + dwell_};
}

double HopSchedule::cycle_seconds() const { return channels_.size() * (dwell_ + settle_); }

double HopSchedule::dwell_for_probability(double period, double burst, double probability) {
  if (!std::isfinite(period) || period <= 0 || !std::isfinite(burst) || burst <= 0 ||
      burst > period || !std::isfinite(probability) || probability <= 0 || probability > 1)
    throw std::invalid_argument("invalid burst period, duration or probability");
  const double dwell = burst + period * probability;
  if (!std::isfinite(dwell)) throw std::invalid_argument("dwell calculation overflow");
  return dwell;
}

double HopSchedule::catch_probability(double period, double burst) const {
  dwell_for_probability(period, burst, 1);
  return std::clamp((dwell_ - burst) / period, 0.0, 1.0);
}

std::vector<double> HopSchedule::channels_for_band(int band) {
  if (band == 24) return {2399.5e6, 2414.5e6, 2429.5e6, 2444.5e6, 2459.5e6};
  if (band == 58) return {5756.5e6, 5776.5e6, 5796.5e6};
  throw std::invalid_argument("band must be 24 or 58");
}

}
