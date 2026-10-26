// tools/iq_gen.cpp — generate a raw IQ test file (complex tone + noise).
// For exercising iq_replay and tests. NOT a transmitter and never fed to a radio.
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "droneid/types.h"

using droneid::cf32;
using droneid::SampleFormat;

static void usage(const char* p) {
  std::fprintf(stderr,
               "usage: %s OUT [--rate HZ] [--tone HZ] [--secs S] [--format cf32|sc16|sc8]\n"
               "              [--noise A] [--amp A]\n",
               p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  std::string out = argv[1];
  double rate = 15.36e6, tone = 100e3, secs = 0.01, noise = 0.01, amp = 0.5;
  SampleFormat fmt = SampleFormat::cf32;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--rate") rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--tone") tone = std::strtod(next().c_str(), nullptr);
    else if (a == "--secs") secs = std::strtod(next().c_str(), nullptr);
    else if (a == "--noise") noise = std::strtod(next().c_str(), nullptr);
    else if (a == "--amp") amp = std::strtod(next().c_str(), nullptr);
    else if (a == "--format") { if (!droneid::parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else { usage(argv[0]); return 2; }
  }

  const std::uint64_t n = static_cast<std::uint64_t>(rate * secs);
  std::FILE* fp = std::fopen(out.c_str(), "wb");
  if (!fp) { std::perror("fopen"); return 1; }

  std::mt19937 rng(1234);
  std::normal_distribution<float> gauss(0.0f, noise);
  const double w = 2.0 * M_PI * tone / rate;

  std::vector<cf32> block(65536);
  std::uint64_t written = 0;
  while (written < n) {
    const std::size_t m = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), n - written));
    for (std::size_t i = 0; i < m; ++i) {
      const double ph = w * static_cast<double>(written + i);
      block[i] = cf32(static_cast<float>(amp * std::cos(ph)) + gauss(rng),
                      static_cast<float>(amp * std::sin(ph)) + gauss(rng));
    }
    switch (fmt) {
      case SampleFormat::cf32:
        std::fwrite(block.data(), sizeof(cf32), m, fp);
        break;
      case SampleFormat::sc16: {
        std::vector<std::int16_t> o(2 * m);
        for (std::size_t i = 0; i < m; ++i) {
          o[2 * i]     = static_cast<std::int16_t>(std::lround(block[i].real() * 32767.0f));
          o[2 * i + 1] = static_cast<std::int16_t>(std::lround(block[i].imag() * 32767.0f));
        }
        std::fwrite(o.data(), sizeof(std::int16_t), o.size(), fp);
        break;
      }
      case SampleFormat::sc8: {
        std::vector<std::int8_t> o(2 * m);
        for (std::size_t i = 0; i < m; ++i) {
          o[2 * i]     = static_cast<std::int8_t>(std::lround(block[i].real() * 127.0f));
          o[2 * i + 1] = static_cast<std::int8_t>(std::lround(block[i].imag() * 127.0f));
        }
        std::fwrite(o.data(), sizeof(std::int8_t), o.size(), fp);
        break;
      }
    }
    written += m;
  }
  std::fclose(fp);

  // Write a sidecar so iq_replay can auto-detect format/rate.
  std::FILE* meta = std::fopen((out + ".meta").c_str(), "wb");
  if (meta) {
    std::fprintf(meta, "format = %s\nsample_rate = %.0f\ncenter_freq = 0\n", droneid::to_string(fmt), rate);
    std::fclose(meta);
  }
  std::printf("wrote %llu samples (%s) to %s (+.meta)\n",
              static_cast<unsigned long long>(written), droneid::to_string(fmt), out.c_str());
  return 0;
}
