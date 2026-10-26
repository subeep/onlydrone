// tests/test_config.cpp
// GPL-3.0-or-later.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "droneid/config.h"
#include "droneid/test/microtest.h"

using droneid::Config;

namespace {
std::string tmp_path(const char* name) {
  const char* dir = std::getenv("TMPDIR");
  std::string base = dir ? dir : "/tmp";
  return base + "/droneid_cfg_" + name;
}
}  // namespace

TEST(Config, ParsesKnownKeysAndIgnoresSectionsAndComments) {
  const std::string p = tmp_path("a.toml");
  {
    std::ofstream o(p);
    o << "# narrowband 1GbE profile\n"
      << "[radio]\n"
      << "device_args = \"addr=192.168.10.2\"\n"
      << "sample_rate = 15360000\n"
      << "center_freq = 2.4595e9\n"
      << "wire_format = \"sc16\"   # comment after value\n"
      << "num_channels = 1\n"
      << "[ui]\n"
      << "ui = false\n"
      << "progress_file = progress.md\n";
  }
  Config c = Config::load(p);
  EXPECT_EQ(c.device_args, std::string("addr=192.168.10.2"));
  EXPECT_NEAR(c.sample_rate, 15.36e6, 1.0);
  EXPECT_NEAR(c.center_freq, 2.4595e9, 1.0);
  EXPECT_EQ(c.wire_format, std::string("sc16"));
  EXPECT_EQ(c.num_channels, 1);
  EXPECT_FALSE(c.ui);
  EXPECT_EQ(c.progress_file, std::string("progress.md"));
  EXPECT_EQ(c.get("sample_rate"), std::string("15360000"));
  std::remove(p.c_str());
}

TEST(Config, MissingFileThrows) {
  bool threw = false;
  try {
    Config::load("/no/such/droneid/config.toml");
  } catch (const std::exception&) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

MICROTEST_MAIN
