#include "receiver_args.h"
#include "droneid/config.h"
#include "droneid/usrp_source.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv, {"--config", "--seconds", "--freq", "--gain"}, {"--help"});
    if (args.has("--help")) {
      std::puts("iq_record_pair PREFIX [--config PATH] [--seconds 1] [--freq HZ] [--gain DB]\n"
                "Receives Radio#0/#1 at 15.36 MSPS, sc8 on wire, cf32 to PREFIX.ch0.cf32/PREFIX.ch1.cf32.\n"
                "Checks sample timestamps and rejects overflows. RF phase calibration remains mandatory.");
      return 0;
    }
    if (args.files.size() != 1) throw std::invalid_argument("provide one output prefix");
    const double seconds = args.number("--seconds", 1);
    if (seconds <= 0 || seconds > 60) throw std::invalid_argument("seconds must be in (0,60]");
    droneid::Config config;
    if (args.has("--config")) config = droneid::Config::load(args.get("--config"));
    if (std::abs(config.sample_rate - 15.36e6) > 1) throw std::invalid_argument("paired capture requires 15.36 MSPS");
    droneid::UsrpParams params;
    params.device_args = config.device_args;
    params.master_clock_rate = config.master_clock_rate;
    params.gain_db = args.number("--gain", config.gain_db);
    params.center_freq = args.number("--freq", config.center_freq);
    if (params.center_freq < 1e6 || params.center_freq > 6e9) throw std::invalid_argument("invalid center");
    params.antenna = config.antenna;
    params.channels = {0, 1};
    params.wire_format = "sc8";
    params.auto_start = false;
    const std::string paths[] = {args.files[0] + ".ch0.cf32", args.files[0] + ".ch1.cf32"};
    for (const auto& path : paths)
      if (std::filesystem::exists(path) || std::filesystem::exists(path + ".meta") ||
          std::filesystem::exists(path + ".partial"))
        throw std::invalid_argument("output already exists: " + path);
    std::ofstream outputs[2];
    for (int channel = 0; channel < 2; ++channel) {
      outputs[channel].open(paths[channel] + ".partial", std::ios::binary);
      if (!outputs[channel]) throw std::runtime_error("cannot open output");
    }
    droneid::UsrpSource source(params);
    if (std::abs(source.sample_rate() - 15.36e6) > 1) throw std::runtime_error("unexpected RX rate");
    const double tune_time = source.device_time() + 0.2;
    source.tune_at(params.center_freq, tune_time);
    source.start_at(tune_time + 0.1);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::vector<droneid::cf32> left(65536), right(65536);
    const uint64_t target = static_cast<uint64_t>(seconds * source.sample_rate());
    uint64_t received = 0;
    int64_t timestamp = 0, first_timestamp = 0, capture_unix_ns = 0;
    int timeouts = 0;
    while (received < target && !stopped) {
      const auto count = source.recv_channels({left.data(), right.data()},
          std::min<uint64_t>(left.size(), target - received), timestamp);
      if (!count) {
        if (++timeouts > 3) throw std::runtime_error("paired RX repeatedly timed out");
        continue;
      }
      timeouts = 0;
      if (!received) {
        first_timestamp = timestamp;
        capture_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (std::abs(timestamp / 1e9 - (tune_time + 0.1)) > 1.5 / source.sample_rate())
          throw std::runtime_error("paired RX did not start at the commanded time");
      }
      if (source.overflow_count() || std::abs((timestamp - first_timestamp) / 1e9 -
          received / source.sample_rate()) > 1.5 / source.sample_rate())
        throw std::runtime_error("paired RX gap/overflow; partial files are not valid calibration data");
      outputs[0].write(reinterpret_cast<const char*>(left.data()), count * sizeof(droneid::cf32));
      outputs[1].write(reinterpret_cast<const char*>(right.data()), count * sizeof(droneid::cf32));
      if (!outputs[0] || !outputs[1]) throw std::runtime_error("paired output write failed");
      received += count;
    }
    source.stop();
    if (source.overflow_count()) throw std::runtime_error("overflow during paired capture; data unvalidated");
    for (int channel = 0; channel < 2; ++channel) {
      outputs[channel].close();
      if (!outputs[channel]) throw std::runtime_error("paired output close failed");
    }
    for (int channel = 0; channel < 2; ++channel) {
      std::filesystem::rename(paths[channel] + ".partial", paths[channel]);
      std::ofstream metadata(paths[channel] + ".meta");
      metadata.precision(17);
      metadata << "format=cf32\nsample_rate=" << source.sample_rate() << "\ncenter_freq=" << params.center_freq
               << "\nchannel=" << channel << "\nfirst_device_timestamp_ns=" << first_timestamp
               << "\ncapture_host_unix_ns=" << capture_unix_ns
               << "\nsamples=" << received << "\noverflows=0\nphase_calibrated=false\n";
      metadata.close();
      if (!metadata) throw std::runtime_error("metadata write failed");
    }
    std::fprintf(stderr, "Recorded %llu aligned samples/channel; phase calibration is still required.\n",
                 static_cast<unsigned long long>(received));
    return stopped ? 130 : 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
