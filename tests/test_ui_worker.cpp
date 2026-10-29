#include "receiver_worker.h"
#include "droneid/aoa.h"
#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/test/microtest.h"
#include "droneid/o4_detector.h"
#include "droneid/odid_wifi.h"
#include "o4_fixture.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace droneid;

TEST(UiWorker, SlowRfQueueDoesNotDropPacketDecoderInput) {
  ui::LiveWindowFanout queue(2);
  ui::SampleWindow window; window.samples={{1,2}};window.start_sample=0;
  queue.push(window);
  auto packet0=queue.pop(0);
  EXPECT_EQ(packet0->start_sample,uint64_t(0));
  window.start_sample=100;queue.push(window);
  auto packet1=queue.pop(0);
  window.start_sample=200;queue.push(window);
  EXPECT_EQ(queue.dropped(0),uint64_t(0));
  EXPECT_EQ(queue.dropped(1),uint64_t(1));
  auto rf1=queue.pop(1);
  EXPECT_EQ(rf1.get(),packet1.get()); // shared immutable samples; no second IQ copy
  EXPECT_EQ(rf1->start_sample,uint64_t(100));
  EXPECT_EQ(queue.pop(0)->start_sample,uint64_t(200));
  EXPECT_TRUE(queue.empty(0));
  EXPECT_EQ(queue.pop(1)->start_sample,uint64_t(200));
  EXPECT_TRUE(queue.empty(1));
  EXPECT_FALSE(bool(queue.pop(0)));
}

TEST(UiWorker, BothLiveQueuesBoundedAndDroppedRangesRemainVisible) {
  ui::LiveWindowFanout queue(1);
  ui::SampleWindow first; first.start_sample=123;queue.push(first);
  ui::SampleWindow second; second.start_sample=456;queue.push(second);
  EXPECT_EQ(queue.dropped(0),uint64_t(1));
  EXPECT_EQ(queue.dropped(1),uint64_t(1));
  EXPECT_EQ(queue.pop(0)->start_sample,uint64_t(456));
  EXPECT_EQ(queue.pop(1)->start_sample,uint64_t(456));
}

TEST(UiWorker, O4EvidenceMatchesStreamingWithoutInventingTrack) {
  const auto samples=structured_reference(15.36e6);
  const auto path=(std::filesystem::temp_directory_path()/"punjdrone_o4_worker.cf32").string();
  {
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(samples.data()),samples.size()*sizeof(cf32));
  }
  O4CandidateDetector detector(15.36e6);
  uint64_t candidates=0, structures=0;
  const auto events=detector.feed(samples.data(),samples.size(),0);
  for (const auto& e:events) { candidates+=e.candidate; structures+=e.structure_match; }
  ui::SourceConfig config;
  config.replay=path;
  config.fmt=SampleFormat::cf32;
  config.rate=15.36e6;
  ui::UiModel model;
  std::atomic<bool> stop{false};
  ui::replay_worker(model,config,stop);
  EXPECT_EQ(model.status().state,ui::UsrpStatus::State::Disconnected);
  EXPECT_EQ(model.rf_summary().o4_windows,events.size());
  EXPECT_EQ(model.rf_summary().o4_candidate_windows,candidates);
  EXPECT_EQ(model.rf_summary().o4_structure_windows,structures);
  EXPECT_TRUE(candidates>0);
  EXPECT_TRUE(model.tracks().empty());
  auto connecting=model.status();
  connecting.state=ui::UsrpStatus::State::Connecting;
  model.set_status(connecting);
  EXPECT_EQ(model.rf_summary().o4_windows,uint64_t(0));
  std::filesystem::remove(path);
}

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

TEST(UiWorker, PairedQueueDropsWholePairNeverSingleChannel) {
  ui::BoundedPairedQueue queue(2);
  for (uint64_t idx = 0; idx < 3; ++idx) {
    ui::PairedSampleWindow win;
    win.ch0.assign(64, cf32(static_cast<float>(idx + 1), 0));
    win.ch1.assign(64, cf32(0, static_cast<float>(idx + 1)));
    win.start_sample = idx * 64;
    queue.push(std::move(win));
  }
  EXPECT_EQ(queue.dropped_pairs(), uint64_t(1));
  ui::PairedSampleWindow first;
  EXPECT_TRUE(queue.pop(first));
  EXPECT_EQ(first.start_sample, uint64_t(64));
  EXPECT_EQ(first.ch0.size(), first.ch1.size());
  EXPECT_TRUE(first.after_drop_or_gap);
}

