#include "receiver_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

#include "droneid/aoa.h"
#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/rf_metrics.h"
#ifdef DRONEID_HAVE_UHD
#include "droneid/usrp_source.h"
#endif

namespace droneid::ui {
namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

DfUiState map_cal_state(DfCalState s) {
  switch (s) {
    case DfCalState::Disconnected: return DfUiState::Disconnected;
    case DfCalState::Uncalibrated: return DfUiState::Uncalibrated;
    case DfCalState::Calibrating:  return DfUiState::Calibrating;
    case DfCalState::Valid:        return DfUiState::Valid;
    case DfCalState::Stale:        return DfUiState::Stale;
    case DfCalState::Error:        return DfUiState::Error;
  }
  return DfUiState::Error;
}

void sync_df_status(UiModel& model, const DfCalibrationService& service, int64_t current_ns,
                    uint64_t dropped_pairs, double orientation_deg = 0.0) {
  auto df = model.df_status();
  df.state = map_cal_state(service.state());
  df.reason = service.reason();
  df.session_id = service.session_id();
  df.config_epoch = service.config_epoch();
  df.rig_id = service.rig_id();
  df.reference_method = service.calibration().measured_unix_ns <= 0 ||
                                service.calibration().reference_method.empty()
                            ? "none"
                            : service.calibration().reference_method;
  df.center_hz = service.calibration().center_hz;
  df.passband_hz = service.calibration().passband_hz;
  df.spacing_m = service.calibration().spacing_m;
  df.orientation_deg = orientation_deg;
  df.dropped_pairs = dropped_pairs;
  df.updated_ms = now_ms();
  if (service.state() == DfCalState::Valid && service.calibration().measured_unix_ns > 0 &&
      current_ns >= service.calibration().measured_unix_ns) {
    df.cal_age_seconds =
        static_cast<double>(current_ns - service.calibration().measured_unix_ns) / 1e9;
  } else {
    df.cal_age_seconds = 0;
  }
  model.set_df_status(df);
}

void process_window(UiModel& model, const SampleWindow& window, const dji::DecodeOptions& options) {
  for (const auto& burst : dji::decode_samples(window.samples, options)) {
    if (!burst.crc_ok || static_cast<std::size_t>(burst.sample_index) >= window.accept_before) continue;
    dji::Telemetry telemetry;
    PacketRow packet;
    packet.time_ms = now_ms();
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : burst.payload) {
      packet.payload_hex += hex[byte >> 4];
      packet.payload_hex += hex[byte & 15];
    }
    const auto envelope = dji::inspect_packet(burst.payload);
    packet.payload_format = envelope.format;
    packet.inner_crc16 = !envelope.length_valid ? "invalid_length" : envelope.crc16_ok ? "OK" : "fail";
    packet.telemetry_parsed = dji::parse_frame(burst.payload, telemetry);
    if (!packet.telemetry_parsed) {
      packet.type = "Unparsed " + packet.payload_format + " (outer CRC OK; inner " + packet.inner_crc16 + ")";
      model.add_packet(packet);
      continue;
    }
    packet.sequence = telemetry.sequence;
    packet.serial = telemetry.serial;
    packet.type = telemetry.type_name;
    packet.drone_lat = telemetry.drone_lat;
    packet.drone_lon = telemetry.drone_lon;
    packet.altitude_m = telemetry.altitude_m;
    packet.height_m = telemetry.height_m;
    packet.pilot_lat = telemetry.pilot_lat;
    packet.pilot_lon = telemetry.pilot_lon;
    packet.home_lat = telemetry.home_lat;
    packet.home_lon = telemetry.home_lon;
    packet.pilot_valid = telemetry.pilot_valid;
    packet.home_valid = telemetry.home_valid;
    model.add_packet(packet);
    DroneTrack track;
    track.key = telemetry.serial;
    track.serial = telemetry.serial;
    track.type = telemetry.type_name;
    track.corr = burst.corr;
    track.decoded = true;
    track.last_ms = packet.time_ms;
    model.upsert_track(track);
  }
  const std::size_t count = std::min(window.accept_before, window.samples.size());
  if (count < 1024) return;
  std::vector<float> power;
  for (std::size_t index = 0; index + 32 <= count; index += 32) {
    double sum = 0;
    for (std::size_t offset = 0; offset < 32; ++offset) sum += std::norm(window.samples[index + offset]);
    power.push_back(static_cast<float>(sum / 32));
  }
  const auto quantile = power.begin() + power.size() / 5;
  std::nth_element(power.begin(), quantile, power.end());
  const double noise = std::clamp(static_cast<double>(*quantile), 1e-20, 1.0);
  EnergyBurstDetector detector(options.sample_rate, 10 * std::log10(noise));
  for (const auto& burst : detector.feed(window.samples.data(), count, true)) {
    const auto metrics = measure_rf(burst.samples, options.sample_rate, noise);
    model.record_rf_burst(metrics.rssi_dbfs, metrics.snr_db, metrics.bandwidth_hz,
                          metrics.spectral_center_hz, now_ms());
  }
}

