// tools/iq_record.cpp — capture live IQ from the X310 to a raw file + metadata sidecar.
// Receive-only. Writes samples exactly as they arrive (fc32/cf32), or converts to sc16
// on the way out to halve file size. The .meta sidecar lets iq_replay/burst_scan read it back.
// GPL-3.0-or-later.
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "droneid/config.h"
#include "droneid/logging.h"
#include "droneid/usrp_source.h"

using namespace droneid;

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop.store(true); }

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s OUT [--config PATH] [--freq HZ] [--rate HZ] [--gain dB]\n"
      "            [--ant NAME] [--secs S] [--wire sc16|sc8] [--store cf32|sc16]\n"
      "            [--clock internal|external|gpsdo]\n"
      "  Ctrl-C stops early. Writes OUT and OUT.meta.\n", p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string out = argv[1];

  UsrpParams up;
  double secs = 10.0;
  std::string store = "cf32";  // on-disk format: cf32 (lossless) or sc16 (half size)
  std::string config_path;

  // Config provides DEFAULTS, so load it first; CLI flags below then override it.
  for (int i = 2; i < argc; ++i)
    if (std::string(argv[i]) == "--config" && i + 1 < argc) config_path = argv[i + 1];
  if (!config_path.empty()) {
    try {
      Config c = Config::load(config_path);
      up.device_args = c.device_args;
      up.master_clock_rate = c.master_clock_rate;
      up.sample_rate = c.sample_rate;
      up.center_freq = c.center_freq;
      up.gain_db = c.gain_db;
      up.antenna = c.antenna;
      up.wire_format = c.wire_format;
    } catch (const std::exception& e) {
      LOG_ERROR("%s", e.what());
      return 1;
    }
  }

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--config") { next(); }  // already handled above
    else if (a == "--freq")  up.center_freq = std::strtod(next().c_str(), nullptr);
    else if (a == "--rate")  up.sample_rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--gain")  up.gain_db = std::strtod(next().c_str(), nullptr);
    else if (a == "--ant")   up.antenna = next();
    else if (a == "--secs")  secs = std::strtod(next().c_str(), nullptr);
    else if (a == "--wire")  up.wire_format = next();
    else if (a == "--store") store = next();
    else if (a == "--clock") { up.clock_source = next(); up.time_source = up.clock_source; }
    else { usage(argv[0]); return 2; }
  }

  std::FILE* fp = std::fopen(out.c_str(), "wb");
  if (!fp) { std::perror("fopen"); return 1; }

  std::signal(SIGINT, on_sigint);
  try {
    UsrpSource src(up);
    if (!src.ref_locked())
      LOG_WARN("reference not locked — capture timing may drift");

    const double rate = src.sample_rate();
    const auto target = static_cast<std::uint64_t>(rate * secs);
    std::vector<cf32> buf(1u << 16);
    std::vector<std::int16_t> sc16;
    std::uint64_t written = 0;
    int64_t t_ns = 0;

    LOG_INFO("recording %.2f s (%llu samples) -> %s [%s]",
             secs, static_cast<unsigned long long>(target), out.c_str(), store.c_str());

    while (!g_stop.load() && written < target) {
      const std::size_t got = src.recv(buf.data(), buf.size(), t_ns);
      if (got == 0) continue;
      if (store == "sc16") {
        sc16.resize(2 * got);
        for (std::size_t i = 0; i < got; ++i) {
          sc16[2 * i]     = static_cast<std::int16_t>(std::lround(buf[i].real() * 32767.0f));
          sc16[2 * i + 1] = static_cast<std::int16_t>(std::lround(buf[i].imag() * 32767.0f));
        }
        std::fwrite(sc16.data(), sizeof(std::int16_t), sc16.size(), fp);
      } else {
        std::fwrite(buf.data(), sizeof(cf32), got, fp);
      }
      written += got;
    }
    std::fclose(fp);

    // Sidecar.
    std::FILE* meta = std::fopen((out + ".meta").c_str(), "wb");
    if (meta) {
      std::time_t now = std::time(nullptr);
      char ts[32];
      std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::gmtime(&now));
      std::fprintf(meta,
          "format = %s\nsample_rate = %.0f\ncenter_freq = %.0f\ngain_db = %.1f\n"
          "antenna = %s\nwire_format = %s\noverflows = %llu\nutc = %s\n",
          store.c_str(), rate, src.center_freq(), up.gain_db, up.antenna.c_str(),
          up.wire_format.c_str(), static_cast<unsigned long long>(src.overflow_count()), ts);
      std::fclose(meta);
    }

    LOG_INFO("done: %llu samples, %llu overflow(s)",
             static_cast<unsigned long long>(written),
             static_cast<unsigned long long>(src.overflow_count()));
    if (src.overflow_count() > 0)
      LOG_WARN("overflows occurred — lower rate, raise MTU to 9000, or store sc16");
    return 0;
  } catch (const std::exception& e) {
    std::fclose(fp);
    LOG_ERROR("iq_record: %s", e.what());
    return 1;
  }
}