TEST(UiWorker, PairedReplayAssociatesDecodedAndUnknownRfBearingsAndRejectsMalformedPairs) {
  const double pi = std::acos(-1.0);
  const double rate = 15.36e6;
  const double center_hz = 2459.5e6;
  const double spacing_m = 0.05;
  const double bias_rad = 0.72;
  const double drone_angle_deg = -28.0;
  const double rf_angle_deg = 35.0;

  // Read real Mini 2 centered fixture.
  FileSource real_src(std::string(DRONEID_TEST_DATA_DIR) + "/mini2_centered.sc16",
                      SampleFormat::sc16, rate, center_hz);
  std::vector<cf32> mini2(16384);
  int64_t ts = 0;
  mini2.resize(real_src.recv(mini2.data(), mini2.size(), ts));
  EXPECT_TRUE(mini2.size() > 10000);

  // Build paired stream: [4096 splitter cal] + [real Mini 2 @ -28 deg] + [quiet + unknown RF @ +35 deg].
  const std::size_t cal_samples = 4096;
  const std::size_t tail_samples = 8192;
  std::vector<cf32> ch0(cal_samples + mini2.size() + tail_samples, cf32(0, 0));
  std::vector<cf32> ch1(ch0.size(), cf32(0, 0));

  for (std::size_t i = 0; i < cal_samples; ++i) {
    ch0[i] = std::polar(0.35f, static_cast<float>(0.019 * i));
    ch1[i] = ch0[i] * std::polar(1.0f, static_cast<float>(bias_rad));
  }

  const double drone_phase =
      bias_rad + 2.0 * pi * spacing_m * (center_hz / 299792458.0) * std::sin(drone_angle_deg * pi / 180.0);
  for (std::size_t i = 0; i < mini2.size(); ++i) {
    ch0[cal_samples + i] = mini2[i];
    ch1[cal_samples + i] = mini2[i] * std::polar(1.0f, static_cast<float>(drone_phase));
  }

  const double rf_phase =
      bias_rad + 2.0 * pi * spacing_m * (center_hz / 299792458.0) * std::sin(rf_angle_deg * pi / 180.0);
  const std::size_t rf_start = cal_samples + mini2.size() + 2048;
  for (std::size_t i = 0; i < 2048; ++i) {
    const cf32 s = std::polar(0.3f, static_cast<float>(0.031 * i));
    ch0[rf_start + i] = s;
    ch1[rf_start + i] = s * std::polar(1.0f, static_cast<float>(rf_phase));
  }

  const auto dir = std::filesystem::temp_directory_path();
  const auto ch0_path = (dir / "punjdrone_paired_test.ch0.cf32").string();
  const auto ch1_path = (dir / "punjdrone_paired_test.ch1.cf32").string();
  const auto manifest_path = (dir / "punjdrone_paired_test.pair.toml").string();
  const auto jsonl_path = (dir / "punjdrone_paired_test.df.jsonl").string();

  {
    std::ofstream f0(ch0_path, std::ios::binary);
    std::ofstream f1(ch1_path, std::ios::binary);
    f0.write(reinterpret_cast<const char*>(ch0.data()), ch0.size() * sizeof(cf32));
    f1.write(reinterpret_cast<const char*>(ch1.data()), ch1.size() * sizeof(cf32));
  }

  PairManifest manifest;
  manifest.complete = true;
  manifest.status_reason = "ok";
  manifest.session_id = "sess_paired_worker";
  manifest.config_epoch = 1;
  manifest.rig_id = "rig_worker";
  manifest.sample_rate = rate;
  manifest.center_hz = center_hz;
  manifest.gain_ch0_db = 25;
  manifest.gain_ch1_db = 25;
  manifest.spacing_m = spacing_m;
  manifest.first_device_timestamp_ns = 900000000LL;
  manifest.capture_host_unix_ns = 1700000000000000000LL;
  manifest.sample_count = ch0.size();
  manifest.ch0_path = "punjdrone_paired_test.ch0.cf32";
  manifest.ch1_path = "punjdrone_paired_test.ch1.cf32";
  manifest.ch0_fnv1a64 = fnv1a64_file(ch0_path);
  manifest.ch1_fnv1a64 = fnv1a64_file(ch1_path);
  manifest.save(manifest_path);

  ui::UiModel model;
  ui::SourceConfig cfg;
  cfg.pair_manifest = manifest_path;
  cfg.fmt = SampleFormat::cf32;
  cfg.df_cal_samples = cal_samples;
  cfg.df_ref = "splitter";
  cfg.df_jsonl = jsonl_path;
  std::atomic<bool> stop{false};
  ui::paired_replay_worker(model, cfg, stop);

  EXPECT_TRUE(model.status().state == ui::UsrpStatus::State::Disconnected);
  EXPECT_EQ(model.df_status().state, ui::DfUiState::Disconnected);
  EXPECT_EQ(model.packets().size(), std::size_t(1));
  EXPECT_TRUE(model.df_status().accepted_bearings >= 2);

  bool found_decoded = false, found_unknown_rf = false;
  for (const auto& track : model.tracks()) {
    if (track.serial == "SysSecWasHere ") {
      found_decoded = true;
      EXPECT_FALSE(track.has_bearing);
      EXPECT_NEAR(track.bearing_deg, drone_angle_deg, 0.2);
      EXPECT_NEAR(track.mirrored_deg, -152.0, 0.2);
      EXPECT_TRUE(track.bearing_ambiguous);
    } else if (track.key == "unknown_rf") {
      found_unknown_rf = true;
      EXPECT_FALSE(track.decoded);
      EXPECT_TRUE(track.serial.empty());
      EXPECT_FALSE(track.has_bearing);
      EXPECT_NEAR(track.bearing_deg, rf_angle_deg, 0.2);
    }
  }
  EXPECT_TRUE(found_decoded);
  EXPECT_TRUE(found_unknown_rf);
  EXPECT_TRUE(std::filesystem::exists(jsonl_path));

  const auto calibration_path = (dir / "punjdrone_paired_test.cal.toml").string();
  PhaseCal expired;
  expired.session_id = manifest.session_id;
  expired.config_epoch = manifest.config_epoch;
  expired.rig_id = manifest.rig_id;
  expired.center_hz = center_hz;
  expired.sample_rate = rate;
  expired.spacing_m = spacing_m;
  expired.gain_db = 25;
  expired.cal_samples = 4096;
  expired.measured_unix_ns = manifest.capture_host_unix_ns - 2000000000;
  expired.max_age_seconds = 1;
  {
    std::ofstream output(calibration_path);
    output << expired.serialize();
  }
  auto expired_config = cfg;
  expired_config.df_cal_samples = 0;
  expired_config.df_cal = calibration_path;
  ui::UiModel expired_model;
  ui::paired_replay_worker(expired_model, expired_config, stop);
  EXPECT_TRUE(expired_model.status().state == ui::UsrpStatus::State::Error);
  EXPECT_EQ(expired_model.df_status().accepted_bearings, uint64_t(0));
  std::filesystem::remove(calibration_path);

  auto uncalibrated_config = cfg;
  uncalibrated_config.df_cal_samples = 0;
  uncalibrated_config.df_jsonl.clear();
  ui::UiModel uncalibrated_model;
  ui::paired_replay_worker(uncalibrated_model, uncalibrated_config, stop);
  EXPECT_EQ(uncalibrated_model.df_status().accepted_bearings, uint64_t(0));
  EXPECT_EQ(uncalibrated_model.df_status().reference_method, std::string("none"));

  auto unverified_config = cfg;
  unverified_config.pair_manifest.clear();
  unverified_config.replay = ch0_path;
  unverified_config.replay_ch1 = ch1_path;
  ui::UiModel unverified_model;
  ui::paired_replay_worker(unverified_model, unverified_config, stop);
  EXPECT_TRUE(unverified_model.status().state == ui::UsrpStatus::State::Error);
  EXPECT_EQ(unverified_model.df_status().accepted_bearings, uint64_t(0));

  // Malformed pair (truncated ch1 / unequal EOF) must fail closed without valid bearings.
  {
    std::ofstream f1(ch1_path, std::ios::binary | std::ios::trunc);
    f1.write(reinterpret_cast<const char*>(ch1.data()), (ch1.size() - 10) * sizeof(cf32));
  }
  ui::UiModel bad_model;
  ui::paired_replay_worker(bad_model, cfg, stop);
  EXPECT_TRUE(bad_model.status().state == ui::UsrpStatus::State::Error);
  EXPECT_EQ(bad_model.df_status().state, ui::DfUiState::Error);
  EXPECT_EQ(bad_model.df_status().accepted_bearings, uint64_t(0));

  std::filesystem::remove(ch0_path);
  std::filesystem::remove(ch1_path);
  std::filesystem::remove(manifest_path);
  std::filesystem::remove(jsonl_path);
}