void process_paired_window(UiModel& model, const PairedSampleWindow& window,
                           const dji::DecodeOptions& options, double center_hz,
                           DfCalibrationService& cal_service, int64_t eval_unix_ns,
                           uint64_t& next_burst_id, std::ofstream* jsonl_out) {
  if (window.after_drop_or_gap) {
    cal_service.on_gap_or_overflow("paired queue drop or gap invalidated calibration");
    model.invalidate_bearings(map_cal_state(cal_service.state()),
                              "paired queue drop or gap reset active bearings");
  }
  const std::size_t count = std::min(window.accept_before, window.ch0.size());
  double noise = 1e-6;
  if (count >= 1024) {
    std::vector<float> power;
    for (std::size_t index = 0; index + 32 <= count; index += 32) {
      double sum = 0;
      for (std::size_t offset = 0; offset < 32; ++offset)
        sum += std::norm(window.ch0[index + offset]);
      power.push_back(static_cast<float>(sum / 32));
    }
    const auto quantile = power.begin() + power.size() / 5;
    std::nth_element(power.begin(), quantile, power.end());
    noise = std::clamp(static_cast<double>(*quantile), 1e-20, 1.0);
  }

  const auto paired_bursts = dji::decode_paired_samples(window.ch0, window.ch1, options);

  std::vector<std::pair<uint64_t, uint64_t>> decoded_intervals;
  for (const auto& pb : paired_bursts) {
    const auto& burst = pb.decoded;
    if (!burst.crc_ok || static_cast<std::size_t>(burst.sample_index) >= window.accept_before)
      continue;
    const uint64_t global_start = window.start_sample + static_cast<uint64_t>(burst.sample_index);
    const uint64_t global_end = window.start_sample + static_cast<uint64_t>(pb.end_sample_index);
    decoded_intervals.push_back({global_start, global_end});

    dji::Telemetry telemetry;
    PacketRow packet;
    packet.time_ms = now_ms();
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : burst.payload) {
      packet.payload_hex += hex[byte >> 4];
      packet.payload_hex += hex[byte & 15];
    }
    const auto envelope = dji::inspect_packet(burst.payload);
    packet.payload_format = envelope.format;
    packet.inner_crc16 = !envelope.length_valid ? "invalid_length" : envelope.crc16_ok ? "OK" : "fail";
    packet.telemetry_parsed = dji::parse_frame(burst.payload, telemetry);
    if (!packet.telemetry_parsed) {
      packet.type = "Unparsed " + packet.payload_format + " (outer CRC OK; inner " + packet.inner_crc16 + ")";
      model.add_packet(packet);
    } else {
      packet.sequence = telemetry.sequence;
      packet.serial = telemetry.serial;
      packet.type = telemetry.type_name;
      packet.drone_lat = telemetry.drone_lat;
      packet.drone_lon = telemetry.drone_lon;
      packet.altitude_m = telemetry.altitude_m;
      packet.height_m = telemetry.height_m;
      packet.pilot_lat = telemetry.pilot_lat;
      packet.pilot_lon = telemetry.pilot_lon;
      packet.home_lat = telemetry.home_lat;
      packet.home_lon = telemetry.home_lon;
      packet.pilot_valid = telemetry.pilot_valid;
      packet.home_valid = telemetry.home_valid;
      model.add_packet(packet);
      DroneTrack track;
      track.key = telemetry.serial;
      track.serial = telemetry.serial;
      track.type = telemetry.type_name;
      track.corr = burst.corr;
      track.decoded = true;
      track.last_ms = packet.time_ms;
      model.upsert_track(track);
    }

    PairedBlock blk;
    blk.ch0 = pb.ch0_window;
    blk.ch1 = pb.ch1_window;
    blk.first_device_timestamp_ns =
        window.first_device_timestamp_ns +
        static_cast<int64_t>(std::llround(burst.sample_index * 1e9 / options.sample_rate));
    blk.start_sample = global_start;
    blk.sample_rate = options.sample_rate;
    blk.center_hz = center_hz;
    blk.session_id = cal_service.session_id();
    blk.config_epoch = cal_service.config_epoch();
    blk.rig_id = cal_service.rig_id();
    blk.channel_order = cal_service.calibration().channel_order;

    const int64_t burst_unix_ns = eval_unix_ns +
        static_cast<int64_t>(std::llround(global_start * 1e9 / options.sample_rate));
    const auto bearing = cal_service.estimate_block_bearing(blk, burst_unix_ns, noise, 6.0);
    DfBearingEvent ev;
    ev.burst_id = ++next_burst_id;
    ev.start_sample = global_start;
    ev.end_sample = global_end;
    ev.session_id = cal_service.session_id();
    ev.config_epoch = cal_service.config_epoch();
    if (cal_service.calibration().measured_unix_ns > 0)
      ev.cal_id = cal_service.calibration().reference_method + "@" +
                  std::to_string(cal_service.calibration().measured_unix_ns);
    ev.classification = packet.telemetry_parsed ? "decoded_droneid" : "unknown_rf";
    ev.serial = packet.telemetry_parsed ? telemetry.serial : "";
    ev.valid = bearing.valid;
    ev.broadside_deg = bearing.broadside_deg;
    ev.mirrored_deg = bearing.mirrored_deg;
    ev.uncertainty_deg = bearing.uncertainty_deg;
    ev.coherence = bearing.coherence;
    ev.phase_std_deg = bearing.phase_std_deg;
    ev.snr_db = bearing.snr_db;
    ev.ambiguous_front_back = bearing.ambiguous_front_back;
    ev.rejection_reason = bearing.reason;
    ev.time_ms = packet.time_ms;
    model.record_df_event(ev);
    if (jsonl_out && *jsonl_out) (*jsonl_out) << ev.to_jsonl();

    if (bearing.valid && packet.telemetry_parsed) {
      model.set_bearing_detailed(telemetry.serial, static_cast<float>(bearing.broadside_deg),
                                 static_cast<float>(bearing.mirrored_deg),
                                 static_cast<float>(bearing.uncertainty_deg),
                                 static_cast<float>(bearing.coherence),
                                 bearing.ambiguous_front_back, global_start, global_end);
    }
  }

  if (count < 1024) return;
  EnergyBurstDetector detector(options.sample_rate, 10 * std::log10(noise));
  const auto rf_bursts = detector.feed(window.ch0.data(), window.ch0.size(), true);
  for (std::size_t idx = 0; idx < rf_bursts.size(); ++idx) {
    const auto& burst = rf_bursts[idx];
    if (burst.start_sample >= count) continue;
    const auto metrics = measure_rf(burst.samples, options.sample_rate, noise);
    model.record_rf_burst(metrics.rssi_dbfs, metrics.snr_db, metrics.bandwidth_hz,
                          metrics.spectral_center_hz, now_ms());

    const uint64_t b_start = window.start_sample + burst.start_sample;
    const uint64_t b_end = b_start + burst.samples.size();
    bool already_decoded = false;
    for (const auto& iv : decoded_intervals) {
      if (b_start < iv.second && b_end > iv.first) {
        already_decoded = true;
        break;
      }
    }
    if (already_decoded) continue;

    // DF-3: Undecoded RF event (incl. O4 candidates) receives an angle on its isolated interval,
    // retaining unknown_rf without inventing a drone identity or GPS position.
    if (burst.start_sample + burst.samples.size() > window.ch1.size() ||
        burst.samples.size() < 64) {
      continue;
    }

    // Check isolation from adjacent energy bursts (< 64 samples separation or truncated split).
    bool overlapping = burst.truncated;
    if (idx > 0) {
      const auto prev_end = rf_bursts[idx - 1].start_sample + rf_bursts[idx - 1].samples.size();
      if (burst.start_sample < prev_end + 64) overlapping = true;
    }
    if (idx + 1 < rf_bursts.size() &&
        rf_bursts[idx + 1].start_sample < burst.start_sample + burst.samples.size() + 64)
      overlapping = true;

    DfBearingEvent ev;
    ev.burst_id = ++next_burst_id;
    ev.start_sample = b_start;
    ev.end_sample = b_end;
    ev.session_id = cal_service.session_id();
    ev.config_epoch = cal_service.config_epoch();
    if (cal_service.calibration().measured_unix_ns > 0)
      ev.cal_id = cal_service.calibration().reference_method + "@" +
                  std::to_string(cal_service.calibration().measured_unix_ns);
    ev.classification = "unknown_rf";
    ev.time_ms = now_ms();

    if (overlapping) {
      ev.valid = false;
      ev.rejection_reason = "overlapping_or_multipath_burst";
      model.record_df_event(ev);
      if (jsonl_out && *jsonl_out) (*jsonl_out) << ev.to_jsonl();
      continue;
    }

    PairedBlock blk;
    blk.ch0.assign(window.ch0.begin() + burst.start_sample,
                   window.ch0.begin() + burst.start_sample + burst.samples.size());
    blk.ch1.assign(window.ch1.begin() + burst.start_sample,
                   window.ch1.begin() + burst.start_sample + burst.samples.size());
    blk.first_device_timestamp_ns =
        window.first_device_timestamp_ns +
        static_cast<int64_t>(std::llround(burst.start_sample * 1e9 / options.sample_rate));
    blk.start_sample = b_start;
    blk.sample_rate = options.sample_rate;
    blk.center_hz = center_hz;
    blk.session_id = cal_service.session_id();
    blk.config_epoch = cal_service.config_epoch();
    blk.rig_id = cal_service.rig_id();
    blk.channel_order = cal_service.calibration().channel_order;

    const int64_t burst_unix_ns = eval_unix_ns +
        static_cast<int64_t>(std::llround(b_start * 1e9 / options.sample_rate));
    const auto bearing = cal_service.estimate_block_bearing(blk, burst_unix_ns, noise, 6.0);
    ev.valid = bearing.valid;
    ev.broadside_deg = bearing.broadside_deg;
    ev.mirrored_deg = bearing.mirrored_deg;
    ev.uncertainty_deg = bearing.uncertainty_deg;
    ev.coherence = bearing.coherence;
    ev.phase_std_deg = bearing.phase_std_deg;
    ev.snr_db = bearing.snr_db;
    ev.ambiguous_front_back = bearing.ambiguous_front_back;
    ev.rejection_reason = bearing.reason;
    model.record_df_event(ev);
    if (jsonl_out && *jsonl_out) (*jsonl_out) << ev.to_jsonl();

    if (bearing.valid) {
      DroneTrack rf_track;
      rf_track.key = "unknown_rf";
      rf_track.serial = "";
      rf_track.type = "unknown_rf";
      rf_track.generation = "unverified";
      rf_track.rssi_dbfs = static_cast<float>(metrics.rssi_dbfs);
      rf_track.snr_db = static_cast<float>(metrics.snr_db);
      rf_track.decoded = false;
      rf_track.count = 1;
      rf_track.last_ms = ev.time_ms;
      model.upsert_track(rf_track);
      model.set_bearing_detailed("unknown_rf", static_cast<float>(bearing.broadside_deg),
                                 static_cast<float>(bearing.mirrored_deg),
                                 static_cast<float>(bearing.uncertainty_deg),
                                 static_cast<float>(bearing.coherence),
                                 bearing.ambiguous_front_back, b_start, b_end);
    }
  }
}

}  // namespace

