#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "droneid/types.h"
#include "ui_model.h"

namespace droneid::ui {

struct SourceConfig {
  std::string replay;
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
};

struct SampleWindow {
  std::vector<cf32> samples;
  std::size_t accept_before = 0;
  uint64_t start_sample = 0;
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

void replay_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);
void df_live_worker(UiModel& model, SourceConfig config, std::atomic<bool>& stop);

}
