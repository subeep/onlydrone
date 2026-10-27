#include "receiver_args.h"
#include "droneid/aoa.h"
#include "droneid/file_source.h"

#include <chrono>
#include <complex>
#include <cstdio>

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv, {"--format", "--rate", "--center", "--samples", "--cal",
        "--spacing-m", "--measured-unix-ns", "--max-age", "--start-sample"}, {"--aligned", "--splitter-calibrate", "--help"});
    if (args.has("--help")) {
      std::puts("df_bearing CH0 CH1 --aligned --cal PATH [--format cf32] [--rate HZ] [--center HZ] [--samples N]\n"
                "  [--start-sample N] selects the same later window in both files.\n"
                "  Calibration: replace --cal with --splitter-calibrate --spacing-m M --measured-unix-ns NS\n"
                "  [--max-age 3600]. Prints calibration TOML to stdout. Use a splitter/known broadside source.\n"
                "--aligned asserts same-sample, same-start, gap-free coherent input. Never use sequential recordings.\n"
                "Recalibrate after every retune/restart; broadside bearing retains front/back ambiguity.");
      return 0;
    }
    if (args.files.size() != 2 || !args.has("--aligned") ||
        args.has("--cal") == args.has("--splitter-calibrate"))
      throw std::invalid_argument("provide aligned paired files and exactly one of --cal/--splitter-calibrate");
    const double requested = args.number("--samples", 65536);
    if (requested < 32 || requested > 1000000 || std::floor(requested) != requested)
      throw std::invalid_argument("samples must be an integer in [32,1000000]");
    const double start = args.number("--start-sample", 0);
    if (start < 0 || start > 1e12 || std::floor(start) != start)
      throw std::invalid_argument("start sample must be an integer in [0,1e12]");
    droneid::SampleFormat format = droneid::SampleFormat::cf32;
    if (!droneid::parse_format(args.get("--format", "cf32"), format)) throw std::invalid_argument("invalid format");
    droneid::FileSource first(args.files[0], format, args.number("--rate", 15.36e6), args.number("--center", 0));
    droneid::FileSource second(args.files[1], format, args.number("--rate", 15.36e6), args.number("--center", 0));
    if (first.sample_rate() != second.sample_rate() || first.center_freq() != second.center_freq())
      throw std::invalid_argument("paired metadata mismatch");
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
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    droneid::PhaseCal calibration;
    if (args.has("--splitter-calibrate")) {
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
      std::size_t used = 0;
      const auto supplied = args.get("--measured-unix-ns");
      calibration.measured_unix_ns = std::stoll(supplied, &used);
      if (used != supplied.size()) throw std::invalid_argument("invalid capture timestamp");
    } else calibration = droneid::PhaseCal::load(args.get("--cal"));
    const auto bearing = droneid::estimate_bearing(left, right, 0, 0, first.center_freq(),
                                                  first.sample_rate(), calibration, now);
    if (!bearing.valid) throw std::runtime_error(bearing.reason);
    if (args.has("--splitter-calibrate")) {
      if (bearing.coherence < 0.98) throw std::runtime_error("splitter calibration needs coherence >=0.98");
      std::printf("phase_rad = %.17g\ncenter_hz = %.17g\nsample_rate = %.17g\nspacing_m = %.17g\n"
                  "measured_unix_ns = %lld\nmax_age_seconds = %.17g\n", calibration.phase_rad,
                  calibration.center_hz, calibration.sample_rate, calibration.spacing_m,
                  static_cast<long long>(calibration.measured_unix_ns), calibration.max_age_seconds);
    } else {
      std::printf("{\"broadside_deg\":%.9g,\"coherence\":%.9g,\"ambiguous_front_back\":true,"
                  "\"absolute_position\":null}\n", bearing.broadside_deg, bearing.coherence);
    }
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
