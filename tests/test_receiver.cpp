#include "droneid/aoa.h"
#include "droneid/hop.h"
#include "droneid/test/microtest.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

template <typename Action> bool rejects(Action action) {
  try { action(); } catch (const std::exception&) { return true; }
  return false;
}

TEST(Hop, TimingAndCoverageModel) {
  const auto channels = droneid::HopSchedule::channels_for_band(58);
  EXPECT_EQ(channels.size(), std::size_t(3));
  EXPECT_TRUE(channels[0] > 4294967295.0);
  const double dwell = droneid::HopSchedule::dwell_for_probability(0.64, 0.00065, 0.95);
  droneid::HopSchedule scheduler(channels, dwell, 0.05);
  EXPECT_NEAR(scheduler.catch_probability(0.64, 0.00065), 0.95, 1e-10);
  const auto visit = scheduler.visit(4, 12);
  EXPECT_EQ(visit.center_hz, channels[1]);
  EXPECT_NEAR(visit.tune_seconds, 12 + 4 * (dwell + 0.05), 1e-10);
  EXPECT_NEAR(visit.listen_seconds - visit.tune_seconds, 0.05, 1e-10);
  EXPECT_NEAR(visit.end_seconds - visit.listen_seconds, dwell, 1e-10);
  EXPECT_NEAR(scheduler.cycle_seconds(), 3 * (dwell + 0.05), 1e-10);
  EXPECT_EQ(droneid::HopSchedule(channels, 0.0001, 0).catch_probability(0.64, 0.00065), 0.0);
  EXPECT_EQ(droneid::HopSchedule(channels, 1, 0).catch_probability(0.64, 0.00065), 1.0);
}

TEST(Hop, RejectInvalidInputs) {
  EXPECT_TRUE(rejects([] { droneid::HopSchedule({}, 1, 0); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule({2.4e9}, -1, 0); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule({7e9}, 1, 0); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::channels_for_band(5); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::dwell_for_probability(0, 1, 0.5); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::dwell_for_probability(1, 0.1, 1.1); }));
  EXPECT_TRUE(rejects([] { droneid::HopSchedule::dwell_for_probability(1, 2, 0.5); }));
}

TEST(Direction, CalibratedAnglesAndRejections) {
  droneid::PhaseCal calibration;
  calibration.phase_rad = 2.7;
  calibration.center_hz = 2459.5e6;
  calibration.sample_rate = 15.36e6;
  calibration.spacing_m = 0.05;
  calibration.measured_unix_ns = 1000000000;
  calibration.max_age_seconds = 60;
  const double pi = std::acos(-1.0);
  std::vector<droneid::cf32> first(4096), second(4096);
  for (double angle : {-55.0, -30.0, 0.0, 30.0, 55.0}) {
    const double phase = calibration.phase_rad + 2 * pi * calibration.spacing_m *
        calibration.center_hz / 299792458.0 * std::sin(angle * pi / 180);
    for (std::size_t index = 0; index < first.size(); ++index) {
      first[index] = std::polar(0.4f, static_cast<float>(index * 0.013));
      second[index] = first[index] * std::polar(0.6f, static_cast<float>(phase));
    }
    const auto result = droneid::estimate_bearing(first, second, 123, 123, calibration.center_hz,
        calibration.sample_rate, calibration, 2000000000);
    EXPECT_TRUE(result.valid);
    EXPECT_NEAR(result.broadside_deg, angle, 1e-4);
    EXPECT_NEAR(result.coherence, 1, 1e-6);
    EXPECT_TRUE(result.ambiguous_front_back);
  }
  auto estimate = [&](int64_t timestamp, double center, const droneid::PhaseCal& supplied, int64_t now) {
    return droneid::estimate_bearing(first, second, 123, timestamp, center,
                                    calibration.sample_rate, supplied, now);
  };
  EXPECT_FALSE(estimate(124, calibration.center_hz, calibration, 2000000000).valid);
  EXPECT_FALSE(estimate(123, calibration.center_hz + 10, calibration, 2000000000).valid);
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 62000000000).valid);
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 0).valid);
  auto invalid = calibration;
  invalid.spacing_m = 1;
  EXPECT_FALSE(estimate(123, calibration.center_hz, invalid, 2000000000).valid);
  invalid = calibration;
  invalid.phase_rad = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(estimate(123, calibration.center_hz, invalid, 2000000000).valid);
  for (std::size_t index = 0; index < second.size(); ++index)
    second[index] = std::polar(0.5f, static_cast<float>(index * 0.2));
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 2000000000).valid);
  second.assign(first.size(), {});
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 2000000000).valid);
  second.pop_back();
  EXPECT_FALSE(estimate(123, calibration.center_hz, calibration, 2000000000).valid);
}

