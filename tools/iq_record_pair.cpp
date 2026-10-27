#include "receiver_args.h"
#include "droneid/aoa.h"
#include "droneid/config.h"
#include "droneid/usrp_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop_handler(int) { stopped = 1; }

struct WriteChunk {
  std::vector<droneid::cf32> ch0;
  std::vector<droneid::cf32> ch1;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv,
                      {"--config", "--seconds", "--freq", "--gain", "--session-id",
                       "--config-epoch", "--rig-id", "--cable-id", "--spacing-m", "--orientation-deg"},
                      {"--help"});
    if (args.has("--help")) {
      std::puts("iq_record_pair PREFIX [--config PATH] [--seconds 1] [--freq HZ] [--gain DB]\n"
                "  [--session-id ID] [--config-epoch N] [--rig-id ID] [--cable-id ID]\n"
                "  [--spacing-m 0.05] [--orientation-deg 0]\n"
                "Receives Radio#0/#1 at 15.36 MSPS, sc8 on wire, cf32 to PREFIX.ch0.cf32/PREFIX.ch1.cf32\n"
                "using a bounded writer queue and manifest-last transaction (PREFIX.pair.toml).\n"
                "Checks sample timestamps and rejects overflows. RF phase calibration remains mandatory.");
      return 0;
    }
    if (args.files.size() != 1) throw std::invalid_argument("provide one output prefix");
    const std::string prefix = args.files[0];
    const double seconds = args.number("--seconds", 1);
    if (seconds <= 0 || seconds > 60) throw std::invalid_argument("seconds must be in (0,60]");

    droneid::Config config;
    if (args.has("--config")) config = droneid::Config::load(args.get("--config"));
    if (std::abs(config.sample_rate - 15.36e6) > 1)
      throw std::invalid_argument("paired capture requires 15.36 MSPS");

    droneid::UsrpParams params;
    params.device_args = config.device_args;
    params.master_clock_rate = config.master_clock_rate;
    params.gain_db = args.number("--gain", config.gain_db);
    params.center_freq = args.number("--freq", config.center_freq);
    if (params.center_freq < 1e6 || params.center_freq > 6e9)
      throw std::invalid_argument("invalid center");
    params.antenna = config.antenna;
    params.channels = {0, 1};
    params.wire_format = "sc8";
    params.auto_start = false;

    const std::string paths[] = {prefix + ".ch0.cf32", prefix + ".ch1.cf32"};
    const std::string manifest_path = prefix + ".pair.toml";
    const std::string incomplete_manifest_path = prefix + ".pair.incomplete.toml";
    for (const auto& path : paths) {
      if (std::filesystem::exists(path) || std::filesystem::exists(path + ".meta") ||
          std::filesystem::exists(path + ".partial"))
        throw std::invalid_argument("output already exists: " + path);
    }
    if (std::filesystem::exists(manifest_path) || std::filesystem::exists(incomplete_manifest_path))
      throw std::invalid_argument("manifest already exists: " + manifest_path);

    const int64_t init_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    droneid::PairManifest manifest;
    manifest.schema_version = 1;
    manifest.complete = false;
    manifest.status_reason = "recording_in_progress";
    manifest.session_id = args.get("--session-id", "sess_" + std::to_string(init_unix_ns));
    const double epoch_arg = args.number("--config-epoch", 1);
    if (epoch_arg < 1 || std::floor(epoch_arg) != epoch_arg)
      throw std::invalid_argument("config-epoch must be a positive integer");
    manifest.config_epoch = static_cast<uint64_t>(epoch_arg);
    manifest.rig_id = args.get("--rig-id", "x310_ubx_pair");
    manifest.cable_id = args.get("--cable-id", "matched_coax_pair");
    manifest.spacing_m = args.number("--spacing-m", 0.05);
    manifest.orientation_deg = args.number("--orientation-deg", 0.0);
    if (manifest.spacing_m <= 0 || manifest.spacing_m > (299792458.0 / params.center_freq) / 2.0)
      throw std::invalid_argument("spacing-m must be in (0, wavelength/2]");
    manifest.sample_rate = 15.36e6;
    manifest.center_hz = params.center_freq;
    manifest.gain_ch0_db = params.gain_db;
    manifest.gain_ch1_db = params.gain_db;
    manifest.bandwidth_hz = params.bandwidth;
    manifest.antenna_ch0 = params.antenna;
    manifest.antenna_ch1 = params.antenna;
    manifest.channel_order = {0, 1};
    manifest.ch0_path = std::filesystem::path(paths[0]).filename().string();
    manifest.ch1_path = std::filesystem::path(paths[1]).filename().string();

    std::ofstream outputs[2];
    for (int channel = 0; channel < 2; ++channel) {
      outputs[channel].open(paths[channel] + ".partial", std::ios::binary);
      if (!outputs[channel]) throw std::runtime_error("cannot open output");
    }

    auto publish_incomplete = [&](const std::string& reason) {
      manifest.complete = false;
      manifest.status_reason = reason;
      try { manifest.save(incomplete_manifest_path); } catch (...) {}
    };

    try {
      droneid::UsrpSource source(params);
      if (std::abs(source.sample_rate() - 15.36e6) > 1)
        throw std::runtime_error("unexpected RX rate");
      manifest.radio_serial = source.serial();
      manifest.sample_rate = source.sample_rate();

      // Bounded writer queue so disk I/O never blocks the UHD RX loop.
      constexpr std::size_t kMaxWriterQueue = 64;
      std::mutex q_mutex;
      std::condition_variable q_cv;
      std::deque<WriteChunk> write_queue;
      bool rx_done = false;
      std::string writer_error;
      uint64_t fnv0 = 14695981039346656037ULL;
      uint64_t fnv1 = 14695981039346656037ULL;

      std::thread writer([&] {
        for (;;) {
          WriteChunk chunk;
          {
            std::unique_lock<std::mutex> lock(q_mutex);
            q_cv.wait(lock, [&] { return rx_done || !write_queue.empty(); });
            if (write_queue.empty()) break;
            chunk = std::move(write_queue.front());
            write_queue.pop_front();
          }
          const std::size_t bytes = chunk.ch0.size() * sizeof(droneid::cf32);
          fnv0 = droneid::fnv1a64_update(fnv0, chunk.ch0.data(), bytes);
          fnv1 = droneid::fnv1a64_update(fnv1, chunk.ch1.data(), bytes);
          outputs[0].write(reinterpret_cast<const char*>(chunk.ch0.data()), bytes);
          outputs[1].write(reinterpret_cast<const char*>(chunk.ch1.data()), bytes);
          if (!outputs[0] || !outputs[1]) {
            std::lock_guard<std::mutex> lock(q_mutex);
            writer_error = "paired output write failed";
            break;
          }
        }
      });

      auto join_writer = [&] {
        {
          std::lock_guard<std::mutex> lock(q_mutex);
          rx_done = true;
        }
        q_cv.notify_all();
        if (writer.joinable()) writer.join();
      };

      try {
        const double tune_time = source.device_time() + 0.2;
        source.tune_at(params.center_freq, tune_time);
        source.start_at(tune_time + 0.1);
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);

        std::vector<droneid::cf32> left(65536), right(65536);
        const uint64_t target = static_cast<uint64_t>(seconds * source.sample_rate());
        uint64_t received = 0;
        int64_t timestamp = 0, first_timestamp = -1, capture_unix_ns = 0;
        int timeouts = 0;

        while (received < target && !stopped) {
          const auto count = source.recv_channels(
              {left.data(), right.data()},
              std::min<uint64_t>(left.size(), target - received), timestamp);
          if (!count) {
            if (++timeouts > 3) throw std::runtime_error("paired RX repeatedly timed out");
            continue;
          }
          timeouts = 0;
          if (!received) {
            if (timestamp <= 0)
              throw std::runtime_error("paired RX missing positive device timestamp");
            first_timestamp = timestamp;
            capture_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (std::abs(timestamp / 1e9 - (tune_time + 0.1)) > 1.5 / source.sample_rate())
              throw std::runtime_error("paired RX did not start at the commanded time");
          }
          if (source.overflow_count() ||
              std::abs((timestamp - first_timestamp) / 1e9 - received / source.sample_rate()) >
                  1.5 / source.sample_rate()) {
            manifest.overflows = source.overflow_count();
            manifest.discontinuities = 1;
            throw std::runtime_error(
                "paired RX gap/overflow; partial files are not valid calibration data");
          }
          {
            std::lock_guard<std::mutex> lock(q_mutex);
            if (!writer_error.empty()) throw std::runtime_error(writer_error);
            if (write_queue.size() >= kMaxWriterQueue)
              throw std::runtime_error("paired writer queue backpressure overflow");
            write_queue.push_back({std::vector<droneid::cf32>(left.begin(), left.begin() + count),
                                   std::vector<droneid::cf32>(right.begin(), right.begin() + count)});
          }
          q_cv.notify_one();
          received += count;
          manifest.sample_count = received;
          manifest.first_device_timestamp_ns = first_timestamp;
          manifest.capture_host_unix_ns = capture_unix_ns;
        }
        source.stop();
        join_writer();
        if (!writer_error.empty()) throw std::runtime_error(writer_error);
        if (source.overflow_count()) {
          manifest.overflows = source.overflow_count();
          throw std::runtime_error("overflow during paired capture; data unvalidated");
        }
        if (stopped || received != target) {
          publish_incomplete("capture interrupted before target sample count");
          std::fprintf(stderr, "Interrupted after %llu/%llu samples; incomplete artifacts left unpromoted.\n",
                       static_cast<unsigned long long>(received),
                       static_cast<unsigned long long>(target));
          return 130;
        }
        for (int channel = 0; channel < 2; ++channel) {
          outputs[channel].close();
          if (!outputs[channel]) throw std::runtime_error("paired output close failed");
        }

        // Manifest-last transaction: rename data files, write sidecars, then publish complete manifest.
        for (int channel = 0; channel < 2; ++channel) {
          std::filesystem::rename(paths[channel] + ".partial", paths[channel]);
          std::ofstream metadata(paths[channel] + ".meta");
          metadata.precision(17);
          metadata << "format=cf32\nsample_rate=" << source.sample_rate()
                   << "\ncenter_freq=" << params.center_freq
                   << "\nchannel=" << channel
                   << "\nfirst_device_timestamp_ns=" << first_timestamp
                   << "\ncapture_host_unix_ns=" << capture_unix_ns
                   << "\nsamples=" << received
                   << "\noverflows=0\nphase_calibrated=false"
                   << "\nsession_id=" << manifest.session_id
                   << "\nconfig_epoch=" << manifest.config_epoch
                   << "\nrig_id=" << manifest.rig_id << "\n";
          metadata.close();
          if (!metadata) throw std::runtime_error("metadata write failed");
        }
        manifest.ch0_fnv1a64 = droneid::fnv1a64_hex(fnv0);
        manifest.ch1_fnv1a64 = droneid::fnv1a64_hex(fnv1);
        manifest.complete = true;
        manifest.status_reason = "ok";
        manifest.save(manifest_path);

        std::fprintf(stderr,
                     "Recorded %llu aligned samples/channel (manifest %s); phase calibration is still required.\n",
                     static_cast<unsigned long long>(received), manifest_path.c_str());
        return 0;
      } catch (...) {
        try { source.stop(); } catch (...) {}
        join_writer();
        throw;
      }
    } catch (const std::exception& inner) {
      publish_incomplete(inner.what());
      throw;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