WindowAssembler::WindowAssembler(std::size_t stride, std::size_t overlap)
    : stride_(stride), overlap_(overlap) {
  if (!stride || overlap < 10008 || overlap > stride) throw std::invalid_argument("invalid decoder window geometry");
}

void WindowAssembler::reset() { start_ += buffer_.size(); buffer_.clear(); }

std::vector<SampleWindow> WindowAssembler::feed(const cf32* samples, std::size_t count, bool final) {
  if (count) buffer_.insert(buffer_.end(), samples, samples + count);
  std::vector<SampleWindow> output;
  while (buffer_.size() >= stride_ + overlap_) {
    output.push_back({std::vector<cf32>(buffer_.begin(), buffer_.begin() + stride_ + overlap_), stride_, start_});
    buffer_.erase(buffer_.begin(), buffer_.begin() + stride_);
    start_ += stride_;
  }
  if (final && !buffer_.empty()) {
    const auto size = buffer_.size();
    output.push_back({std::move(buffer_), size, start_});
    buffer_.clear();
    start_ += size;
  }
  return output;
}

PairedWindowAssembler::PairedWindowAssembler(std::size_t stride, std::size_t overlap)
    : stride_(stride), overlap_(overlap) {
  if (!stride || overlap < 10008 || overlap > stride)
    throw std::invalid_argument("invalid paired decoder window geometry");
}