TEST(DfGate1, PairedBlockAndManifestTransaction) {
  droneid::PairedBlock block;
  block.ch0.assign(512, droneid::cf32(0.2f, 0.1f));
  block.ch1.assign(512, droneid::cf32(0.1f, 0.2f));
  block.sample_rate = 15.36e6;
  block.center_hz = 2459.5e6;
  block.session_id = "sess_df1";
  block.config_epoch = 1;
  block.rig_id = "rig_a";

  // Missing/zero device timestamp must be rejected (never fabricated zero timestamp).
  block.first_device_timestamp_ns = 0;
  EXPECT_FALSE(block.validate());
  block.first_device_timestamp_ns = -1;
  EXPECT_FALSE(block.validate());
  block.first_device_timestamp_ns = 500000000LL;
  EXPECT_TRUE(block.validate());

  // Gap, overflow, channel swap/duplicate, or length mismatch must be rejected.
  block.has_gap = true;
  EXPECT_FALSE(block.validate());
  block.has_gap = false;
  block.has_overflow = true;
  EXPECT_FALSE(block.validate());
  block.has_overflow = false;
  block.channel_order = {0, 0};
  EXPECT_FALSE(block.validate());
  block.channel_order = {0, 1};
  EXPECT_TRUE(block.validate());

  // Manifest-last transaction & file/checksum/sidecar validation.
  const auto dir = std::filesystem::temp_directory_path();
  const auto ch0_file = (dir / "df1_test.ch0.cf32").string();
  const auto ch1_file = (dir / "df1_test.ch1.cf32").string();
  const auto manifest_file = (dir / "df1_test.pair.toml").string();
  {
    std::ofstream f0(ch0_file, std::ios::binary);
    std::ofstream f1(ch1_file, std::ios::binary);
    f0.write(reinterpret_cast<const char*>(block.ch0.data()), block.ch0.size() * sizeof(droneid::cf32));
    f1.write(reinterpret_cast<const char*>(block.ch1.data()), block.ch1.size() * sizeof(droneid::cf32));
  }
  {
    std::ofstream m0(ch0_file + ".meta");
    std::ofstream m1(ch1_file + ".meta");
    m0 << "format=cf32\nsample_rate=15360000\ncenter_freq=2459500000\nchannel=0\n"
          "first_device_timestamp_ns=500000000\nsamples=512\noverflows=0\n";
    m1 << "format=cf32\nsample_rate=15360000\ncenter_freq=2459500000\nchannel=1\n"
          "first_device_timestamp_ns=500000000\nsamples=512\noverflows=0\n";
  }

  droneid::PairManifest manifest;
  manifest.complete = false;
  manifest.status_reason = "interrupted";
  manifest.session_id = "sess_df1";
  manifest.config_epoch = 1;
  manifest.rig_id = "rig_a";
  manifest.sample_rate = 15.36e6;
  manifest.center_hz = 2459.5e6;
  manifest.gain_ch0_db = 20;
  manifest.gain_ch1_db = 20;
  manifest.first_device_timestamp_ns = 500000000LL;
  manifest.capture_host_unix_ns = 1700000000000000000LL;
  manifest.sample_count = 512;
  manifest.ch0_path = "df1_test.ch0.cf32";
  manifest.ch1_path = "df1_test.ch1.cf32";
  manifest.ch0_fnv1a64 = droneid::fnv1a64_file(ch0_file);
  manifest.ch1_fnv1a64 = droneid::fnv1a64_file(ch1_file);

  // Incomplete manifest must be rejected.
  EXPECT_FALSE(manifest.validate_files(ch0_file, ch1_file));

  // Complete manifest passes roundtrip and file verification.
  manifest.complete = true;
  manifest.status_reason = "ok";
  manifest.save(manifest_file);
  const auto loaded = droneid::PairManifest::load(manifest_file);
  EXPECT_TRUE(loaded.validate_files(ch0_file, ch1_file));
  auto observed = loaded;
  observed.hardware_observed = true;
  observed.master_clock_rate = 184.32e6;
  observed.wire_format = "sc16";
  observed.lo_locked_ch0 = true;
  observed.lo_locked_ch1 = true;
  observed.ref_locked = true;
  observed.rf_tune_hz = {2459.5e6, 2459.5e6};
  observed.save(manifest_file);
  const auto observed_loaded = droneid::PairManifest::load(manifest_file);
  EXPECT_TRUE(observed_loaded.hardware_observed);
  EXPECT_TRUE(observed_loaded.validate_files(ch0_file, ch1_file));
  EXPECT_EQ(observed_loaded.master_clock_rate, 184.32e6);
  observed.lo_locked_ch1 = false;
  EXPECT_FALSE(observed.validate_files(ch0_file, ch1_file));

  // Corrupted checksum or unequal EOF must fail validation.
  auto bad_hash = loaded;
  bad_hash.ch1_fnv1a64 = "0000000000000000";
  EXPECT_FALSE(bad_hash.validate_files(ch0_file, ch1_file));

  auto missing_hash = loaded;
  missing_hash.ch1_fnv1a64.clear();
  EXPECT_FALSE(missing_hash.validate_files(ch0_file, ch1_file));
  {
    std::ofstream metadata(ch1_file + ".meta", std::ios::app);
    metadata << "sample_rate=50000000\nsession_id=different_session\n";
  }
  EXPECT_FALSE(loaded.validate_files(ch0_file, ch1_file));

  std::filesystem::remove(ch0_file);
  std::filesystem::remove(ch1_file);
  std::filesystem::remove(ch0_file + ".meta");
  std::filesystem::remove(ch1_file + ".meta");
  std::filesystem::remove(manifest_file);
}

