#include "droneid/aoa.h"
#include "droneid/config.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace droneid {
namespace {

bool parse_strict_double(const std::string& text, double& out) {
  if (text.empty()) return false;
  std::size_t used = 0;
  try {
    out = std::stod(text, &used);
  } catch (...) {
    return false;
  }
  return used == text.size() && std::isfinite(out);
}

bool parse_strict_i64(const std::string& text, int64_t& out) {
  if (text.empty()) return false;
  std::size_t used = 0;
  try {
    out = std::stoll(text, &used);
  } catch (...) {
    return false;
  }
  return used == text.size();
}

bool parse_strict_u64(const std::string& text, uint64_t& out) {
  if (text.empty() || text.front() == '-') return false;
  std::size_t used = 0;
  try {
    out = std::stoull(text, &used);
  } catch (...) {
    return false;
  }
  return used == text.size();
}

struct CrossStats {
  bool finite = true;
  double first_power = 0;
  double second_power = 0;
  double clipped_fraction = 0;
  double coherence = 0;
  double mean_phase_rad = 0;
  double phase_std_rad = 0;
  double outside_passband_fraction = 0;
};

double outside_passband_fraction(const std::vector<cf32>& samples, double rate, double passband) {
  size_t fft_size = 1;
  while (fft_size * 2 <= std::min<size_t>(1024, samples.size())) fft_size *= 2;
  const double pi = std::acos(-1.0);
  double total_power = 0, outside_power = 0;
  for (size_t start = 0; start < samples.size(); start += fft_size / 2) {
    if (start + fft_size > samples.size()) start = samples.size() - fft_size;
    std::vector<std::complex<double>> spectrum(fft_size);
    for (size_t index = 0; index < fft_size; ++index)
      spectrum[index] = std::complex<double>(samples[start + index]) *
                        (0.5 - 0.5 * std::cos(2 * pi * index / (fft_size - 1)));
    for (size_t index = 1, reversed = 0; index < fft_size; ++index) {
      size_t bit = fft_size >> 1;
      for (; reversed & bit; bit >>= 1) reversed ^= bit;
      reversed ^= bit;
      if (index < reversed) std::swap(spectrum[index], spectrum[reversed]);
    }
    for (size_t length = 2; length <= fft_size; length *= 2) {
      const auto rotation = std::polar(1.0, -2 * pi / length);
      for (size_t base = 0; base < fft_size; base += length) {
        std::complex<double> phase(1, 0);
        for (size_t offset = 0; offset < length / 2; ++offset) {
          const auto upper = spectrum[base + offset];
          const auto lower = phase * spectrum[base + offset + length / 2];
          spectrum[base + offset] = upper + lower;
          spectrum[base + offset + length / 2] = upper - lower;
          phase *= rotation;
        }
      }
    }
    for (size_t index = 0; index < fft_size; ++index) {
      const double frequency = std::min(index, fft_size - index) * rate / fft_size;
      const double power = std::norm(spectrum[index]);
      total_power += power;
      if (frequency > passband / 2 + rate / fft_size) outside_power += power;
    }
    if (start + fft_size >= samples.size()) break;
  }
  return total_power > 0 ? outside_power / total_power : 1;
}

CrossStats analyze_pair(const std::vector<cf32>& first, const std::vector<cf32>& second,
                        double sample_rate, double passband_hz = 0) {
  CrossStats stats;
  const std::size_t n = first.size();
  if (n == 0 || n != second.size()) {
    stats.finite = false;
    return stats;
  }
  std::complex<double> total_cross(0, 0);
  std::size_t clipped = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::complex<double> l = first[i], r = second[i];
    if (!std::isfinite(l.real()) || !std::isfinite(l.imag()) ||
        !std::isfinite(r.real()) || !std::isfinite(r.imag())) {
      stats.finite = false;
      return stats;
    }
    if (std::abs(first[i].real()) >= 0.999f || std::abs(first[i].imag()) >= 0.999f ||
        std::abs(second[i].real()) >= 0.999f || std::abs(second[i].imag()) >= 0.999f) {
      ++clipped;
    }
    total_cross += std::conj(l) * r;
    stats.first_power += std::norm(l);
    stats.second_power += std::norm(r);
  }
  stats.clipped_fraction = static_cast<double>(clipped) / static_cast<double>(n);
  if (stats.first_power <= 0 || stats.second_power <= 0) return stats;

  const double pi = std::acos(-1.0);
  stats.coherence = std::clamp(
      std::abs(total_cross) / std::sqrt(stats.first_power * stats.second_power), 0.0, 1.0);
  stats.mean_phase_rad = std::arg(total_cross);

  // Sub-window circular phase dispersion across 4 temporal segments.
  constexpr std::size_t kBlocks = 4;
  if (n >= kBlocks * 8) {
    const std::size_t blk = n / kBlocks;
    double sq_sum = 0;
    std::size_t valid_blocks = 0;
    for (std::size_t b = 0; b < kBlocks; ++b) {
      std::complex<double> sub_cross(0, 0);
      double p0 = 0, p1 = 0;
      for (std::size_t i = b * blk; i < (b + 1) * blk; ++i) {
        const std::complex<double> l = first[i], r = second[i];
        sub_cross += std::conj(l) * r;
        p0 += std::norm(l);
        p1 += std::norm(r);
      }
      if (p0 > 0 && p1 > 0 && std::abs(sub_cross) > 0) {
        const double diff = std::remainder(std::arg(sub_cross) - stats.mean_phase_rad, 2.0 * pi);
        sq_sum += diff * diff;
        ++valid_blocks;
      }
    }
    if (valid_blocks > 0) stats.phase_std_rad = std::sqrt(sq_sum / static_cast<double>(valid_blocks));
  }

