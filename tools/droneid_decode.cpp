// tools/droneid_decode.cpp — detect + decode DJI DroneID bursts from a capture.
// GPL-3.0-or-later.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/logging.h"

using namespace droneid;

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s FILE [--format cf32|sc16] [--rate HZ] [--freq-offset HZ]\n"
      "            [--thresh F] [--secs S] [--no-lpf]\n"
      "  --freq-offset shifts the signal before decode (e.g. -4.5e6 to center a DroneID channel\n"
      "  that sits +4.5 MHz in the capture window).\n", p);
}

static void print_hex(const std::vector<uint8_t>& b) {
  for (uint8_t v : b) std::printf("%02x", v);
  std::printf("\n");
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string path = argv[1];
  SampleFormat fmt = SampleFormat::sc16;
  dji::DecodeOptions opt;
  double secs = 4.0;
  bool scan = false;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate")        opt.sample_rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--freq-offset") opt.freq_offset = std::strtod(next().c_str(), nullptr);
    else if (a == "--thresh")      opt.corr_threshold = std::strtof(next().c_str(), nullptr);
    else if (a == "--secs")        secs = std::strtod(next().c_str(), nullptr);
    else if (a == "--no-lpf")      opt.low_pass = false;
    else if (a == "--scan")        scan = true;
    else { usage(argv[0]); return 2; }
  }
  opt.max_samples = static_cast<size_t>(opt.sample_rate * secs);

  if (scan) {
    // Load samples and report peak ZC correlation vs frequency offset.
    FileSource src(path, fmt, opt.sample_rate);
    std::vector<cf32> iq; std::vector<cf32> blk(1u << 16); int64_t t = 0;
    while (iq.size() < opt.max_samples) {
      size_t g = src.recv(blk.data(), blk.size(), t);
      if (!g) break;
      iq.insert(iq.end(), blk.begin(), blk.begin() + std::min(g, opt.max_samples - iq.size()));
    }
    auto sc = dji::scan_freq_offsets(iq, src.sample_rate(), -8e6, 8e6, 0.5e6);
    std::printf("offset(MHz)  maxcorr   at_sample\n");
    for (auto& s : sc)
      std::printf("  %+6.2f     %.4f    %lld\n", s.offset_hz / 1e6, s.max_corr,
                  static_cast<long long>(s.index));
    return 0;
  }

  LOG_INFO("decoding %s (%.1f s, thresh %.2f, freq_offset %.0f Hz, lpf %s)",
           path.c_str(), secs, opt.corr_threshold, opt.freq_offset, opt.low_pass ? "on" : "off");

  std::vector<dji::DecodedBurst> bursts;
  try {
    bursts = dji::decode_file(path, fmt, opt);
  } catch (const std::exception& e) {
    LOG_ERROR("%s", e.what());
    return 1;
  }

  int crc_ok = 0;
  for (size_t i = 0; i < bursts.size(); ++i) {
    const auto& b = bursts[i];
    std::printf("burst %zu: idx=%lld corr=%.3f cfo=%.0f Hz crc=%s\n",
                i, static_cast<long long>(b.sample_index), b.corr, b.cfo_hz,
                b.crc_ok ? "OK" : "fail");
    if (b.crc_ok) {
      ++crc_ok;
      std::printf("  frame: "); print_hex(b.payload);
      dji::Telemetry t;
      if (dji::parse_frame(b.payload, t)) {
        std::printf("  serial : %s\n", t.serial.c_str());
        std::printf("  drone  : %.6f, %.6f  alt %.0f m  height %.0f m\n",
                    t.drone_lat, t.drone_lon, t.altitude_m, t.height_m);
        std::printf("  pilot  : %.6f, %.6f  home %.6f, %.6f\n",
                    t.pilot_lat, t.pilot_lon, t.home_lat, t.home_lon);
      }
    }
  }
  std::printf("== %zu burst candidate(s), %d CRC-valid ==\n", bursts.size(), crc_ok);
  return 0;
}
