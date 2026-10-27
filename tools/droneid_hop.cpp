#include "receiver_args.h"
#include "droneid/config.h"
#include "droneid/dji.h"
#include "droneid/hop.h"
#ifdef DRONEID_HAVE_UHD
#include "droneid/usrp_source.h"
#endif

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <memory>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv, {"--band", "--dwell", "--settle", "--period", "--burst",
        "--probability", "--cycles", "--config"}, {"--live", "--help"});
    if (args.has("--help")) {
      std::puts("droneid_hop [--band 24|58] [--dwell SECONDS] [--settle SECONDS] [--cycles N]\n"
                "  [--period MEASURED_SECONDS --burst SECONDS --probability P] [--live --config PATH]\n"
                "Default: plan only. Live mode receives bounded dwells, then stops RX to decode.\n"
                "Reported catch probability is a random-phase periodic-source model, not a measured catch rate.");
      return 0;
    }
    if (!args.files.empty()) throw std::invalid_argument("unexpected positional argument");
    const double band = args.number("--band", 24);
    if (band != 24 && band != 58) throw std::invalid_argument("band must be 24 or 58");
    const auto channels = droneid::HopSchedule::channels_for_band(static_cast<int>(band));
    const double cycles = args.number("--cycles", 1);
    if (cycles < 1 || cycles > 1000 || std::floor(cycles) != cycles)
      throw std::invalid_argument("cycles must be an integer in [1,1000]");
    const double period = args.number("--period", 0);
    const double burst = args.number("--burst", 0.00065);
    const double probability = args.number("--probability", 0.95);
    if ((args.has("--burst") || args.has("--probability")) && !args.has("--period"))
      throw std::invalid_argument("burst/probability requires a measured --period");
    double dwell = args.number("--dwell", 0.8);
    if (args.has("--period")) {
      const double derived = droneid::HopSchedule::dwell_for_probability(period, burst, probability);
      if (!args.has("--dwell")) dwell = derived;
    }
    const double settle = args.number("--settle", 0.05);
    droneid::HopSchedule schedule(channels, dwell, settle);
    std::printf("{\"event\":\"plan\",\"dwell_seconds\":%.9g,\"cycle_seconds_without_processing\":%.9g,"
                "\"modeled_per_visit_catch\":", dwell, schedule.cycle_seconds());
    if (args.has("--period")) std::printf("%.9g", schedule.catch_probability(period, burst));
    else std::printf("null");
    std::puts("}");
#ifdef DRONEID_HAVE_UHD
    std::unique_ptr<droneid::UsrpSource> source;
    if (args.has("--live")) {
      if (dwell > 2 || settle < 0.02) throw std::invalid_argument("live dwell <=2s and settle >=20ms required");
      droneid::Config config;
      if (args.has("--config")) config = droneid::Config::load(args.get("--config"));
      if (config.num_channels != 1 || std::abs(config.sample_rate - 15.36e6) > 1)
        throw std::invalid_argument("hop receiver requires one channel at 15.36 MSPS");
      droneid::UsrpParams params;
      params.device_args = config.device_args;
      params.master_clock_rate = config.master_clock_rate;
      params.gain_db = config.gain_db;
      params.antenna = config.antenna;
      params.wire_format = config.wire_format;
      params.center_freq = channels.front();
      params.auto_start = false;
      source = std::make_unique<droneid::UsrpSource>(params);
      if (std::abs(source->sample_rate() - 15.36e6) > 1) throw std::runtime_error("unexpected RX rate");
    }
#else
    if (args.has("--live")) throw std::runtime_error("rebuild with DRONEID_WITH_UHD=ON");
#endif
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::size_t visits = 0, valid_visits = 0, hit_visits = 0;
#ifdef DRONEID_HAVE_UHD
    double previous_end = 0;
#endif
    for (std::size_t index = 0; index < channels.size() * cycles && !stopped; ++index) {
      auto visit = schedule.visit(index);
#ifdef DRONEID_HAVE_UHD
      if (source) {
        visit = schedule.visit(0, source->device_time() + 0.2);
        visit.center_hz = channels[index % channels.size()];
        const double gap_seconds = previous_end ? visit.tune_seconds - previous_end : 0;
        previous_end = visit.end_seconds;
        source->tune_at(visit.center_hz, visit.tune_seconds);
        source->start_at(visit.listen_seconds);
        const auto overflows = source->overflow_count();
        const auto target = static_cast<std::size_t>(std::llround(dwell * source->sample_rate()));
        std::vector<droneid::cf32> samples(target);
        std::size_t received = 0;
        int64_t timestamp = 0, first_timestamp = 0;
        bool discontinuity = false;
        int timeouts = 0;
        while (received < target && !stopped) {
          const auto count = source->recv(samples.data() + received,
              std::min<std::size_t>(65536, target - received), timestamp);
          if (!count) {
            if (++timeouts > 3) throw std::runtime_error("RX repeatedly timed out");
            continue;
          }
          timeouts = 0;
          if (!received) {
            first_timestamp = timestamp;
            if (std::abs(timestamp / 1e9 - visit.listen_seconds) > 1 / source->sample_rate())
              discontinuity = true;
          } else if (std::abs((timestamp - first_timestamp) / 1e9 - received / source->sample_rate()) >
                     1.5 / source->sample_rate()) discontinuity = true;
          received += count;
        }
        source->stop();
        samples.resize(received);
        const bool valid = !discontinuity && received == target && source->overflow_count() == overflows;
        std::size_t crc_valid = 0;
        if (valid) {
          ++valid_visits;
          const auto decoded = droneid::dji::decode_samples(samples, {});
          for (const auto& packet : decoded) if (packet.crc_ok) ++crc_valid;
        }
        if (crc_valid) ++hit_visits;
        std::printf("{\"event\":\"dwell_result\",\"center_hz\":%.0f,\"valid_capture\":%s,"
                    "\"crc_valid\":%zu,\"samples\":%zu,\"extra_gap_before_tune_seconds\":%.9g,"
                    "\"processing_and_restart_gap_follows\":true}\n",
                    visit.center_hz, valid ? "true" : "false", crc_valid, received, gap_seconds);
      }
#endif
      std::printf("{\"event\":\"visit\",\"index\":%zu,\"center_hz\":%.0f,\"tune_seconds\":%.9f,"
                  "\"listen_seconds\":%.9f,\"end_seconds\":%.9f}\n", index, visit.center_hz,
                  visit.tune_seconds, visit.listen_seconds, visit.end_seconds);
      ++visits;
    }
    std::printf("{\"event\":\"summary\",\"planned_or_executed_visits\":%zu,\"valid_captured_visits\":%zu,"
                "\"crc_hit_visits\":%zu,\"empirical_visit_hit_fraction\":", visits, valid_visits, hit_visits);
    if (valid_visits) std::printf("%.9g", static_cast<double>(hit_visits) / valid_visits);
    else std::printf("null");
    std::puts("}");
    return stopped ? 130 : 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
