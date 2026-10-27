// tools/droneid_decode.cpp — detect + decode DJI DroneID bursts from a capture.
// GPL-3.0-or-later.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <limits>
#include <stdexcept>
#include <algorithm>

#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/logging.h"

using namespace droneid;

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s FILE [--format cf32|sc16|sc8] [--rate HZ]\n"
      "            [--freq-offset HZ | --auto-center] [--thresh F] [--secs S] [--no-lpf]\n"
      "            [--scan]\n"
      "  Input is resampled to 15.36 MSPS. --secs 0 reads the whole file.\n"
      "  --auto-center selects the strongest 10 MHz band in the first 1048576 input samples.\n"
      "  --scan reports offset scores over at most the first 1048576 input samples.\n"
      "  --freq-offset shifts the signal before decode (e.g. -4.5e6 to center a DroneID channel\n"
      "  that sits +4.5 MHz in the capture window).\n", p);
}

static void print_hex(const std::vector<uint8_t>& b) {
  for (uint8_t v : b) std::printf("%02x", v);
  std::printf("\n");
}

static int run(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string path = argv[1];
  SampleFormat fmt = SampleFormat::sc16;
  dji::DecodeOptions opt;
  double secs = 4.0;
  bool scan = false;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
      return argv[++i];
    };
    auto number = [&]() {
      const auto value = next();
      size_t used = 0;
      const double parsed = std::stod(value, &used);
      if (used != value.size() || !std::isfinite(parsed))
        throw std::invalid_argument("invalid numeric value for " + a);
      return parsed;
    };
    if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate")        opt.sample_rate = number();
    else if (a == "--freq-offset") opt.freq_offset = number();
    else if (a == "--thresh")      opt.corr_threshold = number();
    else if (a == "--secs")        secs = number();
    else if (a == "--auto-center") opt.auto_center = true;
    else if (a == "--no-lpf")      opt.low_pass = false;
    else if (a == "--scan")        scan = true;
    else { usage(argv[0]); return 2; }
  }
  if (secs < 0 || opt.sample_rate < 10e6 || opt.sample_rate > 250e6)
    throw std::invalid_argument("seconds must be nonnegative; input rate must be 10–250 MSPS");
  FileSource metadata(path, fmt, opt.sample_rate);
  opt.sample_rate = metadata.sample_rate();
  if (!std::isfinite(opt.sample_rate) || opt.sample_rate < 10e6 || opt.sample_rate > 250e6)
    throw std::invalid_argument("invalid sample rate in file metadata");
  const long double requested_samples = static_cast<long double>(opt.sample_rate) * secs;
  if (requested_samples >= std::numeric_limits<size_t>::max() || (secs > 0 && requested_samples < 1))
    throw std::invalid_argument("sample duration is outside the supported range");
  opt.max_samples = static_cast<size_t>(requested_samples);
  if (opt.auto_center && opt.freq_offset != 0)
    throw std::invalid_argument("--auto-center cannot be combined with --freq-offset");

  if (scan) {
    if (opt.auto_center || opt.freq_offset != 0)
      throw std::invalid_argument("--scan cannot be combined with centering options");
    // Load samples and report peak ZC correlation vs frequency offset.
    FileSource src(path, fmt, opt.sample_rate);
    std::vector<cf32> iq; std::vector<cf32> blk(1u << 16); int64_t t = 0;
    const size_t scan_limit = opt.max_samples ? std::min<size_t>(opt.max_samples, 1u << 20) : 1u << 20;
    while (iq.size() < scan_limit) {
      size_t g = src.recv(blk.data(), blk.size(), t);
      if (!g) break;
      iq.insert(iq.end(), blk.begin(), blk.begin() + std::min(g, scan_limit - iq.size()));
    }
    const double extent = std::floor((src.sample_rate() / 2 - 5e6) / 0.5e6) * 0.5e6;
    auto sc = dji::scan_freq_offsets(iq, src.sample_rate(), -extent, extent, 0.5e6);
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
      const auto envelope = dji::inspect_packet(b.payload);
      std::printf("  envelope: format=%s logical_bytes=%zu inner_crc16=%s\n",
                  envelope.format.c_str(), envelope.logical_bytes,
                  !envelope.length_valid ? "invalid_length" : envelope.crc16_ok ? "OK" : "fail");
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

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    LOG_ERROR("%s", error.what());
    return 1;
  }
}
