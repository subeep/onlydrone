// tools/burst_scan.cpp — offline burst analyzer for Phase 1 characterization.
// Reads a capture, finds energy bursts by power gating above the noise floor, and for
// each measures duration; for a representative burst it estimates occupied bandwidth via
// an averaged FFT. Reports inter-burst period. Output: human summary + optional JSON.
//
// This is how we CONFIRM the §2 constants against a real Mini 2 / Air 3S capture instead
// of assuming them. It does not decode — it characterizes.
// GPL-3.0-or-later.
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "droneid/file_source.h"
#include "droneid/logging.h"

using namespace droneid;

namespace {

struct BurstSpan {
  std::uint64_t start;   // sample index
  std::uint64_t length;  // samples
};

double percentile(std::vector<float> v, double pct) {
  if (v.empty()) return 0;
  const auto k = static_cast<std::size_t>(pct / 100.0 * (v.size() - 1));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

}  // namespace

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s FILE [--format cf32|sc16|sc8] [--rate HZ] [--thresh-db X]\n"
      "            [--min-us U] [--json]\n"
      "  (a FILE.meta sidecar overrides --format/--rate)\n", p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string path = argv[1];
  SampleFormat fmt = SampleFormat::cf32;
  double rate = sig::kChannelRate;
  double thresh_db = 10.0;   // dB above noise floor to call a burst
  double min_us = 50.0;      // ignore blips shorter than this
  bool json = false;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate")      rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--thresh-db") thresh_db = std::strtod(next().c_str(), nullptr);
    else if (a == "--min-us")    min_us = std::strtod(next().c_str(), nullptr);
    else if (a == "--json")      json = true;
    else { usage(argv[0]); return 2; }
  }

  // Load the whole capture (captures are short).
  FileSource src(path, fmt, rate);
  rate = src.sample_rate();
  std::vector<cf32> iq;
  {
    std::vector<cf32> blk(1u << 16);
    int64_t t = 0;
    for (;;) {
      std::size_t got = src.recv(blk.data(), blk.size(), t);
      if (got == 0) break;
      iq.insert(iq.end(), blk.begin(), blk.begin() + got);
    }
  }
  if (iq.empty()) { LOG_ERROR("no samples in %s", path.c_str()); return 1; }

  // Instantaneous power, then a short moving-average envelope.
  const std::size_t N = iq.size();
  std::vector<float> pw(N);
  for (std::size_t i = 0; i < N; ++i)
    pw[i] = iq[i].real() * iq[i].real() + iq[i].imag() * iq[i].imag();

  const std::size_t win = std::max<std::size_t>(16, static_cast<std::size_t>(rate * 1e-6));  // ~1 us
  std::vector<float> env(N, 0.0f);
  double acc = 0;
  for (std::size_t i = 0; i < N; ++i) {
    acc += pw[i];
    if (i >= win) acc -= pw[i - win];
    env[i] = static_cast<float>(acc / std::min<std::size_t>(win, i + 1));
  }

  // Noise floor = 20th percentile of the envelope; threshold above it.
  const double noise = percentile(env, 20.0);
  const double thresh = noise * std::pow(10.0, thresh_db / 10.0);
  const auto min_len = static_cast<std::uint64_t>(rate * min_us * 1e-6);

  // Gate into bursts.
  std::vector<BurstSpan> bursts;
  bool in = false;
  std::uint64_t start = 0;
  for (std::size_t i = 0; i < N; ++i) {
    if (!in && env[i] > thresh) { in = true; start = i; }
    else if (in && env[i] <= thresh) {
      in = false;
      const std::uint64_t len = i - start;
      if (len >= min_len) bursts.push_back({start, len});
    }
  }
  if (in && (N - start) >= min_len) bursts.push_back({start, N - start});

  // Durations + inter-burst periods.
  std::vector<double> durs_us, periods_ms;
  for (const auto& b : bursts) durs_us.push_back(b.length / rate * 1e6);
  for (std::size_t i = 1; i < bursts.size(); ++i)
    periods_ms.push_back((bursts[i].start - bursts[i - 1].start) / rate * 1e3);

  auto median = [](std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };

  // Occupied bandwidth from the largest burst (averaged 1024-pt FFT, 99% power width).
  double occ_bw_hz = 0, peak_offset_hz = 0;
  int fft_symbols_est = 0;
  if (!bursts.empty()) {
    const auto& big = *std::max_element(bursts.begin(), bursts.end(),
        [](const BurstSpan& a, const BurstSpan& b) { return a.length < b.length; });
    const int Nf = 1024;
    std::vector<double> psd(Nf, 0.0);
    auto* in_buf = fftwf_alloc_complex(Nf);
    auto* out_buf = fftwf_alloc_complex(Nf);
    fftwf_plan plan = fftwf_plan_dft_1d(Nf, in_buf, out_buf, FFTW_FORWARD, FFTW_ESTIMATE);
    int nseg = 0;
    for (std::uint64_t off = 0; off + Nf <= big.length; off += Nf / 2) {
      for (int k = 0; k < Nf; ++k) {
        const cf32& s = iq[big.start + off + k];
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * k / (Nf - 1));  // Hann
        in_buf[k][0] = static_cast<float>(s.real() * w);
        in_buf[k][1] = static_cast<float>(s.imag() * w);
      }
      fftwf_execute(plan);
      for (int k = 0; k < Nf; ++k)
        psd[k] += out_buf[k][0] * out_buf[k][0] + out_buf[k][1] * out_buf[k][1];
      ++nseg;
    }
    fftwf_destroy_plan(plan);
    fftwf_free(in_buf);
    fftwf_free(out_buf);

    if (nseg > 0) {
      double total = 0;
      for (double v : psd) total += v;
      // fftshift view: index -> frequency offset.
      double cum = 0;
      int lo = -1, hi = -1;
      // walk shifted order
      std::vector<std::pair<double, int>> binf(Nf);
      for (int k = 0; k < Nf; ++k) {
        int shifted = (k + Nf / 2) % Nf;
        double f = (static_cast<double>(k) - Nf / 2) * rate / Nf;
        binf[k] = {f, shifted};
      }
      for (int k = 0; k < Nf; ++k) {
        cum += psd[binf[k].second];
        if (lo < 0 && cum >= 0.005 * total) lo = k;
        if (cum <= 0.995 * total) hi = k;
      }
      if (lo >= 0 && hi >= lo) occ_bw_hz = (hi - lo) * rate / Nf;
      // peak offset
      int pk = 0; double pv = psd[binf[0].second];
      for (int k = 1; k < Nf; ++k) if (psd[binf[k].second] > pv) { pv = psd[binf[k].second]; pk = k; }
      peak_offset_hz = binf[pk].first;
      // rough symbol count: burst duration / (1/scs with normal CP ~ 71.4 us? no) ->
      // estimate #OFDM symbols assuming 1024-FFT + ~72-sample CP at this rate.
      const double sym_len = (1024 + 72) / rate;
      fft_symbols_est = static_cast<int>(std::lround((big.length / rate) / sym_len));
    }
  }

  const double cap_s = N / rate;
  if (json) {
    std::printf("{\n");
    std::printf("  \"file\": \"%s\",\n", path.c_str());
    std::printf("  \"sample_rate_hz\": %.0f,\n", rate);
    std::printf("  \"capture_s\": %.4f,\n", cap_s);
    std::printf("  \"noise_floor\": %.3e,\n", noise);
    std::printf("  \"threshold_db\": %.1f,\n", thresh_db);
    std::printf("  \"bursts\": %zu,\n", bursts.size());
    std::printf("  \"median_burst_us\": %.2f,\n", median(durs_us));
    std::printf("  \"median_period_ms\": %.3f,\n", median(periods_ms));
    std::printf("  \"occupied_bw_hz\": %.0f,\n", occ_bw_hz);
    std::printf("  \"peak_offset_hz\": %.0f,\n", peak_offset_hz);
    std::printf("  \"ofdm_symbols_est\": %d\n", fft_symbols_est);
    std::printf("}\n");
  } else {
    std::printf("== burst_scan: %s ==\n", path.c_str());
    std::printf("sample rate     : %.3f MSPS\n", rate / 1e6);
    std::printf("capture length  : %.4f s (%zu samples)\n", cap_s, N);
    std::printf("noise floor     : %.3e   threshold: +%.1f dB\n", noise, thresh_db);
    std::printf("bursts found    : %zu\n", bursts.size());
    std::printf("median burst    : %.2f us\n", median(durs_us));
    std::printf("median period   : %.3f ms  (~%.1f bursts/s)\n",
                median(periods_ms), median(periods_ms) > 0 ? 1000.0 / median(periods_ms) : 0.0);
    std::printf("occupied BW     : %.3f MHz\n", occ_bw_hz / 1e6);
    std::printf("peak offset     : %+.1f kHz\n", peak_offset_hz / 1e3);
    std::printf("OFDM symbols est: %d  (expect 8-9 if DroneID; §2)\n", fft_symbols_est);
    std::printf("\nCompare against §2: BW~%.1f MHz, 8-9 symbols. Update the plan with what you see.\n",
                600 * sig::kScsHz / 1e6);
  }
  return 0;
}
