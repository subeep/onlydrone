// src/dji/dsp.cpp — FFTW-backed DSP helpers for the DJI decoder.
// GPL-3.0-or-later.
#include "dsp.hpp"

#include <fftw3.h>

#include <cmath>
#include <complex>

namespace droneid::dji::dsp {
namespace {

// One-shot FFT (dir = FFTW_FORWARD / FFTW_BACKWARD), no scaling.
std::vector<cf32> run_fft(const std::vector<cf32>& x, int dir) {
  const int n = static_cast<int>(x.size());
  std::vector<cf32> out(n);
  auto* in = reinterpret_cast<fftwf_complex*>(const_cast<cf32*>(x.data()));
  auto* o = reinterpret_cast<fftwf_complex*>(out.data());
  fftwf_plan p = fftwf_plan_dft_1d(n, in, o, dir, FFTW_ESTIMATE);
  fftwf_execute(p);
  fftwf_destroy_plan(p);
  return out;
}

}  // namespace

std::vector<cf32> fft(const std::vector<cf32>& x) { return run_fft(x, FFTW_FORWARD); }

std::vector<cf32> ifft(const std::vector<cf32>& x) {
  auto out = run_fft(x, FFTW_BACKWARD);
  const float s = 1.0f / static_cast<float>(x.size());
  for (auto& v : out) v *= s;
  return out;
}

std::vector<cf32> fftshift(const std::vector<cf32>& x) {
  const size_t n = x.size();
  const size_t h = n / 2;  // even n
  std::vector<cf32> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = x[(i + h) % n];
  return out;
}

void freq_shift(std::vector<cf32>& x, double freq, double fs, double n0) {
  const double w = 2.0 * M_PI * freq / fs;
  for (size_t i = 0; i < x.size(); ++i) {
    const double ph = w * (n0 + static_cast<double>(i));
    x[i] *= cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
  }
}

std::vector<cf32> fir_lowpass(const std::vector<cf32>& x, double cutoff_hz, double fs, int taps) {
  // Windowed-sinc (Hamming), normalized cutoff fc = cutoff/(fs/2) matching fir1(taps, Wn).
  const double fc = cutoff_hz / (fs / 2.0);
  const int N = taps + 1;
  std::vector<double> h(N);
  double sum = 0;
  for (int i = 0; i < N; ++i) {
    const double m = i - (N - 1) / 2.0;
    const double sinc = (m == 0.0) ? fc : std::sin(M_PI * fc * m) / (M_PI * m);
    const double win = 0.54 - 0.46 * std::cos(2 * M_PI * i / (N - 1));
    h[i] = sinc * win;
    sum += h[i];
  }
  for (double& v : h) v /= sum;  // unity DC gain

  std::vector<cf32> y(x.size(), cf32(0, 0));
  for (size_t n = 0; n < x.size(); ++n) {
    cf32 acc(0, 0);
    const int kmax = std::min<int>(N, static_cast<int>(n) + 1);
    for (int k = 0; k < kmax; ++k) acc += x[n - k] * static_cast<float>(h[k]);
    y[n] = acc;
  }
  return y;
}

std::vector<float> normalized_corr(const std::vector<cf32>& x, const std::vector<cf32>& zc) {
  const int M = static_cast<int>(zc.size());
  const int N = static_cast<int>(x.size());
  if (N < M) return {};

  // Zero-mean template and its sample variance.
  cf32 mean(0, 0);
  for (const auto& v : zc) mean += v;
  mean /= static_cast<float>(M);
  std::vector<cf32> h(M);
  double var_filter = 0;
  for (int k = 0; k < M; ++k) {
    h[k] = zc[k] - mean;
    var_filter += std::norm(h[k]);
  }
  var_filter /= (M - 1);

  // corr[i] = sum_k x[i+k] * conj(h[k]) via linear convolution with reversed-conj kernel.
  std::vector<cf32> grev(M);
  for (int k = 0; k < M; ++k) grev[k] = std::conj(h[M - 1 - k]);

  // Overlap-add convolution, FFT size = next pow2 >= B + M - 1.
  const int B = 8192;
  int L = 1;
  while (L < B + M - 1) L <<= 1;

  std::vector<cf32> kpad(L, cf32(0, 0));
  for (int k = 0; k < M; ++k) kpad[k] = grev[k];
  const std::vector<cf32> K = fft(kpad);

  const int conv_len = N + M - 1;
  std::vector<cf32> conv(conv_len, cf32(0, 0));
  std::vector<cf32> block(L);
  for (int start = 0; start < N; start += B) {
    const int blen = std::min(B, N - start);
    for (int i = 0; i < blen; ++i) block[i] = x[start + i];
    for (int i = blen; i < L; ++i) block[i] = cf32(0, 0);
    std::vector<cf32> Xb = fft(block);
    for (int i = 0; i < L; ++i) Xb[i] *= K[i];
    std::vector<cf32> yb = ifft(Xb);
    const int out_len = blen + M - 1;
    for (int i = 0; i < out_len && start + i < conv_len; ++i) conv[start + i] += yb[i];
  }

  // Running energy and sum over the M-window via prefix sums.
  std::vector<double> p2(N + 1, 0.0);
  std::vector<cf32> p1(N + 1, cf32(0, 0));
  for (int i = 0; i < N; ++i) {
    p2[i + 1] = p2[i] + std::norm(x[i]);
    p1[i + 1] = p1[i] + x[i];
  }

  const int outN = N - M + 1;
  std::vector<float> score(outN, 0.0f);
  for (int i = 0; i < outN; ++i) {
    const double E = p2[i + M] - p2[i];
    const cf32 S = p1[i + M] - p1[i];
    double var_w = (E - std::norm(S) / M) / (M - 1);
    if (var_w <= 0) { score[i] = 0; continue; }
    const cf32 c = conv[i + M - 1];  // == corr[i]
    const double num = std::norm(c);
    const double den = static_cast<double>(M) * M * var_w * var_filter;
    score[i] = den > 0 ? static_cast<float>(num / den) : 0.0f;
  }
  return score;
}

}  // namespace droneid::dji::dsp
