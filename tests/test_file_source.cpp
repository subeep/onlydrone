// tests/test_file_source.cpp
// GPL-3.0-or-later.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "droneid/file_source.h"
#include "droneid/test/microtest.h"

using namespace droneid;

namespace {
std::string tmp_path(const char* name) {
  const char* dir = std::getenv("TMPDIR");
  std::string base = dir ? dir : "/tmp";
  return base + "/droneid_test_" + name;
}

void write_cf32(const std::string& path, const std::vector<cf32>& s) {
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  std::fwrite(s.data(), sizeof(cf32), s.size(), fp);
  std::fclose(fp);
}

void write_sc16(const std::string& path, const std::vector<cf32>& s) {
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  std::vector<std::int16_t> o(2 * s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    o[2 * i]     = static_cast<std::int16_t>(s[i].real() * 32767.0f);
    o[2 * i + 1] = static_cast<std::int16_t>(s[i].imag() * 32767.0f);
  }
  std::fwrite(o.data(), sizeof(std::int16_t), o.size(), fp);
  std::fclose(fp);
}
}  // namespace

TEST(FileSource, ReadsCf32Exactly) {
  const std::string p = tmp_path("cf32.iq");
  std::vector<cf32> in;
  for (int i = 0; i < 1000; ++i) in.emplace_back(0.01f * i, -0.01f * i);
  write_cf32(p, in);

  FileSource src(p, SampleFormat::cf32, 15.36e6);
  std::vector<cf32> out(4096);
  int64_t t = 0;
  std::size_t total = 0;
  for (;;) {
    std::size_t got = src.recv(out.data(), out.size(), t);
    if (got == 0) break;
    for (std::size_t i = 0; i < got; ++i) {
      EXPECT_NEAR(out[i].real(), in[total + i].real(), 1e-6);
      EXPECT_NEAR(out[i].imag(), in[total + i].imag(), 1e-6);
    }
    total += got;
  }
  EXPECT_EQ(total, in.size());
  EXPECT_TRUE(src.eof());
  std::remove(p.c_str());
}

TEST(FileSource, ReadsSc16WithinTolerance) {
  const std::string p = tmp_path("sc16.iq");
  std::vector<cf32> in;
  for (int i = 0; i < 500; ++i) in.emplace_back(0.5f, -0.25f);
  write_sc16(p, in);

  FileSource src(p, SampleFormat::sc16, 15.36e6);
  std::vector<cf32> out(1024);
  int64_t t = 0;
  std::size_t got = src.recv(out.data(), out.size(), t);
  EXPECT_EQ(got, static_cast<std::size_t>(500));
  // int16 quantization: within one LSB (~3e-5).
  EXPECT_NEAR(out[0].real(), 0.5f, 1e-4);
  EXPECT_NEAR(out[0].imag(), -0.25f, 1e-4);
  std::remove(p.c_str());
}

TEST(FileSource, PartialLastBlockCountsSamplesOnly) {
  // 7 cf32 samples; request blocks of 4 -> 4 then 3.
  const std::string p = tmp_path("partial.iq");
  std::vector<cf32> in(7, cf32(1.0f, 1.0f));
  write_cf32(p, in);

  FileSource src(p, SampleFormat::cf32, 1.0);
  std::vector<cf32> out(4);
  int64_t t = 0;
  EXPECT_EQ(src.recv(out.data(), 4, t), static_cast<std::size_t>(4));
  EXPECT_EQ(src.recv(out.data(), 4, t), static_cast<std::size_t>(3));
  EXPECT_EQ(src.recv(out.data(), 4, t), static_cast<std::size_t>(0));
  EXPECT_EQ(src.samples_read(), static_cast<std::uint64_t>(7));
  std::remove(p.c_str());
}

MICROTEST_MAIN
