#pragma once

#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

struct ReceiverArgs {
  std::map<std::string, std::string> values;
  std::vector<std::string> files;

  ReceiverArgs(int argc, char** argv, const std::set<std::string>& options,
               const std::set<std::string>& flags = {}) {
    for (int index = 1; index < argc; ++index) {
      const std::string key = argv[index];
      if (flags.count(key)) {
        if (!values.emplace(key, "true").second) throw std::invalid_argument("duplicate " + key);
      } else if (options.count(key)) {
        if (++index >= argc) throw std::invalid_argument("missing value for " + key);
        if (!values.emplace(key, argv[index]).second) throw std::invalid_argument("duplicate " + key);
      } else if (key.rfind("--", 0) == 0) throw std::invalid_argument("unknown option " + key);
      else files.push_back(key);
    }
  }

  bool has(const std::string& key) const { return values.count(key) != 0; }
  std::string get(const std::string& key, const std::string& fallback = "") const {
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
  }
  double number(const std::string& key, double fallback) const {
    if (!has(key)) return fallback;
    const auto value = get(key);
    std::size_t used = 0;
    const double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result)) throw std::invalid_argument("invalid " + key);
    return result;
  }
};
