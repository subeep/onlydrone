#include "droneid/sweep.h"
#include "droneid/hop.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace droneid {
const char* to_string(SweepMode mode) {
  switch (mode) {
    case SweepMode::DroneId: return "droneid";
    case SweepMode::O4: return "o4_candidate";
    case SweepMode::WifiRid: return "wifi_remote_id";
  }
  return "unknown";
}
SweepPlan::SweepPlan(const std::string& profile, bool include_52, bool all_wifi,
                     double settle, double jitter) : settle_(settle), jitter_(jitter) {
  if (!std::isfinite(settle) || settle < .02 || settle > .5 ||
      !std::isfinite(jitter) || jitter < 0 || jitter > .1)
    throw std::invalid_argument("sweep settle must be 20-500 ms; jitter 0-100 ms");
  double dji = .8, o4 = .64, wifi = 1.;
  if (profile == "fast") { dji = .64; o4 = .32; wifi = .5; }
  else if (profile == "thorough") { dji = 1.28; o4 = 1.28; all_wifi = true; }
  else if (profile != "balanced") throw std::invalid_argument("sweep profile must be fast, balanced or thorough");
  std::vector<SweepStep> priority, structure, rid;
  for (int band : {24,58}) for (double f : HopSchedule::channels_for_band(band))
    priority.push_back({f,dji,SweepMode::DroneId});
  for (double mhz = 2405; mhz <= 2475; mhz += 10) structure.push_back({mhz*1e6,o4,SweepMode::O4});
  structure.push_back({2478.5e6,o4,SweepMode::O4});
  for (double mhz = 5730; mhz <= 5840; mhz += 10) structure.push_back({mhz*1e6,o4,SweepMode::O4});
  structure.push_back({5845e6,o4,SweepMode::O4});
  if (include_52) for (double mhz = 5175; mhz <= 5245; mhz += 10)
    structure.push_back({mhz*1e6,o4,SweepMode::O4});
  if (all_wifi) for (int ch = 1; ch <= 13; ++ch) rid.push_back({(2407.+5*ch)*1e6,wifi,SweepMode::WifiRid});
  else for (int ch : {1,6,11}) rid.push_back({(2407.+5*ch)*1e6,wifi,SweepMode::WifiRid});
  // Interleave protocols to avoid an entire band/mode waiting behind the others.
  for (std::size_t i = 0; i < std::max({priority.size(),structure.size(),rid.size()}); ++i) {
    if (i < priority.size()) steps_.push_back(priority[i]);
    if (i < structure.size()) steps_.push_back(structure[i]);
    if (i < rid.size()) steps_.push_back(rid[i]);
  }
}
SweepStep SweepPlan::step(std::uint64_t index) const {
  const auto cycle = index / steps_.size();
  auto result = steps_[(index % steps_.size() + (cycle % steps_.size()) * 7) % steps_.size()];
  // Reproducible bounded jitter avoids an exactly periodic revisit schedule.
  const auto phase = ((index % 1009) * 137 + 53) % 1009;
  result.dwell_seconds += jitter_ * static_cast<double>(phase) / 1008.;
  return result;
}
double SweepPlan::minimum_cycle_seconds() const {
  double total = 0;
  for (const auto& s : steps_) total += s.dwell_seconds + settle_ + command_lead_seconds;
  return total;
}
double SweepPlan::maximum_cycle_seconds_before_processing() const {
  return minimum_cycle_seconds() + jitter_ * steps_.size();
}
DwellContinuity::DwellContinuity(double rate, double start, std::uint64_t overflows)
    : rate_(rate), start_(start), initial_overflows_(overflows) {
  if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(start) || start < 0)
    throw std::invalid_argument("invalid capture clock");
}
void DwellContinuity::add(std::size_t samples, std::int64_t timestamp, std::uint64_t overflows) {
  if (!samples) return;
  if (!received_) {
    first_ = timestamp;
    if (std::abs(timestamp / 1e9 - start_) > 1.5/rate_) continuous_ = false;
  } else if (std::abs((timestamp - first_) / 1e9 - received_/rate_) > 1.5/rate_) continuous_ = false;
  if (timestamp <= 0 || overflows != initial_overflows_) continuous_ = false;
  received_ += samples;
}
bool DwellContinuity::valid(std::size_t expected) const { return continuous_ && received_ == expected && expected > 0; }
}
