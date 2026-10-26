// droneid/pipeline.h — processing pipeline interface.
// Phase 0 ships only a no-op pipeline that counts samples and mean power, so the
// end-to-end wiring (source -> ring -> pipeline) can be exercised before any DSP exists.
// GPL-3.0-or-later.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "droneid/types.h"

namespace droneid {

class Pipeline {
 public:
  virtual ~Pipeline() = default;
  virtual void process(const cf32* samples, std::size_t n) = 0;
};

// No-op stage for Phase 0: accumulates sample count and average power.
class NoopPipeline : public Pipeline {
 public:
  void process(const cf32* s, std::size_t n) override {
    for (std::size_t i = 0; i < n; ++i) {
      const double re = s[i].real(), im = s[i].imag();
      power_sum_ += re * re + im * im;
    }
    count_ += n;
  }

  std::uint64_t count() const { return count_; }
  double mean_power() const { return count_ ? power_sum_ / static_cast<double>(count_) : 0.0; }
  double mean_power_dbfs() const {
    const double p = mean_power();
    return p > 0.0 ? 10.0 * std::log10(p) : -300.0;
  }

 private:
  std::uint64_t count_ = 0;
  double power_sum_ = 0.0;
};

}  // namespace droneid
