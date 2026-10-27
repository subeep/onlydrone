// tools/frame_gen.cpp — synthesize a DJI DroneID burst to a file for end-to-end testing.
// This is NEVER transmitted; it exists so the decoder can be validated without a radio.
// GPL-3.0-or-later.
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "droneid/dji.h"
#include "droneid/logging.h"

using namespace droneid;

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s OUT [--rate HZ] [--snr-db X] [--pad N] [--serial STR]\n"
      "  Writes a synthetic DroneID burst as cf32 (+ .meta). For decoder tests only.\n", p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string out = argv[1];
  double fs = 15.36e6, snr_db = 30.0;
  int pad = 8192;
  std::string serial = "FRAMEGEN0000TEST";

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--rate") fs = std::strtod(next().c_str(), nullptr);
    else if (a == "--snr-db") snr_db = std::strtod(next().c_str(), nullptr);
    else if (a == "--pad") pad = std::atoi(next().c_str());
    else if (a == "--serial") serial = next();
    else { usage(argv[0]); return 2; }
  }

  // Build a plausible 91-byte user payload with an ASCII serial at the documented offset.
  std::vector<uint8_t> user(91, 0);
  user[0] = 88;
  user[1] = 0x10;
  user[2] = 2;
  user[3] = 0x34; user[4] = 0x12;
  for (size_t i = 0; i < serial.size() && i < 16; ++i) user[7 + i] = static_cast<uint8_t>(serial[i]);
  const uint16_t checksum = dji::packet_crc16(user.data(), user.size() - 2);
  user[user.size() - 2] = checksum & 0xff;
  user[user.size() - 1] = checksum >> 8;

  const auto frame176 = dji::build_frame176(user);
  std::vector<uint8_t> bits7200;
  if (!dji::turbo_encode_frame(frame176, bits7200)) { LOG_ERROR("encode failed"); return 1; }

  // Scramble (air bits = coded bits XOR scrambler).
  const auto scr = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (int i = 0; i < 7200; ++i) bits7200[i] ^= scr[i];

  auto burst = dji::synthesize_burst(bits7200, fs);

  // Normalize power, add padding + AWGN.
  double p = 0; for (auto& s : burst) p += std::norm(s);
  p /= burst.size();
  const float scale = p > 0 ? static_cast<float>(1.0 / std::sqrt(p)) : 1.0f;
  for (auto& s : burst) s *= scale;

  const double noise_amp = std::pow(10.0, -snr_db / 20.0);
  std::mt19937 rng(7);
  std::normal_distribution<float> g(0.0f, static_cast<float>(noise_amp / std::sqrt(2.0)));

  std::vector<cf32> stream;
  stream.reserve(burst.size() + 2 * pad);
  auto add_noise = [&](int n) { for (int i = 0; i < n; ++i) stream.emplace_back(g(rng), g(rng)); };
  add_noise(pad);
  for (auto& s : burst) stream.emplace_back(s.real() + g(rng), s.imag() + g(rng));
  add_noise(pad);

  std::FILE* f = std::fopen(out.c_str(), "wb");
  if (!f) { std::perror("fopen"); return 1; }
  std::fwrite(stream.data(), sizeof(cf32), stream.size(), f);
  std::fclose(f);
  std::FILE* m = std::fopen((out + ".meta").c_str(), "wb");
  if (m) { std::fprintf(m, "format = cf32\nsample_rate = %.0f\ncenter_freq = 0\n", fs); std::fclose(m); }

  LOG_INFO("wrote synthetic burst: %zu samples, serial '%s', snr %.0f dB -> %s",
           stream.size(), serial.c_str(), snr_db, out.c_str());
  return 0;
}