TEST(UiWorker, PairedChunkIndependenceAndPassbandRejection) {
  const double pi = std::acos(-1.0);
  const double rate = 15.36e6;
  const double center_hz = 2459.5e6;
  const double spacing_m = 0.05;
  const double bias_rad = -0.45;
  const double target_deg = 22.5;

  const auto frame = dji::build_frame176(std::vector<uint8_t>(91, 0x35));
  std::vector<uint8_t> bits;
  EXPECT_TRUE(dji::turbo_encode_frame(frame, bits));
  const auto scrambler = dji::generate_scrambler_seq(7200, dji::scrambler_x2_init());
  for (std::size_t idx = 0; idx < bits.size(); ++idx) bits[idx] ^= scrambler[idx];
  const auto burst = dji::synthesize_burst(bits, rate);

  const std::size_t stride = 65536;
  std::vector<cf32> ch0(2 * stride + 16000, cf32(0, 0));
  std::vector<cf32> ch1(ch0.size(), cf32(0, 0));
  const double geom_phase =
      bias_rad + 2.0 * pi * spacing_m * (center_hz / 299792458.0) * std::sin(target_deg * pi / 180.0);
  for (const auto start : {stride - 5000, 2 * stride - 5000}) {
    for (std::size_t i = 0; i < burst.size(); ++i) {
      ch0[start + i] = burst[i];
      ch1[start + i] = burst[i] * std::polar(1.0f, static_cast<float>(geom_phase));
    }
  }

  DfCalibrationService cal;
  EXPECT_TRUE(cal.connect("sess_chunk", 1, "rig_chunk", center_hz, rate, 20.0));
  PairedBlock ref;
  ref.ch0.resize(2048);
  ref.ch1.resize(2048);
  for (std::size_t i = 0; i < ref.ch0.size(); ++i) {
    ref.ch0[i] = std::polar(0.4f, static_cast<float>(0.015 * i));
    ref.ch1[i] = ref.ch0[i] * std::polar(1.0f, static_cast<float>(bias_rad));
  }
  ref.first_device_timestamp_ns = 1000000LL;
  ref.sample_rate = rate;
  ref.center_hz = center_hz;
  ref.session_id = "sess_chunk";
  ref.config_epoch = 1;
  ref.rig_id = "rig_chunk";
  EXPECT_TRUE(cal.begin_calibration("splitter"));
  EXPECT_TRUE(cal.complete_calibration(ref, spacing_m, 1000000000LL, 60.0, 10e6));

  auto run_chunked = [&](std::size_t chunk_step) {
    ui::PairedWindowAssembler assembler(stride, 12000);
    std::vector<double> angles;
    for (std::size_t pos = 0; pos < ch0.size(); pos += chunk_step) {
      const auto n = std::min(chunk_step, ch0.size() - pos);
      for (const auto& win : assembler.feed(ch0.data() + pos, ch1.data() + pos, n,
                                            1000000LL, rate, pos + n == ch0.size())) {
        for (const auto& pb : dji::decode_paired_samples(win.ch0, win.ch1, {})) {
          if (!pb.decoded.crc_ok ||
              static_cast<std::size_t>(pb.decoded.sample_index) >= win.accept_before)
            continue;
          PairedBlock blk = ref;
          blk.ch0 = pb.ch0_window;
          blk.ch1 = pb.ch1_window;
          const auto b = cal.estimate_block_bearing(blk, 2000000000LL);
          EXPECT_TRUE(b.valid);
          angles.push_back(b.broadside_deg);
        }
      }
    }
    return angles;
  };

  const auto whole_angles = run_chunked(ch0.size());
  const auto chunk_angles = run_chunked(4321);
  EXPECT_EQ(whole_angles.size(), std::size_t(2));
  EXPECT_EQ(chunk_angles.size(), std::size_t(2));
  for (std::size_t i = 0; i < whole_angles.size(); ++i) {
    EXPECT_NEAR(whole_angles[i], target_deg, 0.05);
    EXPECT_NEAR(chunk_angles[i], whole_angles[i], 1e-4);
  }

  // Out-of-passband tone (+6.5 MHz > 5 MHz passband edge) must be rejected.
  PairedBlock oob = ref;
  for (std::size_t i = 0; i < oob.ch0.size(); ++i) {
    oob.ch0[i] = std::polar(0.4f, static_cast<float>(2.0 * pi * 6.5e6 * i / rate));
    oob.ch1[i] = oob.ch0[i] * std::polar(1.0f, static_cast<float>(geom_phase));
  }
  const auto oob_res = cal.estimate_block_bearing(oob, 2000000000LL);
  EXPECT_FALSE(oob_res.valid);
  EXPECT_TRUE(oob_res.reason.find("passband") != std::string::npos);
}