void PairedWindowAssembler::reset() {
  start_ += buf0_.size();
  buf0_.clear();
  buf1_.clear();
  gap_pending_ = true;
}

std::vector<PairedSampleWindow> PairedWindowAssembler::feed(const cf32* ch0, const cf32* ch1,
                                                            std::size_t count,
                                                            int64_t base_device_ns,
                                                            double sample_rate, bool final) {
  if (count) {
    if (!ch0 || !ch1) throw std::invalid_argument("null channel buffer in PairedWindowAssembler");
    buf0_.insert(buf0_.end(), ch0, ch0 + count);
    buf1_.insert(buf1_.end(), ch1, ch1 + count);
  }
  std::vector<PairedSampleWindow> output;
  while (buf0_.size() >= stride_ + overlap_) {
    const int64_t ts =
        base_device_ns + static_cast<int64_t>(std::llround(start_ * 1e9 / sample_rate));
    output.push_back({std::vector<cf32>(buf0_.begin(), buf0_.begin() + stride_ + overlap_),
                      std::vector<cf32>(buf1_.begin(), buf1_.begin() + stride_ + overlap_),
                      stride_, start_, ts, gap_pending_});
    gap_pending_ = false;
    buf0_.erase(buf0_.begin(), buf0_.begin() + stride_);
    buf1_.erase(buf1_.begin(), buf1_.begin() + stride_);
    start_ += stride_;
  }
  if (final && !buf0_.empty()) {
    const auto size = buf0_.size();
    const int64_t ts =
        base_device_ns + static_cast<int64_t>(std::llround(start_ * 1e9 / sample_rate));
    output.push_back({std::move(buf0_), std::move(buf1_), size, start_, ts, gap_pending_});
    gap_pending_ = false;
    buf0_.clear();
    buf1_.clear();
    start_ += size;
  }
  return output;
}

