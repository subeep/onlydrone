#include "receiver_args.h"
#include "droneid/aoa.h"
#include "droneid/file_source.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv,
                      {"--format", "--rate", "--center", "--samples", "--cal",
                       "--spacing-m", "--measured-unix-ns", "--max-age", "--start-sample",
                       "--manifest", "--session-id", "--config-epoch", "--rig-id"},
                      {"--aligned", "--splitter-calibrate", "--ota-calibrate", "--help"});
    if (args.has("--help")) {
      std::puts("df_bearing [CH0 CH1] (--manifest PAIR.toml | --aligned) --cal PATH [options]\n"
                "  [--format cf32] [--rate HZ] [--center HZ] [--samples N] [--start-sample N]\n"
                "  Calibration: replace --cal with (--splitter-calibrate | --ota-calibrate)\n"
                "    --spacing-m M --measured-unix-ns NS [--max-age 3600]\n"
                "    [--session-id ID] [--config-epoch N] [--rig-id ID].\n"
                "  --manifest validates completeness, FNV-1a checksums, sample counts, and timestamps.\n"
                "  --aligned is an unverified diagnostic assertion; never promote it to live provenance.\n"
                "  Broadside bearing always retains front/back ambiguity and never yields GPS coordinates.");
      return 0;
    }

    const bool has_manifest = args.has("--manifest");
    const bool has_aligned = args.has("--aligned");
    const bool is_splitter_cal = args.has("--splitter-calibrate");
    const bool is_ota_cal = args.has("--ota-calibrate");
    const bool is_cal = is_splitter_cal || is_ota_cal;

    if ((!has_manifest && !has_aligned) || (is_splitter_cal && is_ota_cal) ||
        (args.has("--cal") == is_cal)) {
      throw std::invalid_argument(
          "provide (--manifest or --aligned) and exactly one of --cal/--splitter-calibrate/--ota-calibrate");
    }

    droneid::PairManifest manifest;
    std::string ch0_path, ch1_path;
    if (has_manifest) {
      manifest = droneid::PairManifest::load(args.get("--manifest"));
      if (args.files.size() == 2) {
        ch0_path = args.files[0];
        ch1_path = args.files[1];
      } else if (args.files.empty()) {
        const auto parent = std::filesystem::path(args.get("--manifest")).parent_path();
        const auto p0 = std::filesystem::path(manifest.ch0_path);
        const auto p1 = std::filesystem::path(manifest.ch1_path);
        ch0_path = (p0.is_absolute() || parent.empty()) ? p0.string() : (parent / p0).string();
        ch1_path = (p1.is_absolute() || parent.empty()) ? p1.string() : (parent / p1).string();
      } else {
        throw std::invalid_argument("provide zero or two channel files when using --manifest");
      }
      std::string err;
      if (!manifest.validate_files(ch0_path, ch1_path, &err))
        throw std::runtime_error("manifest validation failed: " + err);
    } else {
      if (args.files.size() != 2)
        throw std::invalid_argument("provide aligned paired files CH0 and CH1");
      ch0_path = args.files[0];
      ch1_path = args.files[1];
    }

    const double requested = args.number("--samples", 65536);
    if (requested < 32 || requested > 1000000 || std::floor(requested) != requested)
      throw std::invalid_argument("samples must be an integer in [32,1000000]");
    const double start = args.number("--start-sample", 0);
    if (start < 0 || start > 1e12 || std::floor(start) != start)
      throw std::invalid_argument("start sample must be an integer in [0,1e12]");

    const auto meta0 = droneid::SidecarMeta::load(ch0_path);
    const auto meta1 = droneid::SidecarMeta::load(ch1_path);
    if (meta0.present != meta1.present)
      throw std::invalid_argument("paired sidecar metadata mismatch (only one channel has .meta)");
    if (meta0.present) {
      if (meta0.overflows > 0 || meta1.overflows > 0)
        throw std::runtime_error("paired sidecar reports overflows; invalid for DF");
      if (meta0.samples != meta1.samples)
        throw std::runtime_error("paired sidecar sample counts do not match");
      if (meta0.channel >= 0 && meta1.channel >= 0 && meta0.channel == meta1.channel)
        throw std::runtime_error("paired sidecars report identical channel index");
      if (meta0.first_device_timestamp_ns != meta1.first_device_timestamp_ns)
        throw std::runtime_error("paired sidecar device timestamps do not match");
      if (meta0.first_device_timestamp_ns == 0)
        throw std::runtime_error("sidecar has invalid zero device timestamp");
      if (!meta0.session_id.empty() && !meta1.session_id.empty() && meta0.session_id != meta1.session_id)
        throw std::runtime_error("paired sidecar session_id mismatch");
    }

    droneid::SampleFormat format = droneid::SampleFormat::cf32;
    if (!droneid::parse_format(args.get("--format", "cf32"), format))
      throw std::invalid_argument("invalid format");
    const double default_rate = has_manifest ? manifest.sample_rate : 15.36e6;
    const double default_center = has_manifest ? manifest.center_hz : 0.0;
    droneid::FileSource first(ch0_path, format, args.number("--rate", default_rate),
                              args.number("--center", default_center));
    droneid::FileSource second(ch1_path, format, args.number("--rate", default_rate),
                               args.number("--center", default_center));
    if (first.sample_rate() != second.sample_rate() || first.center_freq() != second.center_freq())
      throw std::invalid_argument("paired metadata mismatch");

    // Verify equal on-disk file sizes so unequal EOF is caught even when reading a prefix window.
    if (std::filesystem::exists(ch0_path) && std::filesystem::exists(ch1_path)) {
      if (std::filesystem::file_size(ch0_path) != std::filesystem::file_size(ch1_path))
        throw std::runtime_error("paired files have unequal byte lengths on disk");
    }

    std::vector<droneid::cf32> left(static_cast<std::size_t>(requested)), right(left.size());
    int64_t timestamp = 0;
    for (uint64_t skipped = 0; skipped < static_cast<uint64_t>(start);) {
      const auto count = std::min<uint64_t>(left.size(), static_cast<uint64_t>(start) - skipped);
      if (first.recv(left.data(), count, timestamp) != count ||
          second.recv(right.data(), count, timestamp) != count)
        throw std::invalid_argument("start sample beyond end of paired input");
      skipped += count;
    }
    const auto left_count = first.recv(left.data(), left.size(), timestamp);
    const auto right_count = second.recv(right.data(), right.size(), timestamp);
    if (left_count != left.size() || right_count != right.size())
      throw std::invalid_argument("paired files shorter than requested window");

    int64_t first_ns = 1, second_ns = 1;
    const int64_t sample_offset_ns =
        static_cast<int64_t>(std::llround(start * 1e9 / first.sample_rate()));
    if (has_manifest) {
      first_ns = manifest.first_device_timestamp_ns + sample_offset_ns;
      second_ns = first_ns;
    } else if (meta0.present && meta0.first_device_timestamp_ns > 0) {
      first_ns = meta0.first_device_timestamp_ns + sample_offset_ns;
      second_ns = meta1.first_device_timestamp_ns + sample_offset_ns;
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    droneid::PhaseCal calibration;
    if (is_cal) {
      if (!args.has("--measured-unix-ns") || !args.has("--spacing-m"))
        throw std::invalid_argument("calibration requires capture timestamp and antenna spacing");
      std::complex<double> cross(0, 0);
      for (std::size_t index = 0; index < left.size(); ++index)
        cross += std::conj(std::complex<double>(left[index])) * std::complex<double>(right[index]);
      calibration.phase_rad = std::arg(cross);
      calibration.center_hz = first.center_freq();
      calibration.sample_rate = first.sample_rate();
      calibration.spacing_m = args.number("--spacing-m", 0);
      calibration.max_age_seconds = args.number("--max-age", 3600);
      calibration.reference_method = is_ota_cal ? "ota_broadside" : "splitter";
      calibration.cal_samples = left.size();
      calibration.session_id = args.get("--session-id", has_manifest ? manifest.session_id : meta0.session_id);
      if (args.has("--config-epoch")) {
        const double ep = args.number("--config-epoch", 0);
        if (ep < 1 || std::floor(ep) != ep) throw std::invalid_argument("invalid --config-epoch");
        calibration.config_epoch = static_cast<uint64_t>(ep);
      } else if (has_manifest) {
        calibration.config_epoch = manifest.config_epoch;
      } else if (meta0.config_epoch > 0) {
        calibration.config_epoch = meta0.config_epoch;
      }
      calibration.rig_id = args.get("--rig-id", has_manifest ? manifest.rig_id : meta0.rig_id);
      if (has_manifest) {
        calibration.channel_order = manifest.channel_order;
        calibration.gain_db = manifest.gain_ch0_db;
      }
      std::size_t used = 0;
      const auto supplied = args.get("--measured-unix-ns");
      calibration.measured_unix_ns = std::stoll(supplied, &used);
      if (used != supplied.size()) throw std::invalid_argument("invalid capture timestamp");
    } else {
      calibration = droneid::PhaseCal::load(args.get("--cal"));
      if (has_manifest && !calibration.session_id.empty()) {
        if (calibration.session_id != manifest.session_id ||
            (calibration.config_epoch > 0 && calibration.config_epoch != manifest.config_epoch) ||
            (!calibration.rig_id.empty() && calibration.rig_id != manifest.rig_id) ||
            calibration.channel_order != manifest.channel_order)
          throw std::runtime_error("calibration session/epoch/rig/channel provenance does not match manifest");
      }
    }

    const auto bearing = droneid::estimate_bearing(left, right, first_ns, second_ns,
                                                   first.center_freq(), first.sample_rate(),
                                                   calibration, now);
    if (!bearing.valid) throw std::runtime_error(bearing.reason);

    if (is_cal) {
      const double min_coh = is_ota_cal ? 0.92 : 0.98;
      if (bearing.coherence < min_coh)
        throw std::runtime_error(std::string(calibration.reference_method) +
                                 " calibration needs coherence >=" + (is_ota_cal ? "0.92" : "0.98"));
      calibration.cal_coherence = bearing.coherence;
      calibration.phase_dispersion_rad = bearing.phase_std_deg * std::acos(-1.0) / 180.0;
      std::fputs(calibration.serialize().c_str(), stdout);
    } else {
      const char* provenance = has_manifest ? "manifest_verified" : "unverified_aligned_assertion";
      std::printf("{\"broadside_deg\":%.9g,\"mirrored_deg\":%.9g,\"uncertainty_deg\":%.6g,"
                  "\"coherence\":%.9g,\"phase_std_deg\":%.6g,\"ambiguous_front_back\":true,"
                  "\"provenance\":\"%s\",\"absolute_position\":null}\n",
                  bearing.broadside_deg, bearing.mirrored_deg, bearing.uncertainty_deg,
                  bearing.coherence, bearing.phase_std_deg, provenance);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
