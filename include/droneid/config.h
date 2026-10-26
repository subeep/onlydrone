// droneid/config.h — runtime configuration, loaded from a small TOML-subset file.
// GPL-3.0-or-later.
#pragma once

#include <map>
#include <string>

namespace droneid {

// Configuration for the receiver. Defaults target the verified X310 (serial 326CF02)
// in 1GbE narrowband single-antenna mode (see DRONEID_BUILD_PLAN.md).
struct Config {
  // Radio (used from Phase 1).
  std::string device_args      = "addr=192.168.10.2";
  double      master_clock_rate = 184.32e6;   // 12 x 15.36 MSPS -> integer DDC decim
  double      sample_rate       = 15.36e6;
  double      center_freq       = 2.4595e9;
  double      gain_db           = 20.0;
  std::string antenna           = "RX2";
  std::string wire_format       = "sc16";     // sc16 | sc8
  int         num_channels      = 1;          // 1 = single antenna; 2 = direction finding

  // UI / app.
  bool        ui                = true;
  std::string progress_file     = "progress.md";

  // Everything parsed, for forward compatibility with keys not yet modeled above.
  std::map<std::string, std::string> raw;

  // Parse a file of `key = value` lines. Lines starting with '#' and `[section]`
  // headers are ignored; values may be quoted. Missing file throws std::runtime_error.
  static Config load(const std::string& path);

  // Look up a raw key (post-load), with a default.
  std::string get(const std::string& key, const std::string& def = "") const;
};

}  // namespace droneid
