// tests/test_dji.cpp — DJI decoder validation (turbo round-trip, scrambler, end-to-end).
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <vector>

#include "droneid/dji.h"
#include "droneid/test/microtest.h"

using namespace droneid;

TEST(DjiTurbo, RoundTrip) {
  std::vector<uint8_t> user(91, 0);
  for (int i = 0; i < 91; ++i) user[i] = static_cast<uint8_t>((i * 7 + 3) & 0xff);
  auto frame = dji::build_frame176(user);
  EXPECT_EQ(frame.size(), static_cast<size_t>(176));

  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  EXPECT_EQ(bits.size(), static_cast<size_t>(7200));

  std::vector<uint8_t> out;
  const bool crc = dji::turbo_decode_frame(bits, out);
  EXPECT_TRUE(crc);
  bool same = (out.size() == frame.size());
  for (size_t i = 0; same && i < frame.size(); ++i) if (out[i] != frame[i]) same = false;
  EXPECT_TRUE(same);
}

TEST(DjiScrambler, DeterministicAndBinary) {
  auto s = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  EXPECT_EQ(s.size(), static_cast<size_t>(7200));
  int ones = 0;
  for (auto b : s) { EXPECT_TRUE(b <= 1); ones += b; }
  EXPECT_TRUE(ones > 2000 && ones < 5200);  // roughly balanced
}

TEST(DjiEndToEnd, SynthesizeThenDecode) {
  std::vector<uint8_t> user(91, 0);
  const char* serial = "ENDTOENDTEST01";
  for (int i = 0; serial[i]; ++i) user[7 + i] = static_cast<uint8_t>(serial[i]);
  user[0] = 88;
  user[1] = 0x10;
  user[2] = 2;
  auto frame = dji::build_frame176(user);

  std::vector<uint8_t> bits;
  dji::turbo_encode_frame(frame, bits);
  auto scr = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (int i = 0; i < 7200; ++i) bits[i] ^= scr[i];

  auto burst = dji::synthesize_burst(bits, 15.36e6);
  std::vector<cf32> stream(4096, cf32(0, 0));
  stream.insert(stream.end(), burst.begin(), burst.end());
  stream.resize(stream.size() + 4096, cf32(0, 0));

  dji::DecodeOptions opt;
  opt.sample_rate = 15.36e6;
  opt.corr_threshold = 0.3f;
  opt.low_pass = false;
  auto res = dji::decode_samples(stream, opt);

  bool found = false;
  for (auto& b : res)
    if (b.crc_ok && b.payload == frame) found = true;
  EXPECT_TRUE(found);
}

TEST(DjiEndToEnd, IntegerCarrierOffset) {
  std::vector<uint8_t> user(91);
  for (size_t byte = 0; byte < user.size(); ++byte)
    user[byte] = static_cast<uint8_t>(byte * 13 + 7);
  const auto frame = dji::build_frame176(user);
  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  const auto scrambler = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (size_t bit = 0; bit < bits.size(); ++bit) bits[bit] ^= scrambler[bit];
  const auto burst = dji::synthesize_burst(bits, 15.36e6);

  for (double offset : {-57470.0, 62530.0}) {
    for (bool low_pass : {false, true}) {
      std::vector<cf32> stream(4096, cf32(0, 0));
      for (size_t sample = 0; sample < burst.size(); ++sample) {
        const double phase = 2 * std::acos(-1.0) * offset * sample / 15.36e6;
        stream.push_back(burst[sample] * std::polar(1.0f, static_cast<float>(phase)));
      }
      stream.resize(stream.size() + 4096, cf32(0, 0));
      dji::DecodeOptions options;
      options.low_pass = low_pass;
      const auto decoded = dji::decode_samples(stream, options);
      bool found = false;
      for (const auto& result : decoded) {
        if (result.crc_ok && result.payload == frame) {
          found = true;
          EXPECT_NEAR(result.cfo_hz, offset, 100.0);
        }
      }
      EXPECT_TRUE(found);
    }
  }
}

