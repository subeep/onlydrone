// tests/test_dji.cpp — DJI decoder validation (turbo round-trip, scrambler, end-to-end).
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <vector>

#include "droneid/dji.h"
#include "droneid/file_source.h"
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
  const uint16_t checksum = dji::packet_crc16(user.data(), user.size() - 2);
  user[user.size() - 2] = checksum & 0xff;
  user[user.size() - 1] = checksum >> 8;
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
  dji::Telemetry telemetry;
  EXPECT_TRUE(dji::parse_frame(frame, telemetry));
  EXPECT_EQ(telemetry.serial, std::string(serial));
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

  for (double offset : {-485940.0, -57470.0, 62530.0, 485940.0}) {
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
      if (!found) std::fprintf(stderr, "offset %.0f Hz, low_pass %d: no matching payload\n", offset, low_pass);
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
  const auto envelope = dji::inspect_packet(decoded[0].payload);
  EXPECT_EQ(envelope.format, std::string("v2"));
  EXPECT_EQ(envelope.logical_bytes, size_t(91));
  EXPECT_TRUE(envelope.crc16_ok);
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

TEST(DjiEndToEnd, RawWidebandMini2) {
  const std::string directory = DRONEID_TEST_DATA_DIR;
  const auto expected = dji::decode_file(directory + "/mini2_centered.sc16", SampleFormat::sc16, {});
  EXPECT_EQ(expected.size(), size_t(1));
  if (expected.size() != 1) return;
  const std::string path = directory + "/mini2_wideband.cf32";
  FileSource source(path, SampleFormat::cf32, 50e6);
  std::vector<cf32> samples(60000);
  int64_t timestamp = 0;
  samples.resize(source.recv(samples.data(), samples.size(), timestamp));
  for (bool automatic : {false, true}) {
    dji::DecodeOptions options;
    options.sample_rate = 50e6;
    options.auto_center = automatic;
    options.freq_offset = automatic ? 0 : -9.6e6;
    const auto decoded_file = dji::decode_file(path, SampleFormat::cf32, options);
    const auto decoded_memory = dji::decode_samples(samples, options);
    EXPECT_EQ(decoded_file.size(), size_t(1));
    EXPECT_EQ(decoded_memory.size(), size_t(1));
    if (decoded_file.size() != 1 || decoded_memory.size() != 1) continue;
    EXPECT_TRUE(decoded_file[0].crc_ok);
    EXPECT_TRUE(decoded_file[0].payload == expected[0].payload);
    EXPECT_TRUE(decoded_memory[0].payload == expected[0].payload);
    EXPECT_EQ(decoded_memory[0].sample_index, decoded_file[0].sample_index);
    EXPECT_NEAR(decoded_file[0].sample_index, 7760, 150);
    options.max_samples = 5000;
    EXPECT_TRUE(dji::decode_file(path, SampleFormat::cf32, options).empty());
    EXPECT_TRUE(dji::decode_samples(samples, options).empty());
  }
}

TEST(DjiEndToEnd, NativeWidebandSynthetic) {
  const auto frame = dji::build_frame176(std::vector<uint8_t>(91, 0x53));
  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  const auto scrambler = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (size_t bit = 0; bit < bits.size(); ++bit) bits[bit] ^= scrambler[bit];
  for (double rate : {61.44e6, 92.16e6}) {
    const auto burst = dji::synthesize_burst(bits, rate);
    std::vector<cf32> samples(16384, cf32(0, 0));
    samples.insert(samples.end(), burst.begin(), burst.end());
    samples.resize(samples.size() + 16384);
    for (size_t sample = 0; sample < samples.size(); ++sample)
      samples[sample] *= std::polar(1.0f, static_cast<float>(2 * std::acos(-1.0) * 12e6 * sample / rate));
    dji::DecodeOptions options;
    options.sample_rate = rate;
    options.freq_offset = -12e6;
    const auto decoded = dji::decode_samples(samples, options);
    EXPECT_EQ(decoded.size(), size_t(1));
    if (decoded.size() != 1) continue;
    EXPECT_TRUE(decoded[0].crc_ok);
    EXPECT_TRUE(decoded[0].payload == frame);
    EXPECT_NEAR(decoded[0].sample_index, 16384 - 64 * rate / 15.36e6, 10);
  }
}

TEST(DjiFile, WidebandStreamingMatchesMemory) {
  FileSource source(std::string(DRONEID_TEST_DATA_DIR) + "/mini2_wideband.cf32", SampleFormat::cf32, 50e6);
  std::vector<cf32> capture(60000);
  int64_t timestamp = 0;
  capture.resize(source.recv(capture.data(), capture.size(), timestamp));
  std::vector<cf32> input(230000, cf32(0, 0));
  for (size_t offset : {size_t(65500), size_t(150007)})
    std::copy(capture.begin(), capture.end(), input.begin() + offset);
  const auto path = (std::filesystem::temp_directory_path() / "droneid_test_wideband.cf32").string();
  std::FILE* output = std::fopen(path.c_str(), "wb");
  EXPECT_TRUE(output != nullptr);
  if (!output) return;
  EXPECT_EQ(std::fwrite(input.data(), sizeof(cf32), input.size(), output), input.size());
  std::fclose(output);
  for (size_t limit : {size_t(0), size_t(140000)}) {
    dji::DecodeOptions options;
    options.sample_rate = 50e6;
    options.freq_offset = -9.6e6;
    options.max_samples = limit;
    const auto file_results = dji::decode_file(path, SampleFormat::cf32, options);
    const auto memory_results = dji::decode_samples(input, options);
    EXPECT_EQ(file_results.size(), limit ? size_t(1) : size_t(2));
    EXPECT_EQ(file_results.size(), memory_results.size());
    if (file_results.size() != memory_results.size()) continue;
    for (size_t index = 0; index < file_results.size(); ++index) {
      EXPECT_TRUE(file_results[index].crc_ok);
      EXPECT_TRUE(file_results[index].payload == memory_results[index].payload);
      EXPECT_EQ(file_results[index].sample_index, memory_results[index].sample_index);
    }
  }
  std::remove(path.c_str());
}

namespace {
std::vector<uint8_t> packet_with_inner_crc(size_t length, const std::vector<uint8_t>& header) {
  std::vector<uint8_t> payload(176);
  std::copy(header.begin(), header.end(), payload.begin());
  payload[0] = static_cast<uint8_t>(length - 3);
  const std::string serial = "TESTONLY1234";
  std::copy(serial.begin(), serial.end(), payload.begin() + 7);
  uint16_t checksum = 0x3692;
  for (size_t index = 0; index < length - 2; ++index) {
    checksum ^= payload[index];
    for (int bit = 0; bit < 8; ++bit)
      checksum = (checksum >> 1) ^ ((checksum & 1) ? 0x8408 : 0);
  }
  payload[length - 2] = checksum & 0xff;
  payload[length - 1] = checksum >> 8;
  return payload;
}
}

TEST(DjiEnvelope, VerifiedLayoutsAndCorruption) {
  for (size_t length : {size_t(79), size_t(91)}) {
    auto payload = packet_with_inner_crc(length, {0, 0x10, 2});
    dji::Telemetry telemetry;
    EXPECT_TRUE(dji::inspect_packet(payload).crc16_ok);
    EXPECT_TRUE(dji::parse_frame(payload, telemetry));
    EXPECT_EQ(telemetry.serial, std::string("TESTONLY1234"));
    payload[length + 1] ^= 1;
    EXPECT_TRUE(dji::inspect_packet(payload).crc16_ok);
    payload[7] ^= 1;
    EXPECT_FALSE(dji::inspect_packet(payload).crc16_ok);
    EXPECT_FALSE(dji::parse_frame(payload, telemetry));
    EXPECT_TRUE(telemetry.serial.empty());
  }
  for (const auto& header : std::vector<std::vector<uint8_t>>{
           {0, 0x10, 3}, {0, 0x10, 'I', 'N', 'F', 'P'},
           {0, 0x13, 'C', 'R', 'Y', 'P'}, {0, 0x11, 2}, {0, 0x20, 2}}) {
    const auto payload = packet_with_inner_crc(131, header);
    EXPECT_TRUE(dji::inspect_packet(payload).crc16_ok);
    dji::Telemetry telemetry;
    EXPECT_FALSE(dji::parse_frame(payload, telemetry));
    EXPECT_TRUE(telemetry.serial.empty());
  }
  EXPECT_EQ(dji::inspect_packet(packet_with_inner_crc(112, {0, 0x10, 3})).format, std::string("v3"));
  EXPECT_EQ(dji::inspect_packet(packet_with_inner_crc(131, {0, 0x10, 'I', 'N', 'F', 'P'})).format, std::string("infp"));
  EXPECT_EQ(dji::inspect_packet(packet_with_inner_crc(138, {0, 0x10, 'I', 'N', 'F', 'P'})).format, std::string("infp"));
  EXPECT_EQ(dji::inspect_packet(packet_with_inner_crc(144, {0, 0x11, 2})).format, std::string("auxiliary_11"));
  dji::Telemetry telemetry;
  EXPECT_FALSE(dji::parse_frame(packet_with_inner_crc(78, {0, 0x10, 2}), telemetry));
}

TEST(DjiEnvelope, InvalidLengths) {
  for (const auto& payload : std::vector<std::vector<uint8_t>>{
           {}, {88}, {0, 0x10, 2}, std::vector<uint8_t>(176, 0xff), {88, 0x10, 2, 0, 0}}) {
    const auto envelope = dji::inspect_packet(payload);
    EXPECT_FALSE(envelope.length_valid);
    EXPECT_FALSE(envelope.crc16_ok);
  }
  auto payload = packet_with_inner_crc(91, {0, 0x10, 2});
  payload.resize(90);
  EXPECT_FALSE(dji::inspect_packet(payload).length_valid);
  payload.assign(176, 0);
  payload[0] = 171;
  EXPECT_FALSE(dji::inspect_packet(payload).length_valid);
}

MICROTEST_MAIN
