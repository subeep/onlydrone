#include "receiver_args.h"
#include "droneid/sweep.h"
#include "receiver_worker.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>
#include <chrono>

namespace { volatile std::sig_atomic_t interrupted = 0; void interrupt(int) { interrupted = 1; } }
int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc,argv,{"--profile","--cycles","--device","--gain","--ant","--settle","--jitter","--output"},
                      {"--live","--include-52","--all-wifi","--help"});
    if (args.has("--help")) {
      std::puts("droneid_sweep [--profile fast|balanced|thorough] [--include-52] [--all-wifi]\n"
                "  [--cycles N] [--settle S] [--jitter S] [--live --device ARGS --gain DB --ant RX2 --output FILE]\n"
                "Default is a plan only; live defaults to one cycle. --cycles 0 runs until Ctrl+C.\n"
                "20 MSPS / 200 MHz / sc16. Listening is paused during processing; logs record gaps.");
      return 0;
    }
    if (!args.files.empty()) throw std::invalid_argument("unexpected positional argument");
    droneid::ui::SourceConfig cfg;
    cfg.sweep = true; cfg.sweep_profile = args.get("--profile","balanced");
    cfg.sweep_include_52 = args.has("--include-52"); cfg.sweep_all_wifi = args.has("--all-wifi");
    cfg.sweep_settle = args.number("--settle",.05); cfg.sweep_jitter = args.number("--jitter",.04);
    cfg.device = args.get("--device","addr=192.168.10.2"); cfg.gain = args.number("--gain",20); cfg.antenna = args.get("--ant","RX2");
    cfg.sweep_jsonl = args.get("--output","-");
    const double cycles = args.number("--cycles",1);
    if (cycles < 0 || cycles > 1000 || std::floor(cycles)!=cycles || cfg.gain < 0 || cfg.gain > 100)
      throw std::invalid_argument("cycles must be 0..1000 integer, gain 0..100 dB");
    cfg.sweep_cycles = static_cast<uint64_t>(cycles);
    droneid::SweepPlan plan(cfg.sweep_profile,cfg.sweep_include_52,cfg.sweep_all_wifi,cfg.sweep_settle,cfg.sweep_jitter);
    std::printf("{\"event\":\"sweep_plan\",\"profile\":\"%s\",\"steps\":%zu,\"sample_rate\":20000000,"
                "\"minimum_cycle_before_processing\":%.6f,\"maximum_cycle_before_processing\":%.6f,\"catch_probability\":null}\n",
                cfg.sweep_profile.c_str(),plan.size(),plan.minimum_cycle_seconds(),plan.maximum_cycle_seconds_before_processing());
    if (!args.has("--live")) {
      for (size_t i = 0; i < plan.size(); ++i) {
        auto step = plan.step(i);
        std::printf("{\"event\":\"planned_dwell\",\"index\":%zu,\"center_hz\":%.0f,\"mode\":\"%s\",\"dwell_seconds\":%.6f,\"settle_seconds\":%.6f}\n",
                    i,step.center_hz,droneid::to_string(step.mode),step.dwell_seconds,cfg.sweep_settle);
      }
      return 0;
    }
    std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
    droneid::ui::UiModel model;
    std::atomic<bool> stop{false}, done{false};
    std::thread worker([&]{ droneid::ui::sweep_worker(model,cfg,stop); done.store(true); });
    while (!done.load()) {
      if (interrupted) stop.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    worker.join();
    auto s=model.status();
    std::fprintf(stderr,"%s: %s\n",droneid::ui::to_string(s.state),s.detail.c_str());
    return s.state==droneid::ui::UsrpStatus::State::Error ? 1 : interrupted ? 130 : 0;
  } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