void BoundedPairedQueue::push(PairedSampleWindow window) {
  if (queue_.size() >= capacity_) {
    queue_.pop_front();
    ++dropped_pairs_;
    if (!queue_.empty()) queue_.front().after_drop_or_gap = true;
    else drop_flag_ = true;
  }
  if (drop_flag_) {
    window.after_drop_or_gap = true;
    drop_flag_ = false;
  }
  queue_.push_back(std::move(window));
}

bool BoundedPairedQueue::pop(PairedSampleWindow& out) {
  if (queue_.empty()) return false;
  out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

void replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop) {
  if (!config.pair_manifest.empty() || !config.replay_ch1.empty()) {
    paired_replay_worker(model, config, stop);
    return;
  }
  UsrpStatus status;
  status.state = UsrpStatus::State::Connecting;
  status.label = "Replay: " + config.replay;
  model.set_status(status);
  try {
    FileSource source(config.replay, config.fmt, config.rate);
    status.rate_hz = source.sample_rate();
    status.center_hz = source.center_freq();
    dji::DecodeOptions options;
    options.sample_rate = source.sample_rate();
    options.auto_center = config.auto_center;
    options.freq_offset = config.freq_offset;
    if (!std::isfinite(options.sample_rate) || options.sample_rate < 10e6 || options.sample_rate > 250e6)
      throw std::invalid_argument("replay rate must be between 10 and 250 MSPS");
    const auto overlap = static_cast<std::size_t>(std::ceil(12000 * options.sample_rate / 15.36e6));
    WindowAssembler assembler(1u << 20, std::max<std::size_t>(10008, overlap));
    const uint64_t limit = config.secs > 0 ? static_cast<uint64_t>(config.secs * options.sample_rate) : UINT64_MAX;
    std::vector<cf32> block(65536);
    int64_t timestamp = 0;
    uint64_t received = 0;
    status.state = UsrpStatus::State::Connected;
    status.detail = "Offline replay running";
    model.set_status(status);
    while (!stop.load() && received < limit) {
      const auto count = source.recv(block.data(), std::min<uint64_t>(block.size(), limit - received), timestamp);
      if (!count) break;
      received += count;
      for (const auto& window : assembler.feed(block.data(), count)) {
        if (stop.load()) break;
        process_window(model, window, options);
      }
    }
    if (!stop.load())
      for (const auto& window : assembler.feed(nullptr, 0, true)) process_window(model, window, options);
    status.samples_received = received;
    status.state = UsrpStatus::State::Disconnected;
    status.detail = stop.load() ? "Replay stopped" : "Replay complete (not live reception)";
    model.set_status(status);
  } catch (const std::exception& error) {
    status.state = UsrpStatus::State::Error;
    status.detail = error.what();
    model.set_status(status);
  }
}

