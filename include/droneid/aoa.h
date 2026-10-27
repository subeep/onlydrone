#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "droneid/types.h"

namespace droneid {

struct PhaseCal {
  double phase_rad = 0;
  double center_hz = 0;
  double sample_rate = 0;
  double spacing_m = 0;
  int64_t measured_unix_ns = 0;
  double max_age_seconds = 3600;
  static PhaseCal load(const std::string& path);
};

struct Bearing {
  bool valid = false;
  double broadside_deg = 0;
  double coherence = 0;
  bool ambiguous_front_back = true;
  std::string reason;
};

Bearing estimate_bearing(const std::vector<cf32>& first, const std::vector<cf32>& second,
                         int64_t first_ns, int64_t second_ns, double center_hz,
                         double sample_rate, const PhaseCal& calibration, int64_t now_unix_ns);

}