TEST(DjiEndToEnd, RealMini2Capture) {
  const std::string path = std::string(DRONEID_TEST_DATA_DIR) + "/mini2_centered.sc16";
  dji::DecodeOptions options;
  const auto decoded = dji::decode_file(path, SampleFormat::sc16, options);
  EXPECT_EQ(decoded.size(), size_t(1));
  if (decoded.size() != 1) return;
  EXPECT_TRUE(decoded[0].crc_ok);
  EXPECT_NEAR(decoded[0].cfo_hz, -57470, 100);
  const std::string expected_hex =
      "5810021203131f5379735365635761734865726520000000000000000000000000000000000000000067255429bad57f0100001e038900195a130000000000000000003f00000000000000000000000000000000000000000057f5000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d47823eb1cbb5444fd8e17117cf818ff920a0da56c06721c83596d5c464239c5f36d";
  std::string actual_hex;
  for (uint8_t byte : decoded[0].payload) {
    char encoded[3];
    std::snprintf(encoded, sizeof(encoded), "%02x", byte);
    actual_hex += encoded;
  }
  EXPECT_EQ(actual_hex, expected_hex);
  dji::Telemetry telemetry;
  EXPECT_TRUE(dji::parse_frame(decoded[0].payload, telemetry));
  EXPECT_EQ(telemetry.version, 2);
  EXPECT_EQ(telemetry.sequence, 0x0312);
  EXPECT_EQ(telemetry.serial, std::string("SysSecWasHere "));
  EXPECT_NEAR(telemetry.drone_lat, 0, 1e-6);
  EXPECT_NEAR(telemetry.drone_lon, 0, 1e-6);
  EXPECT_NEAR(telemetry.pilot_lat, 51.447176, 1e-4);
  EXPECT_NEAR(telemetry.pilot_lon, 7.266528, 1e-4);
  EXPECT_TRUE(telemetry.pilot_valid);
  EXPECT_FALSE(telemetry.home_valid);
  EXPECT_EQ(telemetry.gps_time_ms, uint64_t(1648558221652));
  EXPECT_EQ(telemetry.type_name, std::string("Mini 2"));
}

TEST(DjiFile, ChunkBoundaryAndSampleLimit) {
  const auto frame = dji::build_frame176(std::vector<uint8_t>(91, 0x35));
  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  const auto scrambler = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (size_t bit = 0; bit < bits.size(); ++bit) bits[bit] ^= scrambler[bit];
  const auto burst = dji::synthesize_burst(bits, 15.36e6);
  const size_t chunk = 4u << 20;
  std::vector<cf32> stream(chunk + 40000, cf32(0, 0));
  for (size_t start : {size_t(4096), chunk + 100, chunk + 20000})
    std::copy(burst.begin(), burst.end(), stream.begin() + start);
  const auto path = (std::filesystem::temp_directory_path() / "droneid_test_chunk.cf32").string();
  std::FILE* output = std::fopen(path.c_str(), "wb");
  EXPECT_TRUE(output != nullptr);
  if (!output) return;
  EXPECT_EQ(std::fwrite(stream.data(), sizeof(cf32), stream.size(), output), stream.size());
  std::fclose(output);
  for (size_t limit : {size_t(0), stream.size(), chunk + 16000}) {
    dji::DecodeOptions options;
    options.low_pass = false;
    options.max_samples = limit;
    const auto decoded = dji::decode_file(path, SampleFormat::cf32, options);
    EXPECT_EQ(decoded.size(), limit == chunk + 16000 ? size_t(2) : size_t(3));
    int64_t previous_index = -1;
    for (const auto& result : decoded) {
      EXPECT_TRUE(result.crc_ok);
      EXPECT_TRUE(result.payload == frame);
      EXPECT_TRUE(result.sample_index > previous_index);
      previous_index = result.sample_index;
    }
  }
  std::remove(path.c_str());
}

TEST(DjiFrame, RejectsTruncatedAndUnsupportedRecords) {
  dji::Telemetry telemetry;
  telemetry.serial = "stale";
  telemetry.pilot_valid = true;
  std::vector<uint8_t> payload(90, 0);
  payload[2] = 2;
  EXPECT_FALSE(dji::parse_frame(payload, telemetry));
  EXPECT_TRUE(telemetry.serial.empty());
  EXPECT_FALSE(telemetry.pilot_valid);
  payload.resize(176);
  payload[2] = 0xff;
  EXPECT_FALSE(dji::parse_frame(payload, telemetry));
  payload[2] = 2;
  payload[7] = 1;
  EXPECT_FALSE(dji::parse_frame(payload, telemetry));
}

MICROTEST_MAIN