TEST(SweepWorker, O4HistoryDoesNotCrossRetunes) {
  const double rate=20e6;
  auto samples=structured_reference(rate);
  droneid::ui::UiModel model;
  std::vector<droneid::cf32> short_dwell(samples.begin(), samples.begin()+static_cast<std::size_t>(rate*.032));
  for (int visit=0;visit<10;++visit)
    droneid::ui::process_sweep_capture(model,short_dwell,droneid::SweepMode::O4,rate);
  EXPECT_EQ(model.rf_summary().o4_candidate_windows,uint64_t(0));
  droneid::ui::process_sweep_capture(model,samples,droneid::SweepMode::O4,rate);
  EXPECT_TRUE(model.rf_summary().o4_candidate_windows>0);
  EXPECT_TRUE(model.tracks().empty());
  EXPECT_EQ(model.packet_count(),uint64_t(0));
}

TEST(SweepWorker, DecodesDroneIdAtSharedTwentyMsps) {
  FileSource source(std::string(DRONEID_TEST_DATA_DIR)+"/mini2_centered.sc16",SampleFormat::sc16,15.36e6);
  std::vector<cf32> samples(16000); int64_t timestamp=0;
  samples.resize(source.recv(samples.data(),samples.size(),timestamp));
  const auto at20=odid::resample(samples,15.36e6,20e6);
  ui::UiModel model;
  ui::process_sweep_capture(model,at20,SweepMode::DroneId,20e6);
  EXPECT_EQ(model.packet_count(),uint64_t(1));
  EXPECT_EQ(model.tracks().size(),std::size_t(1));
  EXPECT_EQ(model.packets().front().inner_crc16,std::string("OK"));
}

TEST(SweepWorker, DecodesWifiRidAtSharedTwentyMsps) {
  odid::BasicId id; id.id_type=1; id.ua_type=2; id.uas_id="SWEEP-TEST";
  auto mpdu=odid::build_beacon_mpdu({2,0,0,0,0,1},"sweep",0,odid::encode_pack({odid::encode(id)}));
  auto waveform=odid::synth_ofdm_ppdu(mpdu,6);
  std::vector<cf32> samples(10000);
  samples.insert(samples.end(),waveform.begin(),waveform.end()); samples.resize(samples.size()+10000);
  ui::UiModel model(1);
  ui::process_sweep_capture(model,samples,SweepMode::WifiRid,20e6);
  ui::process_sweep_capture(model,samples,SweepMode::WifiRid,20e6);
  EXPECT_EQ(model.packet_count(),uint64_t(2));
  EXPECT_EQ(model.packets().size(),std::size_t(1));
  EXPECT_EQ(model.packets().front().serial,std::string("SWEEP-TEST"));
  EXPECT_EQ(model.packets().front().source,std::string("ODID/ASTM"));
}

MICROTEST_MAIN

