// tools/channelize.cpp — digital downconvert + decimate one channel out of a wideband capture.
// Streams the input (bounded memory), shifts the chosen channel to baseband, low-pass filters,
// and decimates to produce a 15.36 MSPS file the DroneID decoder can consume directly.
// This is how we "sweep the band": capture wide once, channelize each DroneID channel, decode each.
// GPL-3.0-or-later.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "droneid/file_source.h"
#include "droneid/logging.h"

using namespace droneid;

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s IN OUT --offset HZ [--decim D] [--in-rate HZ] [--format cf32|sc16]\n"
      "  Shifts channel at +OFFSET Hz to baseband and decimates by D (default from rates).\n"
      "  Output is sc16 at in_rate/D with a .meta sidecar.\n", p);
}

int main(int argc, char** argv) {
  if (argc < 3) { usage(argv[0]); return 2; }
  const std::string in = argv[1], out = argv[2];
  SampleFormat fmt = SampleFormat::sc16;
  double offset = 0, in_rate = 0;
  int decim = 0;

  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--offset") offset = std::strtod(next().c_str(), nullptr);
    else if (a == "--decim") decim = std::atoi(next().c_str());
    else if (a == "--in-rate") in_rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else { usage(argv[0]); return 2; }
  }

  FileSource src(in, fmt, in_rate > 0 ? in_rate : 15.36e6);
  in_rate = src.sample_rate();
  if (decim <= 0) decim = static_cast<int>(std::lround(in_rate / 15.36e6));
  if (decim < 1) decim = 1;
  const double out_rate = in_rate / decim;

  // Anti-alias low-pass: windowed-sinc (Hamming), cutoff just under the output Nyquist.
  // Choose the tap count so the group delay (ntaps-1)/2 is an INTEGER number of output
  // samples (a multiple of decim) — otherwise the fractional delay rotates the constellation.
  int ntaps = 64 - (64 % (2 * decim)) + 1;
  if (ntaps < 2 * decim + 1) ntaps = 2 * decim + 1;
  const double fc = 0.45 * out_rate / (in_rate / 2.0);  // normalized to in_rate Nyquist
  std::vector<float> h(ntaps);
  double hsum = 0;
  for (int i = 0; i < ntaps; ++i) {
    const double m = i - (ntaps - 1) / 2.0;
    const double s = (m == 0) ? fc : std::sin(M_PI * fc * m) / (M_PI * m);
    const double w = 0.54 - 0.46 * std::cos(2 * M_PI * i / (ntaps - 1));
    h[i] = static_cast<float>(s * w);
    hsum += h[i];
  }
  for (float& v : h) v /= hsum;

  std::FILE* of = std::fopen(out.c_str(), "wb");
  if (!of) { std::perror("fopen"); return 1; }

  std::vector<cf32> delay(ntaps, cf32(0, 0));
  int dpos = 0;
  const double w0 = -2.0 * M_PI * offset / in_rate;
  int64_t n = 0;            // global input sample index (for continuous phase)
  int64_t since = 0;        // samples since last output
  std::vector<cf32> in_blk(1u << 16);
  std::vector<std::int16_t> out_blk;
  int64_t t = 0, out_count = 0;

  for (;;) {
    size_t got = src.recv(in_blk.data(), in_blk.size(), t);
    if (got == 0) break;
    out_blk.clear();
    for (size_t i = 0; i < got; ++i) {
      const double ph = w0 * static_cast<double>(n++);
      const cf32 s = in_blk[i] * cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
      delay[dpos] = s;
      dpos = (dpos + 1) % ntaps;
      if (++since == decim) {
        since = 0;
        cf32 acc(0, 0);
        int idx = dpos;  // oldest sample
        for (int k = ntaps - 1; k >= 0; --k) {
          acc += delay[idx] * h[k];
          idx = (idx + 1) % ntaps;
        }
        out_blk.push_back(static_cast<std::int16_t>(std::lround(acc.real() * 32767.0f)));
        out_blk.push_back(static_cast<std::int16_t>(std::lround(acc.imag() * 32767.0f)));
        ++out_count;
      }
    }
    if (!out_blk.empty()) std::fwrite(out_blk.data(), sizeof(std::int16_t), out_blk.size(), of);
  }
  std::fclose(of);

  std::FILE* m = std::fopen((out + ".meta").c_str(), "wb");
  if (m) {
    std::fprintf(m, "format = sc16\nsample_rate = %.0f\ncenter_freq = %.0f\n",
                 out_rate, src.center_freq() + offset);
    std::fclose(m);
  }
  LOG_INFO("channelize: %s offset %+.1f MHz decim %d -> %lld samples @ %.3f MSPS -> %s",
           in.c_str(), offset / 1e6, decim, static_cast<long long>(out_count), out_rate / 1e6,
           out.c_str());
  return 0;
}
