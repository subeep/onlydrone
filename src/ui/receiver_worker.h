#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "droneid/aoa.h"
#include "droneid/types.h"
#include "droneid/sweep.h"
#include "ui_model.h"

namespace droneid::ui {

struct SourceConfig {
  std::string replay;
  std::string replay_ch1;
  std::string pair_manifest;
  SampleFormat fmt = SampleFormat::sc16;
  double rate = 15.36e6;
  double secs = 0;
  bool auto_center = false;
  double freq_offset = 0;
  std::string checklist = "progress.md";
  int frames = -1;
  bool check = false;
  std::string screenshot;
  std::string device;
  double center_freq = 2.4595e9;
  double gain = 30;
  std::string antenna = "RX2";
  bool df = false;
  std::string df_cal;
  std::string df_ref = "splitter";
  uint64_t df_cal_samples = 0;
  double df_spacing_m = 0.05;
  std::string df_jsonl;
  // Opt-in ASTM F3411 Open Drone ID over Wi-Fi (Beacon/NAN). Off by default; when on, the source
  // is decoded as 802.11 instead of DJI DroneID and live reception tunes a 20 MSPS Wi-Fi channel.
  bool odid = false;
  bool sweep = false;
  std::string sweep_profile = "balanced";
  bool sweep_include_52 = false;
  bool sweep_all_wifi = false;
  std::uint64_t sweep_cycles = 0;  // zero runs until stopped
  double sweep_settle = .05;
  double sweep_jitter = .04;
  std::string sweep_jsonl;
};

// ODID Wi-Fi live profile: 200 MHz master clock / 10 = 20 MSPS, the native 802.11 OFDM rate.
constexpr double kOdidLiveRate = 20e6;
constexpr double kOdidLiveMasterClock = 200e6;
constexpr double kOdidDefaultCenter = 2.437e9;  // Wi-Fi channel 6

struct SampleWindow {
  std::vector<cf32> samples;
  std::size_t accept_before = 0;
  uint64_t start_sample = 0;
};

// One immutable buffer shared by independent packet and RF/O4 consumers. Caller
// supplies synchronization. A slow RF lane never evicts a packet-lane window.
class LiveWindowFanout {
 public:
  explicit LiveWindowFanout(std::size_t capacity) : capacity_(capacity) {
    if(!capacity) throw std::invalid_argument("live queue capacity must be positive");
  }
  void push(SampleWindow window) {
    auto shared=std::make_shared<const SampleWindow>(std::move(window));
    for(std::size_t lane=0;lane<2;++lane) {
      if(queues_[lane].size()==capacity_) {queues_[lane].pop_front();++dropped_[lane];}
      queues_[lane].push_back(shared);
    }
  }
  bool empty(std::size_t lane) const {return queues_.at(lane).empty();}
  std::shared_ptr<const SampleWindow> pop(std::size_t lane) {
    auto& q=queues_.at(lane);
    if(q.empty()) return {};
    auto result=std::move(q.front());q.pop_front();return result;
  }
  std::uint64_t dropped(std::size_t lane) const {return dropped_.at(lane);}
 private:
  std::size_t capacity_;
  std::array<std::deque<std::shared_ptr<const SampleWindow>>,2> queues_;
  std::array<std::uint64_t,2> dropped_{};
};

class WindowAssembler {
 public:
  explicit WindowAssembler(std::size_t stride = 1u << 20, std::size_t overlap = 12000);
  std::vector<SampleWindow> feed(const cf32* samples, std::size_t count, bool final = false);
  void reset();
 private:
  std::size_t stride_, overlap_;
  uint64_t start_ = 0;
  std::vector<cf32> buffer_;
};

// DF-3: Paired window keeping both channels strictly lockstep.
struct PairedSampleWindow {
  std::vector<cf32> ch0;
  std::vector<cf32> ch1;
  std::size_t accept_before = 0;
  uint64_t start_sample = 0;
  int64_t first_device_timestamp_ns = 1;
  bool after_drop_or_gap = false;
};

class PairedWindowAssembler {
 public:
  explicit PairedWindowAssembler(std::size_t stride = 1u << 20, std::size_t overlap = 12000);
  std::vector<PairedSampleWindow> feed(const cf32* ch0, const cf32* ch1, std::size_t count,
                                       int64_t base_device_ns, double sample_rate,
                                       bool final = false);
  void reset();
 private:
  std::size_t stride_, overlap_;
  uint64_t start_ = 0;
  std::vector<cf32> buf0_;
  std::vector<cf32> buf1_;
  bool gap_pending_ = false;
};

// DF-3: Bounded paired queue. Any overflow drops the oldest pair as a unit and flags a gap.
class BoundedPairedQueue {
 public:
  explicit BoundedPairedQueue(std::size_t capacity = 3) : capacity_(capacity ? capacity : 1) {}
  void push(PairedSampleWindow window);
  bool pop(PairedSampleWindow& out);
  bool empty() const { return queue_.empty(); }
  std::size_t size() const { return queue_.size(); }
  uint64_t dropped_pairs() const { return dropped_pairs_; }
 private:
  std::size_t capacity_;
  std::deque<PairedSampleWindow> queue_;
  uint64_t dropped_pairs_ = 0;
  bool drop_flag_ = false;
};

void replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void paired_replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void df_live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void odid_replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void odid_live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void sweep_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void process_odid_capture(UiModel& model, const std::vector<cf32>& samples, double rate);
void process_sweep_capture(UiModel& model, const std::vector<cf32>& samples, SweepMode mode, double rate);

}  // namespace droneid::ui
