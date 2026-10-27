// src/ui/app.cpp — DJI DroneID operator UI (native, Dear ImGui + GLFW/OpenGL3).
//
// Reads ONLY backend APIs (decode_file / parse_frame; UsrpSource under UHD) and renders the
// §11 layout: USRP bar, drones table, decoded packets, direction compass, checklist panel.
// The render thread only reads UiModel snapshots; a worker thread drives decoding.
// Modes: default (window until closed), --frames N (render N frames then exit, for smoke tests),
// --check (no window: run the worker path once headless and exit).
// GPL-3.0-or-later.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <fstream>
#include <stdexcept>

#include <algorithm>

#include "droneid/aoa.h"
#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/rf_metrics.h"
#include "ui_model.h"
#include "receiver_worker.h"

#ifdef DRONEID_HAVE_UHD
#include "droneid/usrp_source.h"
#endif

#ifndef DRONEID_UI_CHECK_ONLY
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#endif

using namespace droneid;
using droneid::ui::UiModel;
using droneid::ui::UsrpStatus;

using Config = droneid::ui::SourceConfig;
using droneid::ui::replay_worker;
using droneid::ui::paired_replay_worker;
using droneid::ui::live_worker;
using droneid::ui::df_live_worker;

#ifndef DRONEID_UI_CHECK_ONLY
namespace {

void draw_usrp_bar(const UsrpStatus& s, bool& want_start, bool& want_stop) {
  ImGui::BeginChild("usrp_bar", ImVec2(0, 112), true);
  const char* dot = "o";
  ImVec4 col(0.6f, 0.6f, 0.6f, 1);
  switch (s.state) {
    case UsrpStatus::State::Connected:   col = ImVec4(0.2f, 0.8f, 0.3f, 1); break;
    case UsrpStatus::State::Connecting:  col = ImVec4(0.9f, 0.7f, 0.1f, 1); break;
    case UsrpStatus::State::Error:       col = ImVec4(0.9f, 0.2f, 0.2f, 1); break;
    default: break;
  }
  if (ImGui::Button("Connect")) want_start = true;
  ImGui::SameLine();
  if (ImGui::Button("Disconnect")) want_stop = true;
  ImGui::SameLine();
  if (ImGui::Button("Reconnect")) { want_stop = true; want_start = true; }
  ImGui::SameLine();
  ImGui::TextColored(col, "%s %s", dot, ui::to_string(s.state));
  ImGui::Text("%s   %.3f MSPS   %.4f GHz   gain %.0f dB   ref %s   overflows %llu",
              s.label.c_str(), s.rate_hz / 1e6, s.center_hz / 1e9, s.gain_db,
              s.ref_locked ? "locked" : "-", static_cast<unsigned long long>(s.overflows));
  if (!s.detail.empty()) ImGui::TextDisabled("%s", s.detail.c_str());
  ImGui::TextDisabled("Samples %llu | timestamp gaps %llu | dropped DSP windows %llu",
      static_cast<unsigned long long>(s.samples_received), static_cast<unsigned long long>(s.discontinuities),
      static_cast<unsigned long long>(s.dropped_windows));
  ImGui::EndChild();
}

void draw_drones(const std::vector<ui::DroneTrack>& tracks, const ui::RfSummary& rf) {
  ImGui::BeginChild("drones", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f), true);
  ImGui::SeparatorText("Drones detected");
  ImGui::TextDisabled("Unknown RF: %llu bursts | peak RSSI %.0f dBFS | last SNR %.0f dB | BW %.1f MHz",
                      static_cast<unsigned long long>(rf.bursts_total), rf.peak_rssi_dbfs,
                      rf.last_snr_db, rf.last_bw_hz / 1e6);
  ImGui::TextDisabled("(RF energy incl. Wi-Fi/baseline — not a drone count)");
  if (ImGui::BeginTable("tracks", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("serial");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("gen");
    ImGui::TableSetupColumn("corr");
    ImGui::TableSetupColumn("bearing");
    ImGui::TableSetupColumn("count");
    ImGui::TableHeadersRow();
    for (const auto& t : tracks) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::TextUnformatted(t.serial.empty() ? "(rf)" : t.serial.c_str());
      ImGui::TableNextColumn(); ImGui::TextUnformatted(t.type.c_str());
      ImGui::TableNextColumn(); ImGui::TextUnformatted(t.generation.c_str());
      ImGui::TableNextColumn(); ImGui::Text("%.2f", t.corr);
      ImGui::TableNextColumn();
      if (t.has_bearing) {
        ImGui::Text("%+.0f deg (+/-%.0f)%s", t.bearing_deg, t.bearing_uncertainty_deg,
                    t.bearing_ambiguous ? " [F/B?]" : "");
      } else {
        ImGui::TextDisabled("-");
      }
      ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(t.count));
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

void draw_packets(const std::vector<ui::PacketRow>& packets) {
  ImGui::BeginChild("packets", ImVec2(0, 0), true);
  ImGui::SeparatorText("Decoded packets");
  ImGui::TextDisabled("Outer-CRC-valid payloads; hover serial for format, inner CRC and hex. Unparsed rows have no telemetry.");
  if (ImGui::BeginTable("pkts", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("seq");
    ImGui::TableSetupColumn("serial");
    ImGui::TableSetupColumn("drone lat,lon");
    ImGui::TableSetupColumn("alt/ht");
    ImGui::TableSetupColumn("pilot lat,lon");
    ImGui::TableSetupColumn("home");
    ImGui::TableHeadersRow();
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
      const auto& p = *it;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (p.telemetry_parsed) ImGui::Text("%u", p.sequence);
      else ImGui::TextDisabled("CRC24 OK");
      ImGui::TableNextColumn(); ImGui::TextUnformatted(p.telemetry_parsed ? p.serial.c_str() : p.payload_format.c_str());
      if (ImGui::IsItemHovered() && !p.payload_hex.empty()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(560);
        ImGui::Text("Format: %s; inner CRC16: %s", p.payload_format.c_str(), p.inner_crc16.c_str());
        ImGui::TextUnformatted(p.payload_hex.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      if (!p.telemetry_parsed) {
        for (int column = 0; column < 4; ++column) {
          ImGui::TableNextColumn(); ImGui::TextDisabled("-");
        }
        continue;
      }
      ImGui::TableNextColumn(); ImGui::Text("%.6f, %.6f", p.drone_lat, p.drone_lon);
      ImGui::TableNextColumn(); ImGui::Text("%.0f/%.0f", p.altitude_m, p.height_m);
      ImGui::TableNextColumn();
      if (p.pilot_valid) ImGui::Text("%.6f, %.6f", p.pilot_lat, p.pilot_lon);
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      if (p.home_valid) ImGui::Text("%.6f, %.6f", p.home_lat, p.home_lon);
      else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

void draw_direction(const std::vector<ui::DroneTrack>& tracks, const ui::DfUiStatus& df,
                    bool& want_cal_splitter, bool& want_cal_ota, bool& want_cal_cancel) {
  ImGui::BeginChild("direction", ImVec2(0, 0), true);
  ImGui::SeparatorText("Direction (2-Element Broadside AoA)");

  ImVec4 state_col(0.6f, 0.6f, 0.6f, 1.0f);
  if (df.state == ui::DfUiState::Valid) state_col = ImVec4(0.2f, 0.85f, 0.35f, 1.0f);
  else if (df.state == ui::DfUiState::Calibrating || df.state == ui::DfUiState::Uncalibrated)
    state_col = ImVec4(0.95f, 0.75f, 0.15f, 1.0f);
  else if (df.state == ui::DfUiState::Stale || df.state == ui::DfUiState::Error)
    state_col = ImVec4(0.9f, 0.25f, 0.25f, 1.0f);

  ImGui::TextColored(state_col, "Cal: %s", ui::to_string(df.state));
  ImGui::SameLine();
  ImGui::TextDisabled("| ref: %s | age: %.0fs", df.reference_method.c_str(), df.cal_age_seconds);
  if (!df.reason.empty()) ImGui::TextDisabled("%s", df.reason.c_str());

  if (ImGui::SmallButton("Calibrate (Splitter)")) want_cal_splitter = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("Calibrate (OTA)")) want_cal_ota = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("Cancel/Invalidate")) want_cal_cancel = true;

  ImGui::TextDisabled("Rig: %s (epoch %llu) | %.3f GHz | orient %.0f deg",
                      df.rig_id.empty() ? "-" : df.rig_id.c_str(),
                      static_cast<unsigned long long>(df.config_epoch),
                      df.center_hz / 1e9, df.orientation_deg);
  ImGui::TextDisabled("Accepted %llu | Rejected %llu (coh %llu, disp %llu, clip %llu, ovlp %llu)",
                      static_cast<unsigned long long>(df.accepted_bearings),
                      static_cast<unsigned long long>(df.rejected_bearings),
                      static_cast<unsigned long long>(df.rejected_coherence),
                      static_cast<unsigned long long>(df.rejected_dispersion),
                      static_cast<unsigned long long>(df.rejected_clipping),
                      static_cast<unsigned long long>(df.rejected_overlap));

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float r = std::max(30.0f, std::min(avail.x, avail.y - 36.0f) * 0.44f);
  const ImVec2 c(p0.x + avail.x * 0.5f, p0.y + r + 8);
  dl->AddCircle(c, r, IM_COL32(120, 120, 120, 255), 48);
  dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(70, 70, 70, 255));
  dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(70, 70, 70, 255));

  bool any = false;
  if (df.state == ui::DfUiState::Valid) {
    for (const auto& t : tracks) {
      if (!t.has_bearing) continue;
      any = true;
      const float q = std::clamp(t.bearing_quality, 0.25f, 1.0f);
      const double a = (t.bearing_deg - 90.0) * 3.14159265358979323846 / 180.0;  // 0deg = broadside up
      const ImVec2 tip(c.x + static_cast<float>(std::cos(a)) * r * q,
                       c.y + static_cast<float>(std::sin(a)) * r * q);
      dl->AddLine(c, tip, IM_COL32(60, 210, 95, static_cast<int>(255 * q)), 2.5f);
      if (t.bearing_ambiguous) {
        const double ma = (t.mirrored_deg - 90.0) * 3.14159265358979323846 / 180.0;
        const ImVec2 mtip(c.x + static_cast<float>(std::cos(ma)) * r * q * 0.85f,
                          c.y + static_cast<float>(std::sin(ma)) * r * q * 0.85f);
        dl->AddLine(c, mtip, IM_COL32(220, 170, 50, static_cast<int>(180 * q)), 1.5f);
      }
    }
  }
  ImGui::Dummy(ImVec2(avail.x, 2 * r + 16));
  if (!any) {
    ImGui::TextDisabled("No active validated bearings (%s)", ui::to_string(df.state));
  } else {
    ImGui::TextDisabled("Solid green: broadside angle; amber: front/back mirror ambiguity.");
  }
  ImGui::EndChild();
}

int run_window(UiModel& model, const Config& cfg) {
  glfwSetErrorCallback([](int e, const char* d) { std::fprintf(stderr, "glfw %d: %s\n", e, d); });
  if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed (no display?)\n"); return 2; }
  const char* glsl = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  GLFWwindow* win = glfwCreateWindow(1280, 800, "DroneID Receiver", nullptr, nullptr);
  if (!win) { std::fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 2; }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init(glsl);

  // Worker lifecycle, driven by Connect/Disconnect/Reconnect buttons.
  std::atomic<bool> stop{false};
  std::thread worker;
  bool running = false;
  std::atomic<bool> worker_done{true};
  bool restart_requested = false;
  Config active_cfg = cfg;

  auto stop_worker = [&]() {
    if (!running) return;
    stop.store(true);
  };
  auto start_worker = [&]() {
    if (running) { stop_worker(); restart_requested = true; return; }
    if (!active_cfg.pair_manifest.empty() || !active_cfg.replay_ch1.empty()) {
      stop.store(false);
      worker_done.store(false);
      worker = std::thread([&] { paired_replay_worker(model, active_cfg, stop); worker_done.store(true); });
      running = true;
      return;
    }
    if (active_cfg.df) {              // live direction-finding (two-channel) source
#ifdef DRONEID_HAVE_UHD
      if (active_cfg.device.empty() || active_cfg.df_cal.empty()) {
        UsrpStatus s;
        s.state = UsrpStatus::State::Error;
        s.label = "DF setup";
        s.detail = "DF needs --device ARGS and --df-cal FILE";
        model.set_status(s);
        return;
      }
      stop.store(false);
      worker_done.store(false);
      worker = std::thread([&] { df_live_worker(model, active_cfg, stop); worker_done.store(true); });
      running = true;
#else
      UsrpStatus s;
      s.state = UsrpStatus::State::Error;
      s.label = "No UHD";
      s.detail = "rebuild with -DDRONEID_WITH_UHD=ON for DF";
      model.set_status(s);
#endif
      return;
    }
    if (!active_cfg.device.empty()) { // live radio takes priority when a device is given
#ifdef DRONEID_HAVE_UHD
      stop.store(false);
      worker_done.store(false);
      worker = std::thread([&] { live_worker(model, active_cfg, stop); worker_done.store(true); });
      running = true;
#else
      UsrpStatus s;
      s.state = UsrpStatus::State::Error;
      s.label = "No UHD";
      s.detail = "rebuild with -DDRONEID_WITH_UHD=ON for live radio";
      model.set_status(s);
#endif
      return;
    }
    if (active_cfg.replay.empty()) {
      UsrpStatus s;
      s.state = UsrpStatus::State::Error;
      s.label = "No source";
      s.detail = "pass --device ARGS (live) or --replay FILE";
      model.set_status(s);
      return;
    }
    stop.store(false);
    worker_done.store(false);
    worker = std::thread([&] { replay_worker(model, active_cfg, stop); worker_done.store(true); });
    running = true;
  };
  start_worker();  // auto-connect on launch

  int frame = 0;
  bool screenshot_failed = false;
  while (!glfwWindowShouldClose(win)) {
    if (running && worker_done.load()) {
      worker.join();
      running = false;
      if (stop.load()) {
        auto status = model.status();
        status.state = UsrpStatus::State::Disconnected;
        model.set_status(status);
        model.invalidate_bearings(ui::DfUiState::Disconnected, "Disconnected by operator");
      }
      if (restart_requested) { restart_requested = false; start_worker(); }
    }
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    bool want_start = false, want_stop = false;
    draw_usrp_bar(model.status(), want_start, want_stop);
    if (want_stop) {
      restart_requested = false;
      stop_worker();
      model.invalidate_bearings(ui::DfUiState::Disconnected, "Disconnected by operator");
    }
    if (want_start) start_worker();

    const auto tracks = model.tracks();
    const auto packets = model.packets();
    const auto rf = model.rf_summary();
    const auto df = model.df_status();
    const float dir_w = 380.0f;
    const float left_w = std::max(220.0f, ImGui::GetContentRegionAvail().x - dir_w - 8);

    ImGui::BeginChild("left", ImVec2(left_w, 0), false);
    draw_drones(tracks, rf);
    draw_packets(packets);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("mid", ImVec2(0, 0), false);
    bool want_cal_splitter = false, want_cal_ota = false, want_cal_cancel = false;
    draw_direction(tracks, df, want_cal_splitter, want_cal_ota, want_cal_cancel);
    if (want_cal_cancel) {
      model.invalidate_bearings(ui::DfUiState::Uncalibrated,
                                "Calibration canceled/invalidated by operator");
    } else if (want_cal_splitter || want_cal_ota) {
      active_cfg.df_ref = want_cal_ota ? "ota_broadside" : "splitter";
      if (active_cfg.df_cal_samples == 0) active_cfg.df_cal_samples = 4096;
      start_worker();
    }
    ImGui::EndChild();

    ImGui::End();

    ImGui::Render();
    int w, h; glfwGetFramebufferSize(win, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.09f, 0.10f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!cfg.screenshot.empty() && cfg.frames > 0 && frame + 1 == cfg.frames && w > 0 && h > 0) {
      std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 3);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
      std::ofstream output(cfg.screenshot, std::ios::binary);
      output << "P6\n" << w << " " << h << "\n255\n";
      for (int row = h - 1; row >= 0; --row)
        output.write(reinterpret_cast<const char*>(pixels.data() + static_cast<std::size_t>(row) * w * 3), w * 3);
      if (!output) {
        screenshot_failed = true;
        std::fprintf(stderr, "Screenshot write failed: %s\n", cfg.screenshot.c_str());
      }
    }
    glfwSwapBuffers(win);

    if (cfg.frames >= 0 && ++frame >= cfg.frames) break;
  }

  stop_worker();
  if (worker.joinable()) worker.join();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return screenshot_failed ? 1 : 0;
}

}  // namespace
#endif  // DRONEID_UI_CHECK_ONLY

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s [source] [--checklist progress.md] [--frames N] [--screenshot FILE.ppm] [--check]\n"
      "  replay source: --replay FILE --format sc16|cf32 --rate HZ [--auto-center]\n"
      "                 [--freq-offset HZ] [--secs S]\n"
      "  paired DF replay: --pair-manifest FILE | (--replay CH0 --replay-ch1 CH1)\n"
      "                    [--df-cal FILE | --df-cal-samples N] [--df-ref splitter|ota_broadside]\n"
      "                    [--df-spacing-m M] [--df-jsonl FILE]\n"
      "  live source:   --device ARGS [--freq HZ] [--gain dB] [--ant NAME]  (needs a UHD build)\n"
      "  direction find: --df --df-cal FILE  (live hardware DF disabled until DF-5 bench approval)\n", p);
}

