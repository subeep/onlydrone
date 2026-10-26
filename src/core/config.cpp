// droneid/src/core/config.cpp — tiny TOML-subset parser.
// GPL-3.0-or-later.
#include "droneid/config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace droneid {
namespace {

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
    return s.substr(1, s.size() - 2);
  return s;
}

}  // namespace

Config Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("config: cannot open " + path);

  Config cfg;
  std::string line;
  while (std::getline(in, line)) {
    // Strip inline comments (not inside quotes — fine for our simple files).
    const auto hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty() || line.front() == '[') continue;  // skip blanks + [sections]

    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = unquote(trim(line.substr(eq + 1)));
    if (key.empty()) continue;

    cfg.raw[key] = val;

    // Map known keys onto typed fields.
    if (key == "device_args")             cfg.device_args = val;
    else if (key == "master_clock_rate")  cfg.master_clock_rate = std::strtod(val.c_str(), nullptr);
    else if (key == "sample_rate")        cfg.sample_rate = std::strtod(val.c_str(), nullptr);
    else if (key == "center_freq")        cfg.center_freq = std::strtod(val.c_str(), nullptr);
    else if (key == "gain_db")            cfg.gain_db = std::strtod(val.c_str(), nullptr);
    else if (key == "antenna")            cfg.antenna = val;
    else if (key == "wire_format")        cfg.wire_format = val;
    else if (key == "num_channels")       cfg.num_channels = std::atoi(val.c_str());
    else if (key == "ui")                 cfg.ui = (val == "true" || val == "1");
    else if (key == "progress_file")      cfg.progress_file = val;
  }
  return cfg;
}

std::string Config::get(const std::string& key, const std::string& def) const {
  auto it = raw.find(key);
  return it == raw.end() ? def : it->second;
}

}  // namespace droneid