TEST(DfGate2And3, CalibrationLifecycleAndQualityGates) {
  const double pi = std::acos(-1.0);
  const double center_hz = 2459.5e6;
  const double rate = 15.36e6;
  const double spacing_m = 0.05;
  const double bias_rad = -1.15;

  droneid::DfCalibrationService service;
  EXPECT_EQ(service.state(), droneid::DfCalState::Disconnected);

  // Connect transitions Disconnected -> Uncalibrated.
  EXPECT_TRUE(service.connect("sess_live_1", 1, "rig_ubx", center_hz, rate, 30.0, {0, 1}, true, true));
  EXPECT_EQ(service.state(), droneid::DfCalState::Uncalibrated);

  // Build a clean splitter reference block.
  droneid::PairedBlock ref;
  ref.ch0.resize(2048);
  ref.ch1.resize(2048);
  for (std::size_t i = 0; i < ref.ch0.size(); ++i) {
    ref.ch0[i] = std::polar(0.35f, static_cast<float>(0.021 * i));
    ref.ch1[i] = ref.ch0[i] * std::polar(1.0f, static_cast<float>(bias_rad));
  }
  ref.first_device_timestamp_ns = 100000000LL;
  ref.sample_rate = rate;
  ref.center_hz = center_hz;
  ref.session_id = "sess_live_1";
  ref.config_epoch = 1;
  ref.rig_id = "rig_ubx";
  ref.channel_order = {0, 1};

  // Uncalibrated block estimation must be rejected.
  EXPECT_FALSE(service.estimate_block_bearing(ref, 2000000000LL).valid);

  // Calibrate: Uncalibrated -> Calibrating -> Valid.
  EXPECT_TRUE(service.begin_calibration("splitter"));
  EXPECT_EQ(service.state(), droneid::DfCalState::Calibrating);
  EXPECT_TRUE(service.complete_calibration(ref, spacing_m, 1000000000LL, 60.0, 10e6));
  EXPECT_EQ(service.state(), droneid::DfCalState::Valid);

  // Estimate target at +30 deg and +60 deg; check mirrored angle and endfire uncertainty growth.
  auto make_target = [&](double deg) {
    droneid::PairedBlock blk = ref;
    blk.first_device_timestamp_ns = 200000000LL;
    const double geom_phase =
        bias_rad + 2.0 * pi * spacing_m * (center_hz / 299792458.0) * std::sin(deg * pi / 180.0);
    for (std::size_t i = 0; i < blk.ch0.size(); ++i) {
      blk.ch0[i] = std::polar(0.35f, static_cast<float>(0.017 * i));
      blk.ch1[i] = blk.ch0[i] * std::polar(0.95f, static_cast<float>(geom_phase + ((i & 1) ? 0.01 : -0.01)));
    }
    return blk;
  };
  const auto b0 = service.estimate_block_bearing(make_target(0.0), 2000000000LL, 1e-5, 10.0);
  const auto b30 = service.estimate_block_bearing(make_target(30.0), 2000000000LL, 1e-5, 10.0);
  const auto b60 = service.estimate_block_bearing(make_target(60.0), 2000000000LL, 1e-5, 10.0);
  EXPECT_TRUE(b0.valid);
  EXPECT_TRUE(b30.valid);
  EXPECT_TRUE(b60.valid);
  EXPECT_NEAR(b30.broadside_deg, 30.0, 0.05);
  EXPECT_NEAR(b30.mirrored_deg, 150.0, 0.05);
  EXPECT_TRUE(b60.uncertainty_deg > b0.uncertainty_deg * 1.8);  // Endfire sensitivity check.

  // Clipping gate rejection.
  auto clipped = make_target(15.0);
  for (std::size_t i = 0; i < 20; ++i) clipped.ch0[i] = droneid::cf32(1.0f, 1.0f);
  EXPECT_FALSE(service.estimate_block_bearing(clipped, 2000000000LL).valid);

  // Excessive phase dispersion / overlapping emitter gate rejection.
  auto dispersed = make_target(15.0);
  for (std::size_t i = dispersed.ch1.size() / 2; i < dispersed.ch1.size(); ++i)
    dispersed.ch1[i] *= std::polar(1.0f, 1.2f);
  EXPECT_FALSE(service.estimate_block_bearing(dispersed, 2000000000LL).valid);

  // Channel swap rejection.
  auto swapped = make_target(15.0);
  swapped.channel_order = {1, 0};
  EXPECT_FALSE(service.estimate_block_bearing(swapped, 2000000000LL).valid);

  // Retune transitions Valid -> Stale and suppresses bearings.
  service.on_retune(2444.5e6);
  EXPECT_EQ(service.state(), droneid::DfCalState::Stale);
  EXPECT_FALSE(service.estimate_block_bearing(make_target(15.0), 2000000000LL).valid);

  // Recalibrate with OTA broadside method, then verify gap/overflow and LO unlock invalidation.
  ref.center_hz = 2444.5e6;
  ref.config_epoch = service.config_epoch();
  EXPECT_TRUE(service.begin_calibration("ota_broadside"));
  EXPECT_TRUE(service.complete_calibration(ref, spacing_m, 1500000000LL, 60.0, 10e6));
  EXPECT_EQ(service.state(), droneid::DfCalState::Valid);
  EXPECT_EQ(service.calibration().reference_method, std::string("ota_broadside"));

  auto gapped = ref;
  gapped.has_gap = true;
  EXPECT_FALSE(service.estimate_block_bearing(gapped, 2000000000LL).valid);
  EXPECT_EQ(service.state(), droneid::DfCalState::Stale);

  service.on_lo_unlock();
  EXPECT_EQ(service.state(), droneid::DfCalState::Error);
  service.on_disconnect();
  EXPECT_EQ(service.state(), droneid::DfCalState::Disconnected);
}