void paired_replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop) {
  UsrpStatus status;
  status.state = UsrpStatus::State::Connecting;
  status.label = !config.pair_manifest.empty() ? ("Paired Replay: " + config.pair_manifest)
                                               : ("Paired Replay: " + config.replay);
  model.set_status(status);

  DfCalibrationService cal_service;
  BoundedPairedQueue paired_queue(3);
  try {
    if (config.pair_manifest.empty())
      throw std::invalid_argument("paired DF requires a verified --pair-manifest; use df_bearing --aligned only for unverified diagnostics");
    if (!config.df_cal.empty() && config.df_cal_samples)
      throw std::invalid_argument("choose loaded calibration or a reference prefix, not both");
    std::string ch0_path = config.replay;
    std::string ch1_path = config.replay_ch1;
    std::string session_id = "paired_replay_session";
    uint64_t config_epoch = 1;
    std::string rig_id = "default_rig";
    std::array<std::size_t, 2> channel_order = {0, 1};
    double spacing_m = config.df_spacing_m;
    double orientation_deg = 0.0;
    int64_t base_device_ns = 1000000000LL;
    int64_t capture_unix_ns = now_ns();
    double gain_db = config.gain;

    if (!config.pair_manifest.empty()) {
      const auto manifest = PairManifest::load(config.pair_manifest);
      const auto parent = std::filesystem::path(config.pair_manifest).parent_path();
      if (ch0_path.empty()) {
        const auto p0 = std::filesystem::path(manifest.ch0_path);
        ch0_path = (p0.is_absolute() || parent.empty()) ? p0.string() : (parent / p0).string();
      }
      if (ch1_path.empty()) {
        const auto p1 = std::filesystem::path(manifest.ch1_path);
        ch1_path = (p1.is_absolute() || parent.empty()) ? p1.string() : (parent / p1).string();
      }
      std::string err;
      if (!manifest.validate_files(ch0_path, ch1_path, &err))
        throw std::runtime_error("paired manifest validation failed: " + err);
      session_id = manifest.session_id;
      config_epoch = manifest.config_epoch;
      rig_id = manifest.rig_id;
      channel_order = manifest.channel_order;
      spacing_m = manifest.spacing_m;
      orientation_deg = manifest.orientation_deg;
      base_device_ns = manifest.first_device_timestamp_ns;
      capture_unix_ns = manifest.capture_host_unix_ns > 0 ? manifest.capture_host_unix_ns : now_ns();
      gain_db = manifest.gain_ch0_db;
      config.rate = manifest.sample_rate;
      config.center_freq = manifest.center_hz;
      config.fmt = SampleFormat::cf32;
    }

    FileSource src0(ch0_path, config.fmt, config.rate, config.center_freq);
    FileSource src1(ch1_path, config.fmt, config.rate, config.center_freq);
    if (src0.sample_rate() != src1.sample_rate() || src0.center_freq() != src1.center_freq())
      throw std::runtime_error("paired channel sample rate or center frequency mismatch");

    const double rate = src0.sample_rate();
    const double center_hz = src0.center_freq() > 0 ? src0.center_freq() : config.center_freq;
    if (!std::isfinite(rate) || rate < 10e6 || rate > 250e6)
      throw std::invalid_argument("paired replay rate must be between 10 and 250 MSPS");

    status.rate_hz = rate;
    status.center_hz = center_hz;
    status.gain_db = gain_db;
    status.state = UsrpStatus::State::Connected;
    status.detail = "Paired DF offline replay running";
    model.set_status(status);

    if (!cal_service.connect(session_id, config_epoch, rig_id, center_hz, rate, gain_db,
                             channel_order, true, true)) {
      sync_df_status(model, cal_service, capture_unix_ns, 0, orientation_deg);
      throw std::runtime_error(cal_service.reason());
    }
    sync_df_status(model, cal_service, capture_unix_ns, 0, orientation_deg);

    int64_t eval_unix_ns = capture_unix_ns;
    if (!config.df_cal.empty()) {
      const auto loaded_cal = PhaseCal::load(config.df_cal);
      if (std::abs(loaded_cal.spacing_m - spacing_m) > 1e-9)
        throw std::runtime_error("calibration spacing does not match manifest rig geometry");
      if (!cal_service.adopt_same_session_calibration(loaded_cal, eval_unix_ns)) {
        sync_df_status(model, cal_service, eval_unix_ns, 0, orientation_deg);
        throw std::runtime_error(cal_service.reason());
      }
      sync_df_status(model, cal_service, eval_unix_ns, 0, orientation_deg);
    }

    uint64_t received = 0;
    if (config.df_cal_samples > 0) {
      if (config.df_cal_samples < 32 || config.df_cal_samples > 1000000)
        throw std::invalid_argument("df-cal-samples must be in [32, 1000000]");
      cal_service.begin_calibration(config.df_ref);
      sync_df_status(model, cal_service, eval_unix_ns, 0, orientation_deg);
      PairedBlock ref_block;
      ref_block.ch0.resize(config.df_cal_samples);
      ref_block.ch1.resize(config.df_cal_samples);
      int64_t t0 = 0, t1 = 0;
      const auto g0 = src0.recv(ref_block.ch0.data(), config.df_cal_samples, t0);
      const auto g1 = src1.recv(ref_block.ch1.data(), config.df_cal_samples, t1);
      if (g0 != config.df_cal_samples || g1 != config.df_cal_samples)
        throw std::runtime_error("paired input shorter than requested df-cal-samples window");
      ref_block.first_device_timestamp_ns = base_device_ns;
      ref_block.start_sample = 0;
      ref_block.sample_rate = rate;
      ref_block.center_hz = center_hz;
      ref_block.session_id = session_id;
      ref_block.config_epoch = config_epoch;
      ref_block.rig_id = rig_id;
      ref_block.channel_order = channel_order;
      if (!cal_service.complete_calibration(ref_block, spacing_m, capture_unix_ns, 3600.0, 10e6)) {
        sync_df_status(model, cal_service, eval_unix_ns, 0, orientation_deg);
        throw std::runtime_error(cal_service.reason());
      }
      received += config.df_cal_samples;
      sync_df_status(model, cal_service, eval_unix_ns, 0, orientation_deg);
    }

    std::ofstream jsonl_file;
    if (!config.df_jsonl.empty()) {
      jsonl_file.open(config.df_jsonl, std::ios::binary | std::ios::trunc);
      if (!jsonl_file) throw std::runtime_error("cannot open df-jsonl output: " + config.df_jsonl);
    }

    dji::DecodeOptions options;
    options.sample_rate = rate;
    options.auto_center = config.auto_center;
    options.freq_offset = config.freq_offset;
    const auto overlap = static_cast<std::size_t>(std::ceil(12000 * rate / 15.36e6));
    PairedWindowAssembler assembler(1u << 20, std::max<std::size_t>(10008, overlap));
    const int64_t post_cal_device_ns =
        base_device_ns + static_cast<int64_t>(std::llround(received * 1e9 / rate));
    const uint64_t limit =
        config.secs > 0 ? static_cast<uint64_t>(config.secs * rate) : UINT64_MAX;
    std::vector<cf32> b0(65536), b1(65536);
    int64_t ts0 = 0, ts1 = 0;
    uint64_t next_burst_id = 0;

    while (!stop.load() && received < limit) {
      const auto req = std::min<uint64_t>(b0.size(), limit - received);
      const auto g0 = src0.recv(b0.data(), req, ts0);
      const auto g1 = src1.recv(b1.data(), req, ts1);
      if (g0 != g1) {
        cal_service.on_gap_or_overflow("unequal EOF across paired channels");
        sync_df_status(model, cal_service, eval_unix_ns, paired_queue.dropped_pairs(), orientation_deg);
        throw std::runtime_error("paired channels returned unequal sample counts");
      }
      if (!g0) break;
      received += g0;
      for (auto& win : assembler.feed(b0.data(), b1.data(), g0, post_cal_device_ns, rate, false)) {
        win.start_sample += config.df_cal_samples;
        paired_queue.push(std::move(win));
      }
      PairedSampleWindow ready_win;
      while (!stop.load() && paired_queue.pop(ready_win)) {
        process_paired_window(model, ready_win, options, center_hz, cal_service, eval_unix_ns,
                              next_burst_id, jsonl_file.is_open() ? &jsonl_file : nullptr);
      }
    }
    if (!stop.load()) {
      for (auto& win : assembler.feed(nullptr, nullptr, 0, post_cal_device_ns, rate, true)) {
        win.start_sample += config.df_cal_samples;
        paired_queue.push(std::move(win));
      }
      PairedSampleWindow ready_win;
      while (!stop.load() && paired_queue.pop(ready_win)) {
        process_paired_window(model, ready_win, options, center_hz, cal_service, eval_unix_ns,
                              next_burst_id, jsonl_file.is_open() ? &jsonl_file : nullptr);
      }
    }
    cal_service.on_disconnect(stop.load() ? "Paired replay stopped" : "Paired replay complete; historical bearings remain in event log");
    sync_df_status(model, cal_service, eval_unix_ns, paired_queue.dropped_pairs(), orientation_deg);
    if (jsonl_file.is_open()) {
      jsonl_file << model.df_summary_jsonl();
      jsonl_file.close();
      if (!jsonl_file) throw std::runtime_error("failed writing DF JSONL output");
    }
    status.samples_received = received;
    status.dropped_windows = paired_queue.dropped_pairs();
    status.state = UsrpStatus::State::Disconnected;
    status.detail = stop.load() ? "Paired replay stopped" : "Paired replay complete (not live reception)";
    model.set_status(status);
  } catch (const std::exception& error) {
    status.state = UsrpStatus::State::Error;
    status.detail = error.what();
    model.set_status(status);
    if (model.df_status().state == DfUiState::Disconnected ||
        model.df_status().state == DfUiState::Valid) {
      model.invalidate_bearings(DfUiState::Error, error.what());
    }
  }
}

