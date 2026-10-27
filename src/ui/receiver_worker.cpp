#include "receiver_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <thread>

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

}

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

void replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop) {
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
}

}
