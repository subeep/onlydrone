#include "droneid/rf_metrics.h"
#include "../dji/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace droneid {

RfMetrics measure_rf(const std::vector<cf32>& samples, double sample_rate, double noise_power) {
  if (samples.empty() || !std::isfinite(sample_rate) || sample_rate <= 0 ||
      !std::isfinite(noise_power) || noise_power <= 0)
    throw std::invalid_argument("RF metrics require samples, a positive rate and noise power");
  RfMetrics metrics;
  double power = 0;
  for (const auto& sample : samples) {
    if (!std::isfinite(std::norm(sample))) throw std::invalid_argument("non-finite IQ sample");
    power += std::norm(sample);
    if (std::abs(sample.real()) >= 0.999 || std::abs(sample.imag()) >= 0.999)
      metrics.clipped_fraction += 1;
  }
  power /= samples.size();
  metrics.rssi_dbfs = 10 * std::log10(std::max(power, 1e-30));
  metrics.snr_db = 10 * std::log10(std::max((power - noise_power) / noise_power, 1e-30));
  metrics.clipped_fraction /= samples.size();
  const std::size_t size = std::min<std::size_t>(1024, samples.size());
  if (size < 32) return metrics;
  std::vector<double> spectrum(size, 0);
  for (std::size_t start = 0; start + size <= samples.size(); start += size) {
    std::vector<cf32> window(size);
    for (std::size_t index = 0; index < size; ++index)
      window[index] = samples[start + index] * static_cast<float>(0.5 - 0.5 *
          std::cos(2 * std::acos(-1.0) * index / (size - 1)));
    const auto transformed = dji::dsp::fftshift(dji::dsp::fft(window));
    for (std::size_t index = 0; index < size; ++index) spectrum[index] += std::norm(transformed[index]);
  }
  double total = 0, moment = 0;
  for (std::size_t index = 0; index < size; ++index) {
    total += spectrum[index];
    moment += spectrum[index] * (static_cast<double>(index) - size / 2) * sample_rate / size;
  }
  if (total <= 0) return metrics;
  metrics.spectral_center_hz = moment / total;
  double cumulative = 0;
  std::size_t lower = 0, upper = size - 1;
  bool lower_found = false;
  for (std::size_t index = 0; index < size; ++index) {
    cumulative += spectrum[index];
    if (!lower_found && cumulative >= total * 0.005) { lower = index; lower_found = true; }
    if (cumulative >= total * 0.995) { upper = index; break; }
  }
  metrics.bandwidth_hz = (upper - lower + 1) * sample_rate / size;
  return metrics;
}

EnergyBurstDetector::EnergyBurstDetector(double rate, double noise_dbfs, double threshold_db,
                                         double min_seconds, double max_seconds) {
  if (!std::isfinite(rate) || rate < 1e3 || rate > 250e6 || !std::isfinite(noise_dbfs) ||
      noise_dbfs < -200 || noise_dbfs > 0 || !std::isfinite(threshold_db) || threshold_db <= 0 ||
      threshold_db > 100 || !std::isfinite(min_seconds) || !std::isfinite(max_seconds) ||
      min_seconds <= 0 || max_seconds < min_seconds || max_seconds > 0.1)
    throw std::invalid_argument("invalid energy detector configuration");
  noise_power_ = std::pow(10.0, noise_dbfs / 10);
  threshold_ = noise_power_ * std::pow(10.0, threshold_db / 10);
  min_samples_ = std::max<std::size_t>(32, std::ceil(rate * min_seconds));
  max_samples_ = std::max<std::size_t>(32, std::ceil(rate * max_seconds));
  quiet_limit_ = std::max<std::size_t>(32, std::ceil(rate * 20e-6));
}

void EnergyBurstDetector::finish(std::vector<RfBurst>& output, bool truncated) {
  if (!truncated && quiet_samples_ <= current_.samples.size())
    current_.samples.resize(current_.samples.size() - quiet_samples_);
  current_.truncated = truncated || continuation_ || current_.start_sample == 0;
  if (current_.samples.size() >= min_samples_) output.push_back(std::move(current_));
  current_ = {};
  active_ = false;
  quiet_samples_ = 0;
  continuation_ = truncated;
}

void EnergyBurstDetector::consume(std::vector<RfBurst>& output) {
  double power = 0;
  for (const auto& sample : block_) power += std::norm(sample);
  const bool loud = power / block_.size() > threshold_;
  if (!active_ && loud) {
    current_.start_sample = received_ - block_.size();
    active_ = true;
  }
  if (active_) {
    current_.samples.insert(current_.samples.end(), block_.begin(), block_.end());
    quiet_samples_ = loud ? 0 : quiet_samples_ + block_.size();
    if (quiet_samples_ >= quiet_limit_) finish(output, false);
    else if (current_.samples.size() >= max_samples_) finish(output, true);
  } else if (!loud) continuation_ = false;
  block_.clear();
}

std::vector<RfBurst> EnergyBurstDetector::feed(const cf32* samples, std::size_t count, bool final) {
  if (finished_) throw std::logic_error("detector already finalized");
  if (!samples && count) throw std::invalid_argument("null IQ input");
  std::vector<RfBurst> output;
  for (std::size_t index = 0; index < count; ++index) {
    if (!std::isfinite(std::norm(samples[index]))) throw std::invalid_argument("non-finite IQ sample");
    block_.push_back(samples[index]);
    ++received_;
    if (block_.size() == 32) consume(output);
  }
  if (final) {
    if (!block_.empty()) consume(output);
    if (active_) finish(output, true);
    finished_ = true;
  }
  return output;
}

}
