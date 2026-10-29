// src/ui/odid_worker.cpp — opt-in ASTM F3411 Open Drone ID (Wi-Fi Beacon / NAN) workers for the UI.
// Replay and live sources feed droneid::odid::OdidWifiStream; decoded frames become PacketRow /
// DroneTrack entries tagged source "ODID/ASTM". The DJI DroneID workers are untouched; these run
// only when SourceConfig::odid is set. Receive-only.
// GPL-3.0-or-later.
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "droneid/file_source.h"
#include "droneid/odid_wifi.h"
#include "receiver_worker.h"
#ifdef DRONEID_HAVE_UHD
#include "droneid/usrp_source.h"
#endif

namespace droneid::ui {
namespace {

constexpr const char* kSource = "ODID/ASTM";

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string stats_detail(const odid::WifiStats& s) {
  char text[256];
  std::snprintf(text, sizeof text,
                "ODID Wi-Fi: %llu ODID frames | 802.11 FCS ok %llu / fail %llu | non-ODID %llu | OFDM sync %llu | DSSS sync %llu",
                static_cast<unsigned long long>(s.odid_frames), static_cast<unsigned long long>(s.fcs_ok),
                static_cast<unsigned long long>(s.fcs_fail), static_cast<unsigned long long>(s.non_odid_frames),
                static_cast<unsigned long long>(s.ofdm_sync), static_cast<unsigned long long>(s.dsss_sync));
  return text;
}

// Merges successive frames per transmitter MAC so a frame without a Basic ID still maps to the
// identity seen earlier from the same radio, then publishes packet + track rows.
class OdidPublisher {
 public:
  explicit OdidPublisher(UiModel& model) : model_(model) {}

  void publish(const odid::OdidDetection& d) {
    auto& uas = by_mac_[odid::mac_string(d.frame.src)];
    odid::merge(uas, d.frame.uas);
    const std::string id = uas.primary_id().empty() ? "mac:" + odid::mac_string(d.frame.src) : uas.primary_id();
    const auto* basic = uas.primary_basic_id();
    const std::string transport = odid::to_string(d.frame.transport);
    char phy[48];
    std::snprintf(phy, sizeof phy, "%s %gM", odid::to_string(d.phy.phy), d.phy.rate_mbps);

    PacketRow p;
    p.time_ms = now_ms();
    p.source = kSource;
    p.sequence = d.frame.message_counter;
    p.serial = id;
    p.type = std::string(kSource) + " " + (basic ? odid::ua_type_name(basic->ua_type) : "UA type unknown");
    p.telemetry_parsed = true;
    p.crc_ok = d.phy.fcs_ok;
    p.payload_format = std::string(kSource) + " " + transport + " " + phy;
    p.inner_crc16 = "n/a (802.11 FCS OK)";
    static const char hex[] = "0123456789abcdef";
    for (const auto b : d.frame.payload) { p.payload_hex += hex[b >> 4]; p.payload_hex += hex[b & 15]; }
    // Position fields come from this frame only, never from a stale merged message.
    p.drone_valid = d.frame.uas.location && d.frame.uas.location->position_valid();
    if (d.frame.uas.location) {
      const auto& l = *d.frame.uas.location;
      p.drone_lat = l.lat;
      p.drone_lon = l.lon;
      p.altitude_m = l.alt_geo_m;
      p.height_m = l.height_m;
    }
    if (d.frame.uas.system && d.frame.uas.system->operator_position_valid()) {
      p.pilot_valid = true;
      p.pilot_lat = d.frame.uas.system->operator_lat;
      p.pilot_lon = d.frame.uas.system->operator_lon;
    }
    model_.add_packet(p);

    DroneTrack t;
    t.key = track_key(kSource, id);
    t.source = kSource;
    t.serial = id;
    t.type = p.type;
    t.generation = std::string(kSource) + " " + transport;
    t.corr = d.phy.quality;
    t.snr_db = d.phy.snr_db;
    t.decoded = true;
    t.last_ms = p.time_ms;
    model_.upsert_track(t);
  }

 private:
  UiModel& model_;
  std::map<std::string, odid::UasData> by_mac_;
};

odid::WifiDecodeOptions options_for(double rate, double freq_offset) {
  odid::WifiDecodeOptions o;
  o.sample_rate = rate;
  o.freq_offset = freq_offset;
  if (!std::isfinite(rate) || rate < 20e6 || rate > 250e6)
    throw std::invalid_argument("ODID Wi-Fi needs a 20..250 MSPS source (802.11 channels are 20 MHz wide)");
  if (std::abs(freq_offset) > rate / 2) throw std::invalid_argument("ODID frequency offset outside the sampled band");
  return o;
}

}  // namespace

void process_odid_capture(UiModel& model, const std::vector<cf32>& samples, double rate) {
  odid::OdidWifiStream stream(options_for(rate, 0));
  OdidPublisher publisher(model);
  for (std::size_t offset = 0; offset < samples.size(); offset += 262144) {
    const auto count = std::min(std::size_t(262144), samples.size()-offset);
    for (const auto& decoded : stream.feed(samples.data()+offset, count)) publisher.publish(decoded);
  }
  for (const auto& decoded : stream.feed(nullptr, 0, true)) publisher.publish(decoded);
}

void odid_replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop) {
  UsrpStatus status;
  status.state = UsrpStatus::State::Connecting;
  status.label = "Replay (ODID Wi-Fi): " + config.replay;
  model.set_status(status);
  try {
    FileSource source(config.replay, config.fmt, config.rate, config.center_freq);
    status.rate_hz = source.sample_rate();
    status.center_hz = source.center_freq();
    odid::OdidWifiStream stream(options_for(source.sample_rate(), config.freq_offset));
    OdidPublisher publisher(model);
    const uint64_t limit = config.secs > 0 ? static_cast<uint64_t>(config.secs * source.sample_rate()) : UINT64_MAX;
    std::vector<cf32> block(1 << 18);
    int64_t timestamp = 0;
    status.state = UsrpStatus::State::Connected;
    status.detail = "ODID Wi-Fi offline replay running";
    model.set_status(status);
    while (!stop.load() && status.samples_received < limit) {
      const auto count = source.recv(block.data(), std::min<uint64_t>(block.size(), limit - status.samples_received), timestamp);
      if (!count) break;
      status.samples_received += count;
      for (const auto& d : stream.feed(block.data(), count)) publisher.publish(d);
    }
    if (!stop.load())
      for (const auto& d : stream.feed(nullptr, 0, true)) publisher.publish(d);
    status.state = UsrpStatus::State::Disconnected;
    status.detail = (stop.load() ? "Replay stopped. " : "Replay complete (not live reception). ") + stats_detail(stream.stats());
    model.set_status(status);
  } catch (const std::exception& error) {
    status.state = UsrpStatus::State::Error;
    status.detail = error.what();
    model.set_status(status);
  }
}

