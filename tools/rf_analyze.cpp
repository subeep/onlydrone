#include "receiver_args.h"
#include "droneid/file_source.h"
#include "droneid/rf_metrics.h"

#include <algorithm>
#include <cstdio>

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv, {"--format", "--rate", "--noise-dbfs", "--threshold-db", "--seconds"},
                      {"--help"});
    if (args.has("--help")) {
      std::puts("rf_analyze FILE --noise-dbfs DB [--format sc16|cf32|sc8] [--rate HZ]\n"
                "  [--threshold-db 10] [--seconds 0]\n"
                "JSONL energy events only: not O4 classification, drone count, absolute range or CFO.\n"
                "Measure noise on the same channel/gain; incorrect noise input biases SNR and detection.");
      return 0;
    }
    if (args.files.size() != 1 || !args.has("--noise-dbfs"))
      throw std::invalid_argument("provide one IQ file and --noise-dbfs; see --help");
    droneid::SampleFormat format = droneid::SampleFormat::sc16;
    if (!droneid::parse_format(args.get("--format", "sc16"), format))
      throw std::invalid_argument("invalid format");
    droneid::FileSource source(args.files[0], format, args.number("--rate", 15.36e6));
    const double rate = source.sample_rate();
    droneid::EnergyBurstDetector detector(rate, args.number("--noise-dbfs", -70),
                                         args.number("--threshold-db", 10));
    const double seconds = args.number("--seconds", 0);
    if (seconds < 0 || seconds > 86400) throw std::invalid_argument("seconds must be in [0,86400]");
    const uint64_t limit = seconds ? static_cast<uint64_t>(seconds * rate) : UINT64_MAX;
    uint64_t received = 0, count = 0;
    std::vector<droneid::cf32> buffer(65536);
    int64_t timestamp = 0;
    auto emit = [&](const std::vector<droneid::RfBurst>& bursts) {
      for (const auto& burst : bursts) {
        const auto metrics = droneid::measure_rf(burst.samples, rate, detector.noise_power());
        std::printf("{\"event\":\"rf_burst\",\"classification\":\"unknown_rf\",\"start_sample\":%llu,"
                    "\"duration_seconds\":%.9g,\"rssi_dbfs\":%.6g,\"snr_db\":%.6g,"
                    "\"bandwidth_99_hz\":%.9g,\"spectral_center_hz\":%.9g,\"clipped_fraction\":%.6g,"
                    "\"truncated\":%s,\"bearing\":null}\n", static_cast<unsigned long long>(burst.start_sample),
                    burst.samples.size() / rate, metrics.rssi_dbfs, metrics.snr_db, metrics.bandwidth_hz,
                    metrics.spectral_center_hz, metrics.clipped_fraction, burst.truncated ? "true" : "false");
        ++count;
      }
    };
    while (received < limit) {
      const auto got = source.recv(buffer.data(), std::min<uint64_t>(buffer.size(), limit - received), timestamp);
      if (!got) break;
      emit(detector.feed(buffer.data(), got));
      received += got;
    }
    emit(detector.feed(nullptr, 0, true));
    std::printf("{\"event\":\"summary\",\"samples\":%llu,\"rf_events\":%llu,\"identified_drones\":null}\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(count));
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
