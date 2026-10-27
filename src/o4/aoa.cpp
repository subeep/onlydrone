#include "droneid/aoa.h"
#include "droneid/config.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace droneid {

PhaseCal PhaseCal::load(const std::string& path) {
  const auto config = Config::load(path);
  auto number = [&](const std::string& key) {
    const auto value = config.get(key);
    std::size_t used = 0;
    const double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result))
      throw std::invalid_argument("invalid calibration field: " + key);
    return result;
  };
  PhaseCal calibration;
  calibration.phase_rad = number("phase_rad");
  calibration.center_hz = number("center_hz");
  calibration.sample_rate = number("sample_rate");
  calibration.spacing_m = number("spacing_m");
  calibration.max_age_seconds = number("max_age_seconds");
  const auto timestamp = config.get("measured_unix_ns");
  std::size_t used = 0;
  calibration.measured_unix_ns = std::stoll(timestamp, &used);
  if (used != timestamp.size()) throw std::invalid_argument("invalid calibration timestamp");
  return calibration;
}

Bearing estimate_bearing(const std::vector<cf32>& first, const std::vector<cf32>& second,
                         int64_t first_ns, int64_t second_ns, double center_hz,
                         double sample_rate, const PhaseCal& calibration, int64_t now_unix_ns) {
  Bearing result;
  auto reject = [&](const char* reason) { result.reason = reason; return result; };
  if (!std::isfinite(center_hz) || center_hz <= 0 || !std::isfinite(sample_rate) || sample_rate <= 0 ||
      !std::isfinite(calibration.phase_rad) || !std::isfinite(calibration.center_hz) ||
      !std::isfinite(calibration.sample_rate) || !std::isfinite(calibration.spacing_m) ||
      !std::isfinite(calibration.max_age_seconds)) return reject("invalid calibration or RF settings");
  if (first.size() != second.size() || first.size() < 32 || first_ns != second_ns)
    return reject("channels must have equal lengths and aligned timestamps");
  if (calibration.measured_unix_ns <= 0 || now_unix_ns < calibration.measured_unix_ns ||
      calibration.max_age_seconds <= 0 ||
      (static_cast<long double>(now_unix_ns) - calibration.measured_unix_ns) / 1e9 > calibration.max_age_seconds)
    return reject("calibration missing, future-dated or expired");
  if (std::abs(calibration.center_hz - center_hz) > 1 ||
      std::abs(calibration.sample_rate - sample_rate) > 1)
    return reject("calibration frequency or sample rate mismatch");
  const double wavelength = 299792458.0 / center_hz;
  if (calibration.spacing_m <= 0 || calibration.spacing_m > wavelength / 2)
    return reject("antenna spacing must be positive and no greater than half a wavelength");
  std::complex<double> cross(0, 0);
  double first_power = 0, second_power = 0;
  for (std::size_t index = 0; index < first.size(); ++index) {
    const std::complex<double> left = first[index], right = second[index];
    if (!std::isfinite(std::norm(left)) || !std::isfinite(std::norm(right)))
      return reject("non-finite samples");
    cross += std::conj(left) * right;
    first_power += std::norm(left);
    second_power += std::norm(right);
  }
  if (first_power <= 0 || second_power <= 0) return reject("zero signal power");
  result.coherence = std::clamp(std::abs(cross) / std::sqrt(first_power * second_power), 0.0, 1.0);
  if (result.coherence < 0.8) return reject("insufficient channel coherence");
  const double pi = std::acos(-1.0);
  const double phase = std::remainder(std::arg(cross) - calibration.phase_rad, 2 * pi);
  const double sine = phase * wavelength / (2 * pi * calibration.spacing_m);
  if (std::abs(sine) > 1.0 + 1e-6) return reject("phase inconsistent with antenna spacing");
  result.broadside_deg = std::asin(std::clamp(sine, -1.0, 1.0)) * 180 / pi;
  result.valid = true;
  return result;
}

}
