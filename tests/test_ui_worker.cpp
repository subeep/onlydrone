#include "receiver_worker.h"
#include "droneid/dji.h"
#include "droneid/test/microtest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace droneid;

TEST(UiWorker, ReplayMatchesBackendAndTrackCount) {
  ui::UiModel model;
  ui::SourceConfig config;
  config.replay = std::string(DRONEID_TEST_DATA_DIR) + "/mini2_wideband.cf32";
  config.fmt = SampleFormat::cf32;
  config.rate = 50e6;
  config.auto_center = true;
  std::atomic<bool> stop{false};
  ui::replay_worker(model, config, stop);
  EXPECT_TRUE(model.status().state != ui::UsrpStatus::State::Error);
  EXPECT_EQ(model.packets().size(), std::size_t(1));
  EXPECT_EQ(model.tracks().size(), std::size_t(1));
  if (!model.tracks().empty()) {
    EXPECT_EQ(model.tracks()[0].count, uint64_t(1));
    EXPECT_EQ(model.tracks()[0].serial, std::string("SysSecWasHere "));
  }
  EXPECT_EQ(model.status().rate_hz, 50e6);
  EXPECT_EQ(model.status().samples_received, uint64_t(52084));
  EXPECT_TRUE(model.status().state == ui::UsrpStatus::State::Disconnected);
  ui::replay_worker(model, config, stop);
  EXPECT_EQ(model.packets().size(), std::size_t(2));
  EXPECT_EQ(model.tracks()[0].count, uint64_t(2));
}

TEST(UiWorker, WindowBoundaryHasNeitherLossNorDuplicates) {
  const std::size_t stride = 65536;
  const auto frame = dji::build_frame176(std::vector<uint8_t>(91, 0x35));
  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  const auto scrambler = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (std::size_t index = 0; index < bits.size(); ++index) bits[index] ^= scrambler[index];
  const auto burst = dji::synthesize_burst(bits, 15.36e6);
  std::vector<cf32> samples(2 * stride + 16000);
  for (const auto start : {stride - 5000, 2 * stride - 5000})
    std::copy(burst.begin(), burst.end(), samples.begin() + start);
  ui::WindowAssembler assembler(stride, 12000);
  std::size_t valid = 0;
  for (std::size_t start = 0; start < samples.size(); start += 7777) {
    const auto count = std::min<std::size_t>(7777, samples.size() - start);
    for (const auto& window : assembler.feed(samples.data() + start, count, start + count == samples.size())) {
      for (const auto& result : dji::decode_samples(window.samples, {})) {
        if (result.crc_ok && static_cast<std::size_t>(result.sample_index) < window.accept_before) {
          EXPECT_TRUE(result.payload == frame);
          ++valid;
        }
      }
    }
  }
  EXPECT_EQ(valid, std::size_t(2));
}

TEST(UiWorker, ResetDropsPartialWindow) {
  ui::WindowAssembler assembler(65536, 12000);
  const std::vector<cf32> samples(20000, cf32(1, 0));
  EXPECT_TRUE(assembler.feed(samples.data(), samples.size()).empty());
  assembler.reset();
  EXPECT_TRUE(assembler.feed(nullptr, 0, true).empty());
}

TEST(UiWorker, StopAndMissingFileAreHandled) {
  ui::UiModel model;
  ui::SourceConfig config;
  config.replay = "/tmp/nonexistent-punjdrone-UI-fixture";
  std::atomic<bool> stop{false};
  ui::replay_worker(model, config, stop);
  EXPECT_TRUE(model.status().state == ui::UsrpStatus::State::Error);
  config.replay = std::string(DRONEID_TEST_DATA_DIR) + "/mini2_centered.sc16";
  stop.store(true);
  ui::replay_worker(model, config, stop);
  EXPECT_TRUE(model.packets().empty());
  EXPECT_EQ(model.status().samples_received, uint64_t(0));
}

TEST(UiWorker, DfRejectsCrossSessionCalibrationWithoutOpeningRadio) {
  ui::UiModel model;
  ui::SourceConfig config;
  config.device = "unreachable-test-device";
  config.df_cal = "/tmp/nonexistent-calibration";
  std::atomic<bool> stop{false};
  ui::df_live_worker(model, config, stop);
  EXPECT_TRUE(model.status().state == ui::UsrpStatus::State::Error);
  EXPECT_EQ(model.status().samples_received, uint64_t(0));
}

TEST(UiWorker, UnsupportedCrcPayloadIsLoggedWithoutTelemetry) {
  std::vector<uint8_t> payload(91, 0x49);
  const auto frame = dji::build_frame176(payload);
  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  const auto scrambler = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (std::size_t index = 0; index < bits.size(); ++index) bits[index] ^= scrambler[index];
  const auto burst = dji::synthesize_burst(bits, 15.36e6);
  std::vector<cf32> samples(30000);
  std::copy(burst.begin(), burst.end(), samples.begin() + 4000);
  const auto path = std::filesystem::temp_directory_path() / "punjdrone-ui-unsupported-test.cf32";
  {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(cf32));
  }
  ui::UiModel model;
  ui::SourceConfig config;
  config.replay = path.string();
  config.fmt = SampleFormat::cf32;
  std::atomic<bool> stop{false};
  ui::replay_worker(model, config, stop);
  std::filesystem::remove(path);
  EXPECT_EQ(model.packets().size(), std::size_t(1));
  EXPECT_TRUE(model.tracks().empty());
  if (!model.packets().empty()) {
    EXPECT_FALSE(model.packets()[0].telemetry_parsed);
    EXPECT_EQ(model.packets()[0].payload_hex.size(), std::size_t(352));
    EXPECT_TRUE(model.packets()[0].serial.empty());
  }
}

MICROTEST_MAIN