void live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop) {
  UsrpStatus status;
  status.state = UsrpStatus::State::Connecting;
  status.label = "X310 live";
  model.set_status(status);
#ifdef DRONEID_HAVE_UHD
  try {
    UsrpParams parameters;
    parameters.device_args = config.device;
    parameters.center_freq = config.center_freq;
    parameters.gain_db = config.gain;
    parameters.antenna = config.antenna;
    UsrpSource source(parameters);
    if (std::abs(source.sample_rate() - 15.36e6) > 1) throw std::runtime_error("unexpected live sample rate");
    status.state = UsrpStatus::State::Connected;
    status.label = "X310 " + source.serial();
    status.rate_hz = source.sample_rate();
    status.center_hz = source.center_freq();
    status.gain_db = config.gain;
    status.ref_locked = source.ref_locked();
    model.set_status(status);
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<SampleWindow> queue;
    bool finished = false;
    std::string dsp_error;
    std::thread processor([&] {
      for (;;) {
        SampleWindow window;
        {
          std::unique_lock<std::mutex> lock(mutex);
          ready.wait(lock, [&] { return finished || !queue.empty(); });
          if (stop.load() || (finished && queue.empty())) break;
          window = std::move(queue.front());
          queue.pop_front();
        }
        try { process_window(model, window, {}); }
        catch (const std::exception& error) {
          std::lock_guard<std::mutex> lock(mutex);
          dsp_error = error.what();
          break;
        }
      }
    });
    auto finish = [&] {
      { std::lock_guard<std::mutex> lock(mutex); finished = true; }
      ready.notify_all();
      processor.join();
    };
    auto enqueue = [&](std::vector<SampleWindow> windows) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!dsp_error.empty()) throw std::runtime_error("DSP: " + dsp_error);
      for (auto& window : windows) {
        if (queue.size() == 3) { queue.pop_front(); ++status.dropped_windows; }
        queue.push_back(std::move(window));
      }
      ready.notify_one();
    };
    try {
      WindowAssembler assembler;
      std::vector<cf32> block(65536);
      int64_t timestamp = 0, expected = 0;
      uint64_t previous_overflows = source.overflow_count();
      const uint64_t limit = config.secs > 0 ? static_cast<uint64_t>(config.secs * source.sample_rate()) : UINT64_MAX;
      int timeouts = 0;
      while (!stop.load() && status.samples_received < limit) {
        const auto count = source.recv(block.data(), std::min<uint64_t>(block.size(), limit - status.samples_received), timestamp);
        if (!count) {
          if (++timeouts >= 5) throw std::runtime_error("five consecutive RX timeouts");
          continue;
        }
        timeouts = 0;
        const auto overflows = source.overflow_count();
        if ((status.samples_received && std::abs(timestamp - expected) > 100) || overflows != previous_overflows) {
          assembler.reset();
          ++status.discontinuities;
        }
        expected = timestamp + static_cast<int64_t>(std::llround(count * 1e9 / source.sample_rate()));
        previous_overflows = overflows;
        status.samples_received += count;
        status.overflows = overflows;
        enqueue(assembler.feed(block.data(), count));
        status.detail = "RX + bounded DSP queue; gaps/drops are reported";
        model.set_status(status);
      }
      if (!stop.load()) enqueue(assembler.feed(nullptr, 0, true));
      source.stop();
      finish();
      if (!dsp_error.empty()) throw std::runtime_error("DSP: " + dsp_error);
      status.detail = "Live stream stopped";
      status.state = UsrpStatus::State::Disconnected;
      model.set_status(status);
    } catch (...) {
      if (processor.joinable()) finish();
      throw;
    }
  } catch (const std::exception& error) {
    status.state = UsrpStatus::State::Error;
    status.detail = error.what();
    model.set_status(status);
  }
#else
  (void)config;
  (void)stop;
  status.state = UsrpStatus::State::Error;
  status.detail = "Rebuild with UHD support for live reception";
  model.set_status(status);
#endif
}

void df_live_worker(UiModel& model, SourceConfig, std::atomic<bool>&) {
  UsrpStatus status;
  status.state = UsrpStatus::State::Error;
  status.label = "DF calibration required";
  status.detail = "Live DF disabled: calibration session and phase repeatability are unverified. Use same-session paired IQ tooling.";
  model.set_status(status);
  model.invalidate_bearings(
      DfUiState::Error,
      "Live DF disabled until DF-5 bench profile approval; use paired replay or same-session paired IQ tooling");
}

}  // namespace droneid::ui
