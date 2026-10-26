// src/app/main.cpp — droneid application entry point.
// Phase 0: loads config and reports what it would do. The live capture pipeline,
// UsrpSource, detector/decoder and UI are wired in from Phase 1 onward.
// GPL-3.0-or-later.
#include <cstdio>
#include <cstring>
#include <string>

#include "droneid/config.h"
#include "droneid/logging.h"

using namespace droneid;

static constexpr const char* kVersion = "0.0.1-phase0";

static void usage(const char* p) {
  std::fprintf(stderr,
               "droneid %s — DJI DroneID passive receiver (passive RX only)\n"
               "usage: %s [--config PATH] [--no-ui] [--version] [--verbose]\n",
               kVersion, p);
}

int main(int argc, char** argv) {
  std::string config_path;
  bool no_ui = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") { std::printf("droneid %s\n", kVersion); return 0; }
    else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else if (a == "--no-ui") no_ui = true;
    else if (a == "--verbose") log_level() = LogLevel::Debug;
    else if (a == "--config") { if (i + 1 < argc) config_path = argv[++i]; }
    else { usage(argv[0]); return 2; }
  }

  Config cfg;
  if (!config_path.empty()) {
    try {
      cfg = Config::load(config_path);
      LOG_INFO("loaded config: %s", config_path.c_str());
    } catch (const std::exception& e) {
      LOG_ERROR("%s", e.what());
      return 1;
    }
  }
  if (no_ui) cfg.ui = false;

  LOG_INFO("droneid %s (passive receiver)", kVersion);
  LOG_INFO("device     : %s", cfg.device_args.c_str());
  LOG_INFO("sample rate: %.0f Hz  center: %.0f Hz  fmt: %s  ch: %d",
           cfg.sample_rate, cfg.center_freq, cfg.wire_format.c_str(), cfg.num_channels);
  LOG_INFO("ui         : %s", cfg.ui ? "on" : "off (headless)");
  LOG_WARN("Phase 0 scaffold: no capture/DSP yet. Use `iq_replay` to exercise the pipeline.");
  return 0;
}
