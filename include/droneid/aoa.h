#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "droneid/types.h"

namespace droneid {

// DF-1: Versioned sidecar metadata parsed from <path>.meta.
struct SidecarMeta {
  bool present = false;
  SampleFormat format = SampleFormat::cf32;
  double sample_rate = 0;
  double center_freq = 0;
  int channel = -1;
  int64_t first_device_timestamp_ns = -1;
  int64_t capture_host_unix_ns = 0;
  uint64_t samples = 0;
  uint64_t overflows = 0;
  bool phase_calibrated = false;
  std::string session_id;
  uint64_t config_epoch = 0;
  std::string rig_id;

  static SidecarMeta load(const std::string& iq_path);
};

// DF-1: Paired sample block with explicit device timestamp, session/epoch provenance, and fault flags.
// A missing device timestamp (<= 0) is invalid, never a fabricated zero timestamp.
struct PairedBlock {
  std::vector<cf32> ch0;
  std::vector<cf32> ch1;
  int64_t first_device_timestamp_ns = -1;
  uint64_t start_sample = 0;
  double sample_rate = 0;
  double center_hz = 0;
  std::string session_id;
  uint64_t config_epoch = 0;
  std::string rig_id;
  std::array<std::size_t, 2> channel_order = {0, 1};
  bool has_gap = false;
  bool has_overflow = false;

  bool validate(std::string* reason = nullptr) const;
};

// DF-1: Versioned capture manifest for transactional two-channel recordings.
// Host wall time is freshness metadata only, never sample alignment evidence.
struct PairManifest {
  int schema_version = 1;
  bool complete = false;
  std::string status_reason = "uninitialized";
  std::string session_id;
  uint64_t config_epoch = 1;
  std::string rig_id = "default_rig";
  std::string cable_id = "default_cables";
  std::string radio_serial = "?";
  std::string dboard_ch0 = "UBX";
  std::string dboard_ch1 = "UBX";
  std::string uhd_version = "unknown";
  std::string fpga_version = "unknown";
  std::string clock_source = "internal";
  std::string time_source = "internal";
  double sample_rate = 15.36e6;
  double center_hz = 0;
  double gain_ch0_db = 0;
  double gain_ch1_db = 0;
  double bandwidth_hz = 10e6;
  std::string antenna_ch0 = "RX2";
  std::string antenna_ch1 = "RX2";
  std::array<std::size_t, 2> channel_order = {0, 1};
  double spacing_m = 0.05;
  double orientation_deg = 0;
  int64_t first_device_timestamp_ns = -1;
  int64_t capture_host_unix_ns = 0;
  uint64_t sample_count = 0;
  uint64_t overflows = 0;
  uint64_t discontinuities = 0;
  std::string ch0_path;
  std::string ch1_path;
  std::string ch0_fnv1a64;
  std::string ch1_fnv1a64;

  static PairManifest load(const std::string& path);
  void save(const std::string& path) const;
  std::string serialize() const;
  bool validate_files(const std::string& resolved_ch0, const std::string& resolved_ch1,
                      std::string* reason = nullptr) const;
};

uint64_t fnv1a64_update(uint64_t hash, const void* data, std::size_t bytes);
std::string fnv1a64_hex(uint64_t hash);
std::string fnv1a64_file(const std::string& path, uint64_t* byte_count = nullptr);

// DF-2: Persistent calibration lifecycle states.
enum class DfCalState {
  Disconnected,
  Uncalibrated,
  Calibrating,
  Valid,
  Stale,
  Error
};

const char* to_string(DfCalState state);

struct PhaseCal {
  double phase_rad = 0;
  double center_hz = 0;
  double sample_rate = 0;
  double spacing_m = 0;
  int64_t measured_unix_ns = 0;
  double max_age_seconds = 3600;

  // DF-2 extended session/rig provenance and passband validity fields (optional in legacy TOML):
  std::string session_id;
  uint64_t config_epoch = 0;
  std::string rig_id;
  std::string reference_method = "splitter";  // "splitter" or "ota_broadside"
  double passband_hz = 15.36e6;
  double gain_db = 0;
  std::array<std::size_t, 2> channel_order = {0, 1};
  uint64_t cal_samples = 0;
  double cal_coherence = 1.0;
  double phase_dispersion_rad = 0;
  double delay_slope_rad_per_hz = 0;
  bool lo_locked_ch0 = true;
  bool lo_locked_ch1 = true;

  static PhaseCal load(const std::string& path);
  std::string serialize() const;
};

struct Bearing {
  bool valid = false;
  double broadside_deg = 0;
  double mirrored_deg = 0;
  double uncertainty_deg = 0;
  double coherence = 0;
  double phase_diff_rad = 0;
  double phase_std_deg = 0;
  double snr_db = 0;
  double clipped_fraction = 0;
  bool ambiguous_front_back = true;
  std::string reason;
};

Bearing estimate_bearing(const std::vector<cf32>& first, const std::vector<cf32>& second,
                         int64_t first_ns, int64_t second_ns, double center_hz,
                         double sample_rate, const PhaseCal& calibration, int64_t now_unix_ns);

// DF-2 & DF-3: Persistent same-session calibration state machine and block/burst validator.
class DfCalibrationService {
 public:
  DfCalibrationService() = default;

  DfCalState state() const { return state_; }
  const std::string& reason() const { return reason_; }
  const PhaseCal& calibration() const { return cal_; }
  const std::string& session_id() const { return session_id_; }
  uint64_t config_epoch() const { return config_epoch_; }
  const std::string& rig_id() const { return rig_id_; }

  bool connect(const std::string& session_id, uint64_t config_epoch, const std::string& rig_id,
               double center_hz, double sample_rate, double gain_db,
               std::array<std::size_t, 2> channel_order = {0, 1},
               bool lo_locked_ch0 = true, bool lo_locked_ch1 = true);

  bool begin_calibration(const std::string& reference_method = "splitter");

  bool complete_calibration(const PairedBlock& reference_block, double spacing_m,
                            int64_t measured_unix_ns, double max_age_seconds = 3600,
                            double passband_hz = 10e6);

  // Adopt a loaded calibration only if its session/epoch/rig/RF parameters match the active session.
  bool adopt_same_session_calibration(const PhaseCal& cal, int64_t now_unix_ns);

  void on_retune(double new_center_hz);
  void on_config_change(const std::string& change_reason);
  void on_gap_or_overflow(const std::string& gap_reason);
  void on_lo_unlock(const std::string& lock_reason = "LO lock lost on one or both channels");
  void on_disconnect(const std::string& disc_reason = "receiver disconnected");

  // DF-3: Estimate bearing on a paired block with full session/provenance, SNR, clipping,
  // phase dispersion, passband, and endfire uncertainty gates.
  Bearing estimate_block_bearing(const PairedBlock& block, int64_t now_unix_ns,
                                 double noise_power = 0.0, double min_snr_db = 6.0);

 private:
  DfCalState state_ = DfCalState::Disconnected;
  std::string reason_ = "receiver disconnected";
  std::string session_id_;
  uint64_t config_epoch_ = 0;
  std::string rig_id_;
  double center_hz_ = 0;
  double sample_rate_ = 0;
  double gain_db_ = 0;
  std::array<std::size_t, 2> channel_order_ = {0, 1};
  bool lo_locked_ch0_ = false;
  bool lo_locked_ch1_ = false;
  std::string pending_ref_method_ = "splitter";
  PhaseCal cal_;
};

}  // namespace droneid