int main(int argc, char** argv) {
  try {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
      return argv[++i];
    };
    auto number = [&]() {
      const auto value = next();
      std::size_t used = 0;
      const double parsed = std::stod(value, &used);
      if (used != value.size() || !std::isfinite(parsed)) throw std::invalid_argument("invalid " + a);
      return parsed;
    };
    if (a == "--replay") cfg.replay = next();
    else if (a == "--replay-ch1") cfg.replay_ch1 = next();
    else if (a == "--pair-manifest") cfg.pair_manifest = next();
    else if (a == "--format") { if (!parse_format(next(), cfg.fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate") cfg.rate = number();
    else if (a == "--secs") cfg.secs = number();
    else if (a == "--auto-center") cfg.auto_center = true;
    else if (a == "--freq-offset") cfg.freq_offset = number();
    else if (a == "--device") cfg.device = next();
    else if (a == "--freq") cfg.center_freq = number();
    else if (a == "--gain") cfg.gain = number();
    else if (a == "--ant") cfg.antenna = next();
    else if (a == "--df") cfg.df = true;
    else if (a == "--df-cal") cfg.df_cal = next();
    else if (a == "--df-ref") cfg.df_ref = next();
    else if (a == "--df-cal-samples") {
      const double s = number();
      if (s < 0 || s > 1e7 || std::floor(s) != s) throw std::invalid_argument("invalid --df-cal-samples");
      cfg.df_cal_samples = static_cast<uint64_t>(s);
    }
    else if (a == "--df-spacing-m") cfg.df_spacing_m = number();
    else if (a == "--df-jsonl") cfg.df_jsonl = next();
    else if (a == "--checklist") cfg.checklist = next();
    else if (a == "--frames") {
      const double frames = number();
      if (frames < 1 || frames > 1000000 || std::floor(frames) != frames) throw std::invalid_argument("invalid frame count");
      cfg.frames = static_cast<int>(frames);
    }
    else if (a == "--screenshot") cfg.screenshot = next();
    else if (a == "--check") cfg.check = true;
    else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else { usage(argv[0]); return 2; }
  }

  if (cfg.rate < 10e6 || cfg.rate > 250e6 || cfg.secs < 0 || cfg.secs > 86400 ||
      cfg.center_freq < 1e6 || cfg.center_freq > 6e9 ||
      (cfg.auto_center && cfg.freq_offset != 0) || (!cfg.screenshot.empty() && (cfg.frames < 1 || cfg.check)))
    throw std::invalid_argument("invalid rate, duration, frequency, centering or screenshot options");
  if (cfg.check && cfg.device.empty() && cfg.replay.empty() && cfg.pair_manifest.empty())
    throw std::invalid_argument("--check requires a source");
  if (cfg.df && cfg.pair_manifest.empty() && cfg.replay_ch1.empty() &&
      (cfg.device.empty() || cfg.df_cal.empty()))
    throw std::invalid_argument("DF requires device and calibration, or paired replay input");
  UiModel model;
  model.load_checklist_file(cfg.checklist);

  if (cfg.check) {
    if (!cfg.device.empty() && cfg.secs == 0) cfg.secs = 2;
    // Headless self-check. Replay runs to completion; a live device is time-boxed (--secs, def 2 s).
    std::atomic<bool> stop{false};
    std::thread worker;
    if (!cfg.pair_manifest.empty() || !cfg.replay_ch1.empty()) {
      worker = std::thread([&] { paired_replay_worker(model, cfg, stop); });
    } else if (!cfg.device.empty()) {
#ifdef DRONEID_HAVE_UHD
      if (cfg.df) worker = std::thread([&] { df_live_worker(model, cfg, stop); });
      else worker = std::thread([&] { live_worker(model, cfg, stop); });
#else
      std::fprintf(stderr, "--device needs a UHD build (-DDRONEID_WITH_UHD=ON)\n");
      return 1;
#endif
    } else if (!cfg.replay.empty()) {
      worker = std::thread([&] { replay_worker(model, cfg, stop); });
    }
    if (worker.joinable()) worker.join();
    const auto st = model.status();
    const auto rf = model.rf_summary();
    std::printf("check: status=%s packets=%zu tracks=%zu rf_bursts=%llu checklist_groups=%zu overflows=%llu samples=%llu gaps=%llu dropped_windows=%llu\n",
                ui::to_string(st.state), model.packets().size(), model.tracks().size(),
                static_cast<unsigned long long>(rf.bursts_total), model.checklist().size(),
                static_cast<unsigned long long>(st.overflows), static_cast<unsigned long long>(st.samples_received),
                static_cast<unsigned long long>(st.discontinuities), static_cast<unsigned long long>(st.dropped_windows));
    for (const auto& track : model.tracks()) {
      std::printf("  track: serial=%s count=%llu corr=%.3f", track.serial.c_str(),
                  static_cast<unsigned long long>(track.count), track.corr);
      if (track.has_bearing) {
        std::printf(" bearing_deg=%.4f mirrored_deg=%.4f uncertainty_deg=%.4f ambiguous=%s",
                    track.bearing_deg, track.mirrored_deg, track.bearing_uncertainty_deg,
                    track.bearing_ambiguous ? "true" : "false");
      }
      std::printf("\n");
    }
    for (const auto& packet : model.packets())
      std::printf("  packet: telemetry=%s frame=%s format=%s inner_crc16=%s\n",
                  packet.telemetry_parsed ? "parsed" : "unsupported", packet.payload_hex.c_str(),
                  packet.payload_format.c_str(), packet.inner_crc16.c_str());
    if (!cfg.pair_manifest.empty() || !cfg.replay_ch1.empty() || cfg.df) {
      const auto df = model.df_status();
      std::printf("  df_status: state=%s accepted=%llu rejected=%llu dropped_pairs=%llu reason=%s\n",
                  ui::to_string(df.state),
                  static_cast<unsigned long long>(df.accepted_bearings),
                  static_cast<unsigned long long>(df.rejected_bearings),
                  static_cast<unsigned long long>(df.dropped_pairs),
                  df.reason.c_str());
      for (const auto& ev : model.df_events())
        std::fputs(("  df_event: " + ev.to_jsonl()).c_str(), stdout);
    }
    if (!st.detail.empty()) std::printf("  detail: %s\n", st.detail.c_str());
    return st.state == UsrpStatus::State::Error ? 1 : 0;
  }

#ifdef DRONEID_UI_CHECK_ONLY
  std::fprintf(stderr, "built without GUI backend; use --check\n");
  return 0;
#else
  return run_window(model, cfg);  // owns the worker; Connect/Disconnect/Reconnect drive it
#endif
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
  }
}