  if (n >= 32 && passband_hz > 0)
    stats.outside_passband_fraction = std::max(
        outside_passband_fraction(first, sample_rate, passband_hz),
        outside_passband_fraction(second, sample_rate, passband_hz));
  return stats;
}

}  // namespace

uint64_t fnv1a64_update(uint64_t hash, const void* data, std::size_t bytes) {
  const auto* ptr = static_cast<const uint8_t*>(data);
  for (std::size_t i = 0; i < bytes; ++i) {
    hash ^= static_cast<uint64_t>(ptr[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string fnv1a64_hex(uint64_t hash) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
  return std::string(buf);
}

std::string fnv1a64_file(const std::string& path, uint64_t* byte_count) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open file for checksum: " + path);
  uint64_t hash = 14695981039346656037ULL;
  uint64_t total = 0;
  char buffer[65536];
  while (in) {
    in.read(buffer, sizeof(buffer));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (!got) break;
    hash = fnv1a64_update(hash, buffer, got);
    total += got;
  }
  if (byte_count) *byte_count = total;
  return fnv1a64_hex(hash);
}

SidecarMeta SidecarMeta::load(const std::string& iq_path) {
  SidecarMeta meta;
  const std::string sidecar_path = iq_path + ".meta";
  if (!std::filesystem::exists(sidecar_path)) return meta;
  const auto cfg = Config::load(sidecar_path);
  meta.present = true;
  if (!cfg.get("format").empty()) {
    if (!parse_format(cfg.get("format"), meta.format))
      throw std::invalid_argument("invalid sidecar format in " + sidecar_path);
  }
  auto opt_double = [&](const char* key, double& target) {
    const auto v = cfg.get(key);
    if (!v.empty() && !parse_strict_double(v, target))
      throw std::invalid_argument(std::string("invalid sidecar field ") + key);
  };
  auto opt_i64 = [&](const char* key, int64_t& target) {
    const auto v = cfg.get(key);
    if (!v.empty() && !parse_strict_i64(v, target))
      throw std::invalid_argument(std::string("invalid sidecar field ") + key);
  };
  auto opt_u64 = [&](const char* key, uint64_t& target) {
    const auto v = cfg.get(key);
    if (!v.empty() && !parse_strict_u64(v, target))
      throw std::invalid_argument(std::string("invalid sidecar field ") + key);
  };
  opt_double("sample_rate", meta.sample_rate);
  opt_double("center_freq", meta.center_freq);
  int64_t ch = -1;
  opt_i64("channel", ch);
  meta.channel = static_cast<int>(ch);
  opt_i64("first_device_timestamp_ns", meta.first_device_timestamp_ns);
  opt_i64("capture_host_unix_ns", meta.capture_host_unix_ns);
  opt_u64("samples", meta.samples);
  opt_u64("overflows", meta.overflows);
  const auto pc = cfg.get("phase_calibrated");
  meta.phase_calibrated = (pc == "true" || pc == "1");
  meta.session_id = cfg.get("session_id");
  opt_u64("config_epoch", meta.config_epoch);
  meta.rig_id = cfg.get("rig_id");
  return meta;
}

bool PairedBlock::validate(std::string* reason) const {
  auto fail = [&](const char* msg) {
    if (reason) *reason = msg;
    return false;
  };
  if (ch0.empty() || ch0.size() != ch1.size() || ch0.size() < 32)
    return fail("paired block channels must be non-empty, >=32 samples, and equal length");
  if (first_device_timestamp_ns <= 0)
    return fail("missing or non-positive device timestamp in paired block");
  if (session_id.empty() || config_epoch == 0 || rig_id.empty())
    return fail("missing session, epoch or rig provenance in paired block");
  if (!std::isfinite(sample_rate) || sample_rate <= 0 || !std::isfinite(center_hz) || center_hz <= 0)
    return fail("invalid sample rate or center frequency in paired block");
  if (channel_order[0] == channel_order[1])
    return fail("paired block channels must be distinct");
  if (has_gap || has_overflow)
    return fail("paired block contains sample gap or overflow");
  return true;
}

std::string PairManifest::serialize() const {
  std::ostringstream os;
  os << std::setprecision(17);
  os << "schema_version = " << schema_version << "\n"
     << "complete = " << (complete ? "true" : "false") << "\n"
     << "status_reason = \"" << status_reason << "\"\n"
     << "session_id = \"" << session_id << "\"\n"
     << "config_epoch = " << config_epoch << "\n"
     << "rig_id = \"" << rig_id << "\"\n"
     << "cable_id = \"" << cable_id << "\"\n"
     << "radio_serial = \"" << radio_serial << "\"\n"
     << "dboard_ch0 = \"" << dboard_ch0 << "\"\n"
     << "dboard_ch1 = \"" << dboard_ch1 << "\"\n"
     << "uhd_version = \"" << uhd_version << "\"\n"
     << "fpga_version = \"" << fpga_version << "\"\n"
     << "clock_source = \"" << clock_source << "\"\n"
     << "time_source = \"" << time_source << "\"\n"
     << "hardware_observed = " << (hardware_observed ? "true" : "false") << "\n"
     << "lo_locked_ch0 = " << (lo_locked_ch0 ? "true" : "false") << "\n"
     << "lo_locked_ch1 = " << (lo_locked_ch1 ? "true" : "false") << "\n"
     << "ref_locked = " << (ref_locked ? "true" : "false") << "\n"
     << "master_clock_rate = " << master_clock_rate << "\n"
     << "wire_format = \"" << wire_format << "\"\n"
     << "rf_tune_ch0_hz = " << rf_tune_hz[0] << "\n"
     << "rf_tune_ch1_hz = " << rf_tune_hz[1] << "\n"
     << "dsp_tune_ch0_hz = " << dsp_tune_hz[0] << "\n"
     << "dsp_tune_ch1_hz = " << dsp_tune_hz[1] << "\n"
     << "sample_rate = " << sample_rate << "\n"
     << "center_hz = " << center_hz << "\n"
     << "gain_ch0_db = " << gain_ch0_db << "\n"
     << "gain_ch1_db = " << gain_ch1_db << "\n"
     << "bandwidth_hz = " << bandwidth_hz << "\n"
     << "antenna_ch0 = \"" << antenna_ch0 << "\"\n"
     << "antenna_ch1 = \"" << antenna_ch1 << "\"\n"
     << "channel_0 = " << channel_order[0] << "\n"
     << "channel_1 = " << channel_order[1] << "\n"
     << "spacing_m = " << spacing_m << "\n"
     << "orientation_deg = " << orientation_deg << "\n"
     << "first_device_timestamp_ns = " << first_device_timestamp_ns << "\n"
     << "capture_host_unix_ns = " << capture_host_unix_ns << "\n"
     << "sample_count = " << sample_count << "\n"
     << "overflows = " << overflows << "\n"
     << "discontinuities = " << discontinuities << "\n"
     << "ch0_path = \"" << ch0_path << "\"\n"
     << "ch1_path = \"" << ch1_path << "\"\n"
     << "ch0_fnv1a64 = \"" << ch0_fnv1a64 << "\"\n"
     << "ch1_fnv1a64 = \"" << ch1_fnv1a64 << "\"\n";
  return os.str();
}

void PairManifest::save(const std::string& path) const {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open manifest for writing: " + tmp);
    out << serialize();
    out.close();
    if (!out) throw std::runtime_error("failed writing manifest: " + tmp);
  }
  std::filesystem::rename(tmp, path);
}

PairManifest PairManifest::load(const std::string& path) {
  const auto cfg = Config::load(path);
  PairManifest m;
  auto req_double = [&](const char* k) {
    double v = 0;
    if (!parse_strict_double(cfg.get(k), v))
      throw std::invalid_argument(std::string("invalid manifest numeric field: ") + k);
    return v;
  };
  auto req_i64 = [&](const char* k) {
    int64_t v = 0;
    if (!parse_strict_i64(cfg.get(k), v))
      throw std::invalid_argument(std::string("invalid manifest int64 field: ") + k);
    return v;
  };
  auto req_u64 = [&](const char* k) {
    uint64_t v = 0;
    if (!parse_strict_u64(cfg.get(k), v))
      throw std::invalid_argument(std::string("invalid manifest uint64 field: ") + k);
    return v;
  };
  m.schema_version = static_cast<int>(req_i64("schema_version"));
  if (m.schema_version != 1) throw std::invalid_argument("unsupported manifest schema_version");
  m.complete = (cfg.get("complete") == "true" || cfg.get("complete") == "1");
  m.status_reason = cfg.get("status_reason", "");
  m.session_id = cfg.get("session_id");
  m.config_epoch = req_u64("config_epoch");
  m.rig_id = cfg.get("rig_id", "default_rig");
  m.cable_id = cfg.get("cable_id", "default_cables");
  m.radio_serial = cfg.get("radio_serial", "?");
  m.dboard_ch0 = cfg.get("dboard_ch0", "UBX");
  m.dboard_ch1 = cfg.get("dboard_ch1", "UBX");
  m.uhd_version = cfg.get("uhd_version", "unknown");
  m.fpga_version = cfg.get("fpga_version", "unknown");
  m.clock_source = cfg.get("clock_source", "internal");
  m.time_source = cfg.get("time_source", "internal");
  m.hardware_observed = cfg.get("hardware_observed") == "true";
  if (m.hardware_observed) {
    m.lo_locked_ch0 = cfg.get("lo_locked_ch0") == "true";
    m.lo_locked_ch1 = cfg.get("lo_locked_ch1") == "true";
    m.ref_locked = cfg.get("ref_locked") == "true";
    m.master_clock_rate = req_double("master_clock_rate");
    m.wire_format = cfg.get("wire_format");
    m.rf_tune_hz = {req_double("rf_tune_ch0_hz"), req_double("rf_tune_ch1_hz")};
    m.dsp_tune_hz = {req_double("dsp_tune_ch0_hz"), req_double("dsp_tune_ch1_hz")};
  }
  m.sample_rate = req_double("sample_rate");
  m.center_hz = req_double("center_hz");
  m.gain_ch0_db = req_double("gain_ch0_db");
  m.gain_ch1_db = req_double("gain_ch1_db");
  if (!cfg.get("bandwidth_hz").empty()) m.bandwidth_hz = req_double("bandwidth_hz");
  m.antenna_ch0 = cfg.get("antenna_ch0", "RX2");
  m.antenna_ch1 = cfg.get("antenna_ch1", "RX2");
  m.channel_order[0] = static_cast<std::size_t>(req_u64("channel_0"));
  m.channel_order[1] = static_cast<std::size_t>(req_u64("channel_1"));
  m.spacing_m = req_double("spacing_m");
  if (!cfg.get("orientation_deg").empty()) m.orientation_deg = req_double("orientation_deg");
  m.first_device_timestamp_ns = req_i64("first_device_timestamp_ns");
  m.capture_host_unix_ns = req_i64("capture_host_unix_ns");
  m.sample_count = req_u64("sample_count");
  m.overflows = req_u64("overflows");
  if (!cfg.get("discontinuities").empty()) m.discontinuities = req_u64("discontinuities");
  m.ch0_path = cfg.get("ch0_path");
  m.ch1_path = cfg.get("ch1_path");
  m.ch0_fnv1a64 = cfg.get("ch0_fnv1a64");
  m.ch1_fnv1a64 = cfg.get("ch1_fnv1a64");
  return m;
}

bool PairManifest::validate_files(const std::string& resolved_ch0, const std::string& resolved_ch1,
                                  std::string* reason) const {
  auto fail = [&](const std::string& msg) {
    if (reason) *reason = msg;
    return false;
  };
  if (!complete) return fail("pair manifest is marked incomplete: " + status_reason);
  if (hardware_observed && (!lo_locked_ch0 || !lo_locked_ch1 || !ref_locked ||
      !std::isfinite(master_clock_rate) || master_clock_rate <= 0 ||
      (wire_format != "sc16" && wire_format != "sc8")))
    return fail("observed hardware has invalid clocks, wire format or unlocked reference/LO");
  if (schema_version != 1) return fail("unsupported manifest schema_version");
  if (session_id.empty() || config_epoch == 0) return fail("pair manifest missing session_id or config_epoch");
  if (rig_id.empty() || cable_id.empty() || capture_host_unix_ns <= 0)
    return fail("pair manifest missing rig, cable or capture time provenance");
  const auto valid_hash = [](const std::string& hash) {
    return hash.size() == 16 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
  };
  if (!valid_hash(ch0_fnv1a64) || !valid_hash(ch1_fnv1a64))
    return fail("pair manifest requires both FNV-1a checksums");
  if (first_device_timestamp_ns <= 0)
    return fail("pair manifest missing valid positive first_device_timestamp_ns");
  if (overflows > 0 || discontinuities > 0)
    return fail("pair manifest reports overflows or discontinuities");
  if (channel_order[0] == channel_order[1])
    return fail("pair manifest channel_0 and channel_1 must be distinct");
  if (sample_count < 32 || !std::isfinite(sample_rate) || sample_rate <= 0 ||
      !std::isfinite(center_hz) || center_hz <= 0)
    return fail("pair manifest has invalid sample_count, sample_rate, or center_hz");
  if (!std::isfinite(spacing_m) || spacing_m <= 0 || spacing_m > 299792458.0 / center_hz / 2 ||
      !std::isfinite(orientation_deg) || !std::isfinite(bandwidth_hz) || bandwidth_hz <= 0)
    return fail("pair manifest has invalid rig geometry or bandwidth");
  const long double duration_ns = static_cast<long double>(sample_count) * 1e9L / sample_rate;
  if (duration_ns + first_device_timestamp_ns > std::numeric_limits<int64_t>::max() ||
      duration_ns + capture_host_unix_ns > std::numeric_limits<int64_t>::max())
    return fail("pair manifest sample timeline exceeds supported timestamp range");
  if (!std::isfinite(gain_ch0_db) || !std::isfinite(gain_ch1_db) ||
      std::abs(gain_ch0_db - gain_ch1_db) > 1e-6 || antenna_ch0 != antenna_ch1)
    return fail("pair manifest channel gain or antenna mismatch");

  if (!std::filesystem::exists(resolved_ch0) || !std::filesystem::exists(resolved_ch1))
    return fail("paired IQ data file missing");

  // Validate sidecars if present.
  const auto meta0 = SidecarMeta::load(resolved_ch0);
  const auto meta1 = SidecarMeta::load(resolved_ch1);
  if (meta0.present != meta1.present) return fail("only one channel has sidecar metadata");
  if (meta0.present && meta1.present) {
    for (const auto* meta : {&meta0, &meta1}) {
      if (meta->format != SampleFormat::cf32 || std::abs(meta->sample_rate - sample_rate) > 1 ||
          std::abs(meta->center_freq - center_hz) > 1)
        return fail("sidecar format/rate/frequency mismatch against manifest");
      if ((!meta->session_id.empty() && meta->session_id != session_id) ||
          (meta->config_epoch && meta->config_epoch != config_epoch) ||
          (!meta->rig_id.empty() && meta->rig_id != rig_id) ||
          (meta->capture_host_unix_ns && meta->capture_host_unix_ns != capture_host_unix_ns))
        return fail("sidecar session/epoch/rig/capture time mismatch against manifest");
    }
    if (meta0.first_device_timestamp_ns != first_device_timestamp_ns ||
        meta1.first_device_timestamp_ns != first_device_timestamp_ns)
      return fail("sidecar device timestamp mismatch against manifest");
    if (meta0.samples != sample_count || meta1.samples != sample_count)
      return fail("sidecar sample count mismatch against manifest");
    if (meta0.overflows != 0 || meta1.overflows != 0)
      return fail("sidecar reports nonzero overflows");
    if (meta0.channel >= 0 && meta1.channel >= 0 &&
        (static_cast<std::size_t>(meta0.channel) != channel_order[0] ||
         static_cast<std::size_t>(meta1.channel) != channel_order[1]))
      return fail("sidecar channel index order does not match manifest channel_order");
  }

  const std::size_t bps = sizeof(cf32);
  if (sample_count > std::numeric_limits<uint64_t>::max() / bps)
    return fail("manifest sample count overflows byte count");
  uint64_t bytes0 = 0, bytes1 = 0;
  const std::string hash0 = fnv1a64_file(resolved_ch0, &bytes0);
  const std::string hash1 = fnv1a64_file(resolved_ch1, &bytes1);
  if (bytes0 != bytes1 || bytes0 != sample_count * bps)
    return fail("paired file byte lengths do not match manifest sample_count");
  if (!ch0_fnv1a64.empty() && hash0 != ch0_fnv1a64)
    return fail("ch0 checksum mismatch against manifest");
  if (!ch1_fnv1a64.empty() && hash1 != ch1_fnv1a64)
    return fail("ch1 checksum mismatch against manifest");
  return true;
}

const char* to_string(DfCalState state) {
  switch (state) {
    case DfCalState::Disconnected: return "Disconnected";
    case DfCalState::Uncalibrated: return "Uncalibrated";
    case DfCalState::Calibrating:  return "Calibrating";
    case DfCalState::Valid:        return "Valid";
    case DfCalState::Stale:        return "Stale";
    case DfCalState::Error:        return "Error";
  }
  return "Unknown";
}

PhaseCal PhaseCal::load(const std::string& path) {
  const auto config = Config::load(path);
  auto number = [&](const std::string& key) {
    const auto value = config.get(key);
    double result = 0;
    if (!parse_strict_double(value, result))
      throw std::invalid_argument("invalid calibration field: " + key);
    return result;
  };
  PhaseCal calibration;
  calibration.phase_rad = number("phase_rad");
  calibration.center_hz = number("center_hz");
  calibration.sample_rate = number("sample_rate");
  calibration.spacing_m = number("spacing_m");
  calibration.max_age_seconds = number("max_age_seconds");
  if (!parse_strict_i64(config.get("measured_unix_ns"), calibration.measured_unix_ns))
    throw std::invalid_argument("invalid calibration timestamp");

  // Optional DF-2 extended fields (preserve backward compatibility with minimal TOML):
  calibration.session_id = config.get("session_id");
  if (!config.get("config_epoch").empty()) {
    if (!parse_strict_u64(config.get("config_epoch"), calibration.config_epoch))
      throw std::invalid_argument("invalid calibration config_epoch");
  }
  calibration.rig_id = config.get("rig_id");
  if (!config.get("reference_method").empty())
    calibration.reference_method = config.get("reference_method");
  if (calibration.reference_method != "splitter" && calibration.reference_method != "ota_broadside")
    throw std::invalid_argument("invalid calibration reference_method");
  if (!config.get("passband_hz").empty()) calibration.passband_hz = number("passband_hz");
  if (!config.get("gain_db").empty()) calibration.gain_db = number("gain_db");
  if (!config.get("channel_0").empty() && !config.get("channel_1").empty()) {
    uint64_t ch0 = 0, ch1 = 1;
    if (!parse_strict_u64(config.get("channel_0"), ch0) ||
        !parse_strict_u64(config.get("channel_1"), ch1) || ch0 == ch1)
      throw std::invalid_argument("invalid calibration channel order");
    calibration.channel_order = {static_cast<std::size_t>(ch0), static_cast<std::size_t>(ch1)};
  }
  if (!config.get("cal_samples").empty()) {
    if (!parse_strict_u64(config.get("cal_samples"), calibration.cal_samples))
      throw std::invalid_argument("invalid cal_samples");
  }
  if (!config.get("cal_coherence").empty()) calibration.cal_coherence = number("cal_coherence");
  if (!config.get("phase_dispersion_rad").empty())
    calibration.phase_dispersion_rad = number("phase_dispersion_rad");
  if (!config.get("delay_slope_rad_per_hz").empty())
    calibration.delay_slope_rad_per_hz = number("delay_slope_rad_per_hz");
  if (!config.get("lo_locked_ch0").empty())
    calibration.lo_locked_ch0 = (config.get("lo_locked_ch0") == "true" || config.get("lo_locked_ch0") == "1");
  if (!config.get("lo_locked_ch1").empty())
    calibration.lo_locked_ch1 = (config.get("lo_locked_ch1") == "true" || config.get("lo_locked_ch1") == "1");
  return calibration;
}

std::string PhaseCal::serialize() const {
  std::ostringstream os;
  os << std::setprecision(17);
  os << "phase_rad = " << phase_rad << "\n"
     << "center_hz = " << center_hz << "\n"
     << "sample_rate = " << sample_rate << "\n"
     << "spacing_m = " << spacing_m << "\n"
     << "measured_unix_ns = " << measured_unix_ns << "\n"
     << "max_age_seconds = " << max_age_seconds << "\n";
  if (!session_id.empty()) os << "session_id = \"" << session_id << "\"\n";
  if (config_epoch > 0) os << "config_epoch = " << config_epoch << "\n";
  if (!rig_id.empty()) os << "rig_id = \"" << rig_id << "\"\n";
  os << "reference_method = \"" << reference_method << "\"\n"
     << "passband_hz = " << passband_hz << "\n"
     << "gain_db = " << gain_db << "\n"
     << "channel_0 = " << channel_order[0] << "\n"
     << "channel_1 = " << channel_order[1] << "\n";
  if (cal_samples > 0) os << "cal_samples = " << cal_samples << "\n";
  os << "cal_coherence = " << cal_coherence << "\n"
     << "phase_dispersion_rad = " << phase_dispersion_rad << "\n"
     << "delay_slope_rad_per_hz = " << delay_slope_rad_per_hz << "\n"
     << "lo_locked_ch0 = " << (lo_locked_ch0 ? "true" : "false") << "\n"
     << "lo_locked_ch1 = " << (lo_locked_ch1 ? "true" : "false") << "\n";
  return os.str();
}

Bearing estimate_bearing(const std::vector<cf32>& first, const std::vector<cf32>& second,
                         int64_t first_ns, int64_t second_ns, double center_hz,
                         double sample_rate, const PhaseCal& calibration, int64_t now_unix_ns) {
  Bearing result;
  auto reject = [&](const char* reason) { result.reason = reason; return result; };
  if (!std::isfinite(center_hz) || center_hz <= 0 || !std::isfinite(sample_rate) || sample_rate <= 0 ||
      !std::isfinite(calibration.phase_rad) || !std::isfinite(calibration.center_hz) ||
      !std::isfinite(calibration.sample_rate) || !std::isfinite(calibration.spacing_m) ||
      !std::isfinite(calibration.max_age_seconds)) return reject("invalid calibration or RF settings");
  if (!std::isfinite(calibration.passband_hz) || calibration.passband_hz <= 0 ||
      calibration.passband_hz > sample_rate || !std::isfinite(calibration.phase_dispersion_rad) ||
      calibration.phase_dispersion_rad < 0 || !std::isfinite(calibration.delay_slope_rad_per_hz) ||
      calibration.delay_slope_rad_per_hz != 0)
    return reject("invalid passband or unsupported group-delay calibration; scalar phase only");
  if (!calibration.lo_locked_ch0 || !calibration.lo_locked_ch1)
    return reject("calibration LO not locked on both channels");
  if (first.size() != second.size() || first.size() < 32 || first_ns != second_ns)
    return reject("channels must have equal lengths and aligned timestamps");
  if (calibration.measured_unix_ns <= 0 || now_unix_ns < calibration.measured_unix_ns ||
      calibration.max_age_seconds <= 0 ||
      (static_cast<long double>(now_unix_ns) - calibration.measured_unix_ns) / 1e9 > calibration.max_age_seconds)
    return reject("calibration missing, future-dated or expired");
  if (std::abs(calibration.center_hz - center_hz) > 1 ||
      std::abs(calibration.sample_rate - sample_rate) > 1)
    return reject("calibration frequency or sample rate mismatch");
  const double wavelength = 299792458.0 / center_hz;
  if (calibration.spacing_m <= 0 || calibration.spacing_m > wavelength / 2)
    return reject("antenna spacing must be positive and no greater than half a wavelength");

  const auto stats = analyze_pair(first, second, sample_rate, calibration.passband_hz);
  if (!stats.finite) return reject("non-finite samples");
  if (stats.first_power <= 0 || stats.second_power <= 0) return reject("zero signal power");
  if (stats.outside_passband_fraction > 0.01)
    return reject("signal spectral occupancy outside calibrated passband");
  result.clipped_fraction = stats.clipped_fraction;
  if (stats.clipped_fraction > 0.001) return reject("receiver clipping detected");

  result.coherence = stats.coherence;
  if (result.coherence < 0.8) return reject("insufficient channel coherence");

  const double pi = std::acos(-1.0);
  result.phase_std_deg = stats.phase_std_rad * 180.0 / pi;
  if (stats.phase_std_rad > 0.35)
    return reject("excessive phase dispersion or overlapping emitters");

  const double phase = std::remainder(stats.mean_phase_rad - calibration.phase_rad, 2 * pi);
  result.phase_diff_rad = phase;
  const double sine = phase * wavelength / (2 * pi * calibration.spacing_m);
  if (std::abs(sine) > 1.0 + 1e-6) return reject("phase inconsistent with antenna spacing");

  const double clamped_sine = std::clamp(sine, -1.0, 1.0);
  const double broadside_rad = std::asin(clamped_sine);
  result.broadside_deg = broadside_rad * 180 / pi;
  result.mirrored_deg = std::remainder(180.0 - result.broadside_deg, 360.0);

  // Endfire-aware angular uncertainty (grows as 1 / cos(broadside_rad) near +-90 deg).
  const double n_eff = std::min<double>(static_cast<double>(first.size()), 256.0);
  const double coh_sq = std::max(result.coherence * result.coherence, 1e-6);
  const double crlb_phase_rad = std::sqrt(std::max(0.0, 1.0 - coh_sq) / (2.0 * n_eff * coh_sq));
  const double total_phase_err_rad =
      std::max(stats.phase_std_rad, std::max(0.0, calibration.phase_dispersion_rad)) + crlb_phase_rad;
  const double cos_theta = std::max(std::sqrt(std::max(0.0, 1.0 - clamped_sine * clamped_sine)), 0.087);
  result.uncertainty_deg = (180.0 / pi) * (total_phase_err_rad * wavelength) /
                           (2.0 * pi * calibration.spacing_m * cos_theta);

  result.valid = true;
  return result;
}

bool DfCalibrationService::connect(const std::string& session_id, uint64_t config_epoch,
                                   const std::string& rig_id, double center_hz,
                                   double sample_rate, double gain_db,
                                   std::array<std::size_t, 2> channel_order,
                                   bool lo_locked_ch0, bool lo_locked_ch1) {
  cal_ = PhaseCal{};
  if (session_id.empty() || config_epoch == 0 || rig_id.empty()) {
    state_ = DfCalState::Error;
    reason_ = "missing session_id, config_epoch, or rig_id";
    return false;
  }
  if (!std::isfinite(center_hz) || center_hz < 1e6 || center_hz > 6e9 ||
      !std::isfinite(sample_rate) || sample_rate < 1e6 || sample_rate > 250e6 ||
      !std::isfinite(gain_db)) {
    state_ = DfCalState::Error;
    reason_ = "invalid RF frequency, sample rate, or gain";
    return false;
  }
  if (channel_order[0] == channel_order[1]) {
    state_ = DfCalState::Error;
    reason_ = "paired channels must be distinct";
    return false;
  }
  session_id_ = session_id;
  config_epoch_ = config_epoch;
  rig_id_ = rig_id;
  center_hz_ = center_hz;
  sample_rate_ = sample_rate;
  gain_db_ = gain_db;
  channel_order_ = channel_order;
  lo_locked_ch0_ = lo_locked_ch0;
  lo_locked_ch1_ = lo_locked_ch1;
  if (!lo_locked_ch0_ || !lo_locked_ch1_) {
    state_ = DfCalState::Error;
    reason_ = "LO not locked on both RX channels";
    return false;
  }
  state_ = DfCalState::Uncalibrated;
  reason_ = "session connected; same-session phase calibration required";
  return true;
}

bool DfCalibrationService::begin_calibration(const std::string& reference_method) {
  if (state_ == DfCalState::Disconnected) {
    state_ = DfCalState::Error;
    reason_ = "cannot start calibration while disconnected";
    return false;
  }
  if (!lo_locked_ch0_ || !lo_locked_ch1_) {
    state_ = DfCalState::Error;
    reason_ = "cannot calibrate with unlocked LO";
    return false;
  }
  if (reference_method != "splitter" && reference_method != "ota_broadside") {
    state_ = DfCalState::Error;
    reason_ = "unsupported reference method (must be splitter or ota_broadside)";
    return false;
  }
  pending_ref_method_ = reference_method;
  state_ = DfCalState::Calibrating;
  reason_ = "measuring " + reference_method + " phase reference";
  return true;
}

bool DfCalibrationService::complete_calibration(const PairedBlock& reference_block, double spacing_m,
                                                int64_t measured_unix_ns, double max_age_seconds,
                                                double passband_hz) {
  if (state_ != DfCalState::Calibrating && state_ != DfCalState::Uncalibrated) {
    state_ = DfCalState::Error;
    reason_ = "calibration requires Uncalibrated or Calibrating state";
    return false;
  }
  if (!lo_locked_ch0_ || !lo_locked_ch1_) {
    state_ = DfCalState::Error;
    reason_ = "LO unlocked during calibration";
    return false;
  }
  std::string block_err;
  if (!reference_block.validate(&block_err)) {
    state_ = DfCalState::Error;
    reason_ = "reference block invalid: " + block_err;
    return false;
  }
  if (reference_block.session_id != session_id_ ||
      reference_block.config_epoch != config_epoch_ ||
      reference_block.rig_id != rig_id_ ||
      reference_block.channel_order != channel_order_) {
    state_ = DfCalState::Error;
    reason_ = "reference block session, epoch, rig, or channel order mismatch";
    return false;
  }
  if (std::abs(reference_block.center_hz - center_hz_) > 1.0 ||
      std::abs(reference_block.sample_rate - sample_rate_) > 1.0) {
    state_ = DfCalState::Error;
    reason_ = "reference block RF center or sample rate mismatch";
    return false;
  }
  const double wavelength = 299792458.0 / center_hz_;
  if (!std::isfinite(spacing_m) || spacing_m <= 0 || spacing_m > wavelength / 2.0 ||
      !std::isfinite(max_age_seconds) || max_age_seconds <= 0 ||
      !std::isfinite(passband_hz) || passband_hz <= 0 || passband_hz > sample_rate_ ||
      measured_unix_ns <= 0) {
    state_ = DfCalState::Error;
    reason_ = "invalid calibration spacing, age, passband, or timestamp";
    return false;
  }

  const auto stats = analyze_pair(reference_block.ch0, reference_block.ch1, sample_rate_, passband_hz);
  if (!stats.finite) {
    state_ = DfCalState::Error;
    reason_ = "non-finite samples in calibration reference";
    return false;
  }
  const double mean_p0 = stats.first_power / static_cast<double>(reference_block.ch0.size());
  const double mean_p1 = stats.second_power / static_cast<double>(reference_block.ch1.size());
  if (mean_p0 < 1e-7 || mean_p1 < 1e-7) {
    state_ = DfCalState::Error;
    reason_ = "insufficient signal power in calibration reference";
    return false;
  }
  if (stats.clipped_fraction > 0.0) {
    state_ = DfCalState::Error;
    reason_ = "clipping detected in calibration reference";
    return false;
  }
  const double min_coh = (pending_ref_method_ == "splitter") ? 0.98 : 0.92;
  if (stats.coherence < min_coh) {
    state_ = DfCalState::Error;
    reason_ = "reference coherence below threshold for " + pending_ref_method_;
    return false;
  }
  if (stats.phase_std_rad > 0.087) {
    state_ = DfCalState::Error;
    reason_ = "unstable phase in calibration reference window";
    return false;
  }
  if (stats.outside_passband_fraction > 0.01) {
    state_ = DfCalState::Error;
    reason_ = "reference spectral occupancy outside calibrated passband";
    return false;
  }

  PhaseCal cal;
  cal.phase_rad = stats.mean_phase_rad;
  cal.center_hz = center_hz_;
  cal.sample_rate = sample_rate_;
  cal.spacing_m = spacing_m;
  cal.measured_unix_ns = measured_unix_ns;
  cal.max_age_seconds = max_age_seconds;
  cal.session_id = session_id_;
  cal.config_epoch = config_epoch_;
  cal.rig_id = rig_id_;
  cal.reference_method = pending_ref_method_;
  cal.passband_hz = passband_hz;
  cal.gain_db = gain_db_;
  cal.channel_order = channel_order_;
  cal.cal_samples = reference_block.ch0.size();
  cal.cal_coherence = stats.coherence;
  cal.phase_dispersion_rad = stats.phase_std_rad;
  cal.delay_slope_rad_per_hz = 0;
  cal.lo_locked_ch0 = lo_locked_ch0_;
  cal.lo_locked_ch1 = lo_locked_ch1_;

  cal_ = cal;
  state_ = DfCalState::Valid;
  reason_ = "calibrated (" + pending_ref_method_ + ")";
  return true;
}

bool DfCalibrationService::adopt_same_session_calibration(const PhaseCal& cal, int64_t now_unix_ns) {
  if (state_ == DfCalState::Disconnected) {
    state_ = DfCalState::Error;
    reason_ = "cannot adopt calibration while disconnected";
    return false;
  }
  if (cal.session_id.empty() || cal.session_id != session_id_ ||
      cal.config_epoch == 0 || cal.config_epoch != config_epoch_ ||
      cal.rig_id != rig_id_ ||
      cal.channel_order != channel_order_) {
    state_ = DfCalState::Error;
    reason_ = "cross-session or mismatched-epoch/rig/channel calibration rejected";
    return false;
  }
  if (std::abs(cal.center_hz - center_hz_) > 1.0 ||
      std::abs(cal.sample_rate - sample_rate_) > 1.0 ||
      std::abs(cal.gain_db - gain_db_) > 0.1) {
    state_ = DfCalState::Stale;
    reason_ = "calibration RF frequency, sample rate, or gain mismatch";
    return false;
  }
  if (!std::isfinite(cal.phase_rad) || !std::isfinite(cal.center_hz) ||
      !std::isfinite(cal.sample_rate) || !std::isfinite(cal.gain_db) ||
      !std::isfinite(cal.spacing_m) || cal.spacing_m <= 0 || cal.spacing_m > 299792458.0 / center_hz_ / 2 ||
      !std::isfinite(cal.max_age_seconds) || !std::isfinite(cal.passband_hz) ||
      cal.passband_hz <= 0 || cal.passband_hz > sample_rate_ ||
      !std::isfinite(cal.phase_dispersion_rad) || cal.phase_dispersion_rad < 0 ||
      cal.phase_dispersion_rad > 0.087 || cal.cal_samples < 32 ||
      !std::isfinite(cal.cal_coherence) || cal.cal_coherence > 1 ||
      cal.cal_coherence < (cal.reference_method == "splitter" ? 0.98 : 0.92) ||
      (cal.reference_method != "splitter" && cal.reference_method != "ota_broadside") ||
      !std::isfinite(cal.delay_slope_rad_per_hz) || cal.delay_slope_rad_per_hz != 0) {
    state_ = DfCalState::Error;
    reason_ = "invalid calibration settings or unsupported group-delay correction";
    return false;
  }
  if (!cal.lo_locked_ch0 || !cal.lo_locked_ch1 || !lo_locked_ch0_ || !lo_locked_ch1_) {
    state_ = DfCalState::Error;
    reason_ = "LO not locked on both channels";
    return false;
  }
  if (cal.measured_unix_ns <= 0 || now_unix_ns < cal.measured_unix_ns ||
      cal.max_age_seconds <= 0 ||
      (static_cast<long double>(now_unix_ns) - cal.measured_unix_ns) / 1e9 > cal.max_age_seconds) {
    state_ = DfCalState::Stale;
    reason_ = "calibration expired or future-dated";
    return false;
  }
  cal_ = cal;
  state_ = DfCalState::Valid;
  reason_ = "calibrated (" + cal.reference_method + ")";
  return true;
}

void DfCalibrationService::on_retune(double new_center_hz) {
  center_hz_ = new_center_hz;
  ++config_epoch_;
  if (state_ == DfCalState::Valid || state_ == DfCalState::Calibrating) {
    state_ = DfCalState::Stale;
    reason_ = "retune invalidates phase calibration until recalibrated";
  }
}

void DfCalibrationService::on_config_change(const std::string& change_reason) {
  ++config_epoch_;
  if (state_ == DfCalState::Valid || state_ == DfCalState::Calibrating) {
    state_ = DfCalState::Stale;
    reason_ = change_reason.empty() ? "configuration change invalidated calibration" : change_reason;
  }
}

void DfCalibrationService::on_gap_or_overflow(const std::string& gap_reason) {
  if (state_ == DfCalState::Valid || state_ == DfCalState::Calibrating) {
    state_ = DfCalState::Stale;
    reason_ = gap_reason.empty() ? "stream discontinuity or overflow invalidated calibration" : gap_reason;
  }
}

void DfCalibrationService::on_lo_unlock(const std::string& lock_reason) {
  lo_locked_ch0_ = false;
  lo_locked_ch1_ = false;
  state_ = DfCalState::Error;
  reason_ = lock_reason;
}

void DfCalibrationService::on_disconnect(const std::string& disc_reason) {
  state_ = DfCalState::Disconnected;
  reason_ = disc_reason;
  cal_ = PhaseCal{};
}

Bearing DfCalibrationService::estimate_block_bearing(const PairedBlock& block, int64_t now_unix_ns,
                                                     double noise_power, double min_snr_db) {
  Bearing result;
  auto reject = [&](const std::string& r) { result.reason = r; return result; };
  if (state_ != DfCalState::Valid)
    return reject("calibration state is " + std::string(to_string(state_)) + ": " + reason_);
  if (!lo_locked_ch0_ || !lo_locked_ch1_) {
    on_lo_unlock();
    return reject(reason_);
  }
  if (block.has_gap || block.has_overflow) {
    on_gap_or_overflow("paired block reported gap or overflow");
    return reject(reason_);
  }
  std::string block_err;
  if (!block.validate(&block_err)) return reject(block_err);

  if (block.session_id != session_id_ || block.config_epoch != config_epoch_ ||
      block.rig_id != rig_id_ ||
      block.channel_order != channel_order_) {
    return reject("paired block session, config_epoch, rig, or channel_order mismatch");
  }
  if (cal_.measured_unix_ns <= 0 || now_unix_ns < cal_.measured_unix_ns ||
      (static_cast<long double>(now_unix_ns) - cal_.measured_unix_ns) / 1e9 > cal_.max_age_seconds) {
    state_ = DfCalState::Stale;
    reason_ = "calibration expired";
    return reject(reason_);
  }

  const auto stats = analyze_pair(block.ch0, block.ch1, block.sample_rate);
  if (!stats.finite) return reject("non-finite samples");
  const double mean_power =
      0.5 * (stats.first_power + stats.second_power) / static_cast<double>(block.ch0.size());
  if (noise_power > 0 && std::isfinite(noise_power)) {
    const double snr_lin = (mean_power - noise_power) / noise_power;
    result.snr_db = 10.0 * std::log10(std::max(snr_lin, 1e-30));
    if (result.snr_db < min_snr_db) return reject("SNR below bearing gate threshold");
  }

  result = estimate_bearing(block.ch0, block.ch1, block.first_device_timestamp_ns,
                            block.first_device_timestamp_ns, block.center_hz, block.sample_rate,
                            cal_, now_unix_ns);
  if (noise_power > 0 && std::isfinite(noise_power)) {
    const double snr_lin = (mean_power - noise_power) / noise_power;
    result.snr_db = 10.0 * std::log10(std::max(snr_lin, 1e-30));
  }
  return result;
}

}  // namespace droneid