void odid_live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop) {
  UsrpStatus status;
  status.state = UsrpStatus::State::Connecting;
  status.label = "X310 live (ODID Wi-Fi)";
  model.set_status(status);
#ifdef DRONEID_HAVE_UHD
  try {
    UsrpParams parameters;
    parameters.device_args = config.device;
    parameters.master_clock_rate = kOdidLiveMasterClock;
    parameters.sample_rate = kOdidLiveRate;
    parameters.bandwidth = kOdidLiveRate;
    parameters.center_freq = config.center_freq;
    parameters.gain_db = config.gain;
    parameters.antenna = config.antenna;
    UsrpSource source(parameters);
    if (std::abs(source.sample_rate() - kOdidLiveRate) > 1)
      throw std::runtime_error("unexpected ODID live sample rate (needs 200 MHz master clock / 10)");
    const auto options = options_for(source.sample_rate(), config.freq_offset);
    status.state = UsrpStatus::State::Connected;
    status.label = "X310 " + source.serial() + " (ODID Wi-Fi)";
    status.rate_hz = source.sample_rate();
    status.center_hz = source.center_freq();
    status.gain_db = config.gain;
    status.ref_locked = source.ref_locked();
    model.set_status(status);

    struct Chunk { std::vector<cf32> samples; bool gap = false; };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Chunk> queue;
    bool finished = false;
    std::string dsp_error;
    odid::WifiStats totals;
    std::thread processor([&] {
      OdidPublisher publisher(model);
      auto stream = std::make_unique<odid::OdidWifiStream>(options);
      odid::WifiStats done;
      for (;;) {
        Chunk chunk;
        {
          std::unique_lock<std::mutex> lock(mutex);
          ready.wait(lock, [&] { return finished || !queue.empty(); });
          if (stop.load() || (finished && queue.empty())) break;
          chunk = std::move(queue.front());
          queue.pop_front();
        }
        try {
          if (chunk.gap) {  // never stitch frames across a dropped/overflowed span
            for (const auto& d : stream->feed(nullptr, 0, true)) publisher.publish(d);
            done.add(stream->stats());
            stream = std::make_unique<odid::OdidWifiStream>(options);
          }
          for (const auto& d : stream->feed(chunk.samples.data(), chunk.samples.size())) publisher.publish(d);
          std::lock_guard<std::mutex> lock(mutex);
          totals = done;
          totals.add(stream->stats());
        } catch (const std::exception& error) {
          std::lock_guard<std::mutex> lock(mutex);
          dsp_error = error.what();
          return;
        }
      }
      if (!stop.load())
        for (const auto& d : stream->feed(nullptr, 0, true)) publisher.publish(d);
      std::lock_guard<std::mutex> lock(mutex);  // include the final flush in the reported counters
      totals = done;
      totals.add(stream->stats());
    });
    auto finish = [&] {
      { std::lock_guard<std::mutex> lock(mutex); finished = true; }
      ready.notify_all();
      processor.join();
    };
    // ~0.4 s of 20 MSPS backlog; beyond that the oldest chunk is dropped and a gap is flagged.
    constexpr std::size_t kMaxQueued = 128;
    bool pending_gap = false;
    try {
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
          pending_gap = true;
          ++status.discontinuities;
        }
        expected = timestamp + static_cast<int64_t>(std::llround(count * 1e9 / source.sample_rate()));
        previous_overflows = overflows;
        status.samples_received += count;
        status.overflows = overflows;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!dsp_error.empty()) throw std::runtime_error("DSP: " + dsp_error);
          if (queue.size() >= kMaxQueued) {
            queue.pop_front();
            ++status.dropped_windows;
            queue.front().gap = true;
          }
          queue.push_back({std::vector<cf32>(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(count)), pending_gap});
          pending_gap = false;
          status.detail = "RX + bounded ODID DSP queue; " + stats_detail(totals);
        }
        ready.notify_one();
        model.set_status(status);
      }
      source.stop();
      finish();
      if (!dsp_error.empty()) throw std::runtime_error("DSP: " + dsp_error);
      status.detail = "Live stream stopped. " + stats_detail(totals);
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

}  // namespace droneid::ui
