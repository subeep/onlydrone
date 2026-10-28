// tools/odid_gen.cpp — write a SYNTHETIC IQ file of Open Drone ID Wi-Fi frames (fixtures only).
// Builds F3411 Message Packs, wraps them in a beacon or NAN action MPDU, modulates them with the
// 802.11 OFDM or DSSS fixture synthesizer and adds noise / CFO / clock offset. Nothing here is
// transmitted; the output is a file for odid_decode, the UI replay path and tests.
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "droneid/odid_wifi.h"

using namespace droneid;
using namespace droneid::odid;

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s OUT [--rate HZ] [--format cf32|sc16] [--phy ofdm|dsss] [--mbps N] [--short-preamble]\n"
      "            [--transport beacon|nan] [--count N] [--gap-ms MS] [--snr dB] [--cfo HZ] [--ppm PPM]\n"
      "            [--serial ID] [--lat DEG] [--lon DEG] [--seed N]\n"
      "  Writes OUT and OUT.meta. Synthetic fixture only (no radio involved).\n", p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string out = argv[1];
  double rate = 20e6, gap_ms = 2, snr_db = 30, cfo = 0, ppm = 0, lat = 51.4779, lon = -0.0015;
  std::string format = "cf32", phy = "ofdm", transport = "beacon", serial = "SYNTH-ODID-0001";
  int mbps = 6, count = 3;
  unsigned seed = 1;
  bool short_preamble = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
      return argv[++i];
    };
    if (a == "--rate") rate = std::atof(next().c_str());
    else if (a == "--format") format = next();
    else if (a == "--phy") phy = next();
    else if (a == "--mbps") mbps = std::atoi(next().c_str());
    else if (a == "--short-preamble") short_preamble = true;
    else if (a == "--transport") transport = next();
    else if (a == "--count") count = std::atoi(next().c_str());
    else if (a == "--gap-ms") gap_ms = std::atof(next().c_str());
    else if (a == "--snr") snr_db = std::atof(next().c_str());
    else if (a == "--cfo") cfo = std::atof(next().c_str());
    else if (a == "--ppm") ppm = std::atof(next().c_str());
    else if (a == "--serial") serial = next();
    else if (a == "--lat") lat = std::atof(next().c_str());
    else if (a == "--lon") lon = std::atof(next().c_str());
    else if (a == "--seed") seed = static_cast<unsigned>(std::atoi(next().c_str()));
    else { usage(argv[0]); return 2; }
  }
  if (rate < 20e6 || rate > 250e6 || count < 1 || count > 100000 || (format != "cf32" && format != "sc16") ||
      (phy != "ofdm" && phy != "dsss") || (transport != "beacon" && transport != "nan")) {
    usage(argv[0]);
    return 2;
  }

  const Mac mac = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x01};  // locally administered
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  const float sigma = std::sqrt(std::pow(10.0f, static_cast<float>(-snr_db) / 10.0f) / 2.0f);
  const auto gap = static_cast<std::size_t>(gap_ms * 1e-3 * rate);
  std::vector<cf32> iq;
  for (int k = 0; k < count; ++k) {
    BasicId id;
    id.id_type = 1;
    id.ua_type = 2;
    id.uas_id = serial;
    Location loc;
    loc.status = 2;
    loc.direction_deg = 90;
    loc.speed_h_mps = 5.25;
    loc.speed_v_mps = 0.5;
    loc.lat = lat + 1e-5 * k;
    loc.lon = lon;
    loc.alt_baro_m = 120;
    loc.alt_geo_m = 125.5;
    loc.height_m = 50;
    loc.h_accuracy = 10;
    loc.v_accuracy = 4;
    loc.timestamp_raw = static_cast<uint16_t>((1200 + k) % 36000);
    System sys;
    sys.operator_location_type = 1;
    sys.operator_lat = lat - 1e-3;
    sys.operator_lon = lon + 1e-3;
    sys.area_count = 1;
    sys.operator_alt_geo_m = 70;
    sys.timestamp = 245000000;
    SelfId self;
    self.desc = "synthetic fixture";
    OperatorId op;
    op.id = "OP-SYNTH-0001";
    const auto pack = encode_pack({encode(id), encode(loc), encode(self), encode(sys), encode(op)});
    const auto mpdu = transport == "beacon" ? build_beacon_mpdu(mac, "ODID-SYNTH", static_cast<uint8_t>(k), pack)
                                            : build_nan_action_mpdu(mac, static_cast<uint8_t>(k), pack);
    const double fs_tx = rate * (1 + ppm * 1e-6);
    const auto ppdu = phy == "ofdm" ? synth_ofdm_ppdu(mpdu, mbps, fs_tx) : synth_dsss_ppdu(mpdu, mbps, short_preamble, fs_tx);
    if (ppdu.empty()) { std::fprintf(stderr, "unsupported rate %d Mb/s for %s\n", mbps, phy.c_str()); return 2; }
    iq.resize(iq.size() + gap, cf32(0, 0));
    iq.insert(iq.end(), ppdu.begin(), ppdu.end());
  }
  iq.resize(iq.size() + gap, cf32(0, 0));
  const double w = 2 * 3.14159265358979323846 * cfo / rate;
  for (std::size_t n = 0; n < iq.size(); ++n) {
    iq[n] *= std::polar(1.0f, static_cast<float>(std::fmod(w * static_cast<double>(n), 2 * 3.14159265358979323846)));
    iq[n] += cf32(sigma * gauss(rng), sigma * gauss(rng));
    iq[n] *= 0.25f;  // headroom for sc16
  }

  std::FILE* fp = std::fopen(out.c_str(), "wb");
  if (!fp) { std::perror("fopen"); return 1; }
  if (format == "cf32") {
    std::fwrite(iq.data(), sizeof(cf32), iq.size(), fp);
  } else {
    std::vector<int16_t> s(2 * iq.size());
    for (std::size_t n = 0; n < iq.size(); ++n) {
      s[2 * n] = static_cast<int16_t>(std::lround(std::clamp(iq[n].real(), -1.0f, 1.0f) * 32767));
      s[2 * n + 1] = static_cast<int16_t>(std::lround(std::clamp(iq[n].imag(), -1.0f, 1.0f) * 32767));
    }
    std::fwrite(s.data(), sizeof(int16_t), s.size(), fp);
  }
  std::fclose(fp);
  if (std::FILE* meta = std::fopen((out + ".meta").c_str(), "wb")) {
    std::fprintf(meta, "format = %s\nsample_rate = %.0f\ncenter_freq = 2437000000\nsynthetic = odid_gen %s %s %d Mb/s\n",
                 format.c_str(), rate, transport.c_str(), phy.c_str(), mbps);
    std::fclose(meta);
  }
  std::printf("wrote %zu samples (%d frame(s), %s %s %d Mb/s) to %s\n", iq.size(), count, transport.c_str(),
              phy.c_str(), mbps, out.c_str());
  return 0;
}
