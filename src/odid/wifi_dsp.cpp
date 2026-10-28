// src/odid/wifi_dsp.cpp — small self-contained DSP helpers for the ODID Wi-Fi PHY
// (no FFTW dependency, so the ODID library builds everywhere the core does).
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <complex>

#include "wifi_internal.hpp"

namespace droneid::odid {
namespace {

constexpr double kPi = 3.14159265358979323846;

double bessel_i0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 40; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

}  // namespace

std::vector<cf32> resample(const std::vector<cf32>& in, double fs_in, double fs_out, double shift_hz) {
  std::vector<cf32> x(in);
  if (shift_hz != 0 && !x.empty()) {
    const double w = 2 * kPi * shift_hz / fs_in;
    std::complex<double> rot(1, 0);
    const std::complex<double> step(std::cos(w), std::sin(w));
    for (std::size_t n = 0; n < x.size(); ++n) {
      if ((n & 1023) == 0) rot = std::polar(1.0, w * static_cast<double>(n));
      x[n] *= cf32(static_cast<float>(rot.real()), static_cast<float>(rot.imag()));
      rot *= step;
    }
  }
  if (std::abs(fs_in - fs_out) < 1e-6 || x.empty()) return x;

  // Kaiser-windowed sinc low-pass; tau in input samples.
  const double fc = 0.5 * std::min(1.0, fs_out / fs_in) * (fs_out > fs_in ? 0.95 : 0.9);
  const int half = static_cast<int>(std::ceil(3.2 / fc));
  const double beta = 7.0, i0b = bessel_i0(beta);
  auto kernel = [&](double t) {
    const double u = t / half;
    if (std::abs(u) >= 1) return 0.0;
    const double win = bessel_i0(beta * std::sqrt(1 - u * u)) / i0b;
    const double arg = 2 * fc * t;
    const double sinc = std::abs(arg) < 1e-12 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
    return 2 * fc * sinc * win;
  };

  const double ratio = fs_in / fs_out;
  const auto n_in = static_cast<int64_t>(x.size());
  const auto n_out = static_cast<std::size_t>(std::floor((n_in - 1) / ratio)) + 1;
  std::vector<cf32> y(n_out);

  // Fast path: exact rational ratio fs_out/fs_in = L/M with a small L -> precomputed polyphase.
  int64_t up = 0, down = 0;
  for (int64_t l = 1; l <= 1024 && !up; ++l) {
    const double mm = static_cast<double>(l) * fs_in / fs_out;
    if (std::abs(mm - std::round(mm)) < 1e-9 * mm) { up = l; down = static_cast<int64_t>(std::llround(mm)); }
  }
  if (up) {
    const int taps = 2 * half;
    std::vector<float> coef(static_cast<std::size_t>(up * taps));
    for (int64_t ph = 0; ph < up; ++ph)
      for (int j = 0; j < taps; ++j)
        coef[ph * taps + j] = static_cast<float>(kernel(static_cast<double>(ph) / up + half - 1 - j));
    for (std::size_t m = 0; m < n_out; ++m) {
      const int64_t num = static_cast<int64_t>(m) * down;
      const int64_t ip = num / up, ph = num % up;
      const int64_t first = ip - half + 1;
      const float* h = &coef[ph * taps];
      cf32 acc(0, 0);
      if (first >= 0 && first + taps <= n_in) {
        const cf32* px = &x[first];
        float re = 0, im = 0;
        for (int j = 0; j < taps; ++j) { re += px[j].real() * h[j]; im += px[j].imag() * h[j]; }
        acc = cf32(re, im);
      } else {
        for (int j = 0; j < taps; ++j)
          if (first + j >= 0 && first + j < n_in) acc += x[first + j] * h[j];
      }
      y[m] = acc;
    }
    return y;
  }

  // General ratio: kernel tabulated at kRes points per input sample, linear interpolation.
  constexpr int kRes = 512;
  std::vector<float> table(2 * half * kRes + 2, 0.0f);
  for (std::size_t i = 0; i + 1 < table.size(); ++i)
    table[i] = static_cast<float>(kernel(static_cast<double>(i) / kRes - half));
  for (std::size_t m = 0; m < n_out; ++m) {
    const double p = static_cast<double>(m) * ratio;
    const int64_t i0 = static_cast<int64_t>(std::ceil(p - half));
    const double pos = (p - static_cast<double>(i0) + half) * kRes;
    int64_t ib = static_cast<int64_t>(pos);
    const float f = static_cast<float>(pos - static_cast<double>(ib));
    cf32 acc(0, 0);
    for (int64_t i = i0; ib >= 0; ++i, ib -= kRes) {
      if (i < 0) continue;
      if (i >= n_in) break;
      const float h = table[ib] * (1 - f) + table[ib + 1] * f;
      acc += x[i] * h;
    }
    y[m] = acc;
  }
  return y;
}

namespace detail {

void fft64(const cf32* in, cf32* out, bool inverse) {
  static const auto twiddle = [] {
    std::vector<std::complex<double>> t(32);
    for (int k = 0; k < 32; ++k) t[k] = std::polar(1.0, -2 * kPi * k / 64);
    return t;
  }();
  std::complex<double> a[64];
  for (int i = 0; i < 64; ++i) {
    int r = 0;
    for (int b = 0; b < 6; ++b) r |= ((i >> b) & 1) << (5 - b);
    a[r] = std::complex<double>(in[i].real(), in[i].imag());
  }
  for (int len = 2; len <= 64; len <<= 1) {
    const int step = 64 / len;
    for (int i = 0; i < 64; i += len)
      for (int j = 0; j < len / 2; ++j) {
        auto w = twiddle[j * step];
        if (inverse) w = std::conj(w);
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
      }
  }
  for (int i = 0; i < 64; ++i) out[i] = cf32(static_cast<float>(a[i].real()), static_cast<float>(a[i].imag()));
}

}  // namespace detail
}  // namespace droneid::odid