TEST(DfReview, LoadedCalibrationCannotBypassGainOrRig) {
  droneid::DfCalibrationService service;
  EXPECT_TRUE(service.connect("session", 1, "rig", 2459.5e6, 15.36e6, 20));
  droneid::PhaseCal calibration;
  calibration.session_id = "session";
  calibration.config_epoch = 1;
  calibration.rig_id = "rig";
  calibration.center_hz = 2459.5e6;
  calibration.sample_rate = 15.36e6;
  calibration.spacing_m = 0.05;
  calibration.measured_unix_ns = 1000000000;
  calibration.cal_samples = 2048;
  calibration.gain_db = 0;
  EXPECT_FALSE(service.adopt_same_session_calibration(calibration, 2000000000));
  calibration.gain_db = 20;
  calibration.rig_id.clear();
  EXPECT_FALSE(service.adopt_same_session_calibration(calibration, 2000000000));
  calibration.rig_id = "rig";
  calibration.delay_slope_rad_per_hz = 1e-8;
  EXPECT_FALSE(service.adopt_same_session_calibration(calibration, 2000000000));
  calibration.delay_slope_rad_per_hz = 0;
  EXPECT_TRUE(service.adopt_same_session_calibration(calibration, 2000000000));
}

TEST(DfReview, SymmetricOutOfBandEnergyIsNotAnInBandSignal) {
  droneid::DfCalibrationService service;
  EXPECT_TRUE(service.connect("session", 1, "rig", 2459.5e6, 15.36e6, 20));
  droneid::PairedBlock reference;
  reference.sample_rate = 15.36e6;
  reference.center_hz = 2459.5e6;
  reference.first_device_timestamp_ns = 1000000000;
  reference.session_id = "session";
  reference.config_epoch = 1;
  reference.rig_id = "rig";
  reference.ch0.assign(4096, droneid::cf32(0.3f, 0));
  reference.ch1 = reference.ch0;
  EXPECT_TRUE(service.begin_calibration());
  EXPECT_TRUE(service.complete_calibration(reference, 0.05, 1000000000, 60, 2e6));
  auto target = reference;
  for (size_t index = 0; index < target.ch0.size(); ++index) {
    const double phase = 2 * std::acos(-1.0) * 3e6 * index / target.sample_rate;
    target.ch0[index] = droneid::cf32(0.3 * std::cos(phase), 0);
    target.ch1[index] = target.ch0[index];
  }
  const auto result = service.estimate_block_bearing(target, 2000000000);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.reason.find("passband") != std::string::npos);
}

MICROTEST_MAIN
