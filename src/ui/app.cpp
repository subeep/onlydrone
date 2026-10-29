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
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#include <algorithm>

#include "droneid/aoa.h"
#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/rf_metrics.h"
#include "ui_model.h"
#include "receiver_worker.h"
#include "map_state.h"
#ifndef DRONEID_UI_CHECK_ONLY
#include "map_view.h"
#endif

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

struct SourceEditor {
  char path[32768]{};
  char device[1024]{};
  char antenna[64]{};
  int source = 0, format = 1;
  double rate = 15.36, center = 2459.5, gain = 30, seconds = 0;
  bool auto_center = false, odid = false;
  int sweep_profile = 1;
  bool include_52 = false, all_wifi = false;
  char sweep_log[1024] = "sweep-events.jsonl";
  explicit SourceEditor(const Config& cfg) {
    std::snprintf(path, sizeof path, "%s", cfg.replay.c_str());
    std::snprintf(device, sizeof device, "%s", cfg.device.empty() ? "addr=192.168.10.2" : cfg.device.c_str());
    std::snprintf(antenna, sizeof antenna, "%s", cfg.antenna.c_str());
    source = cfg.sweep ? 2 : !cfg.device.empty(); format = static_cast<int>(cfg.fmt);
    sweep_profile = cfg.sweep_profile == "fast" ? 0 : cfg.sweep_profile == "thorough" ? 2 : 1;
    include_52 = cfg.sweep_include_52; all_wifi = cfg.sweep_all_wifi;
    if (!cfg.sweep_jsonl.empty()) std::snprintf(sweep_log,sizeof sweep_log,"%s",cfg.sweep_jsonl.c_str());
    rate = cfg.rate / 1e6; center = cfg.center_freq / 1e6; gain = cfg.gain;
    seconds = cfg.secs; auto_center = cfg.auto_center; odid = cfg.odid;
  }
  void draw(bool running, bool paired) {
    if (!ImGui::CollapsingHeader("Source settings", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (paired) { ImGui::TextWrapped("Paired/DF source configured by command line. Restart without DF options to change source here."); return; }
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(180);
    ImGui::Combo("Source", &source, "IQ replay\0Live X310\0Automatic sweep\0");
    ImGui::SameLine();
    ImGui::BeginDisabled(source == 2);
    if (ImGui::Checkbox("Wi-Fi Remote ID", &odid) && odid) { rate = 20; center = 2437; auto_center = false; }
    ImGui::EndDisabled();
    if (source == 2) {
      ImGui::SetNextItemWidth(130); ImGui::Combo("Sweep profile", &sweep_profile, "fast\0balanced\0thorough\0");
      ImGui::SameLine(); ImGui::Checkbox("Include 5.2 GHz", &include_52);
      ImGui::SameLine(); ImGui::Checkbox("Wi-Fi channels 1-13", &all_wifi);
      ImGui::SetNextItemWidth(400); ImGui::InputText("Evidence JSONL", sweep_log, sizeof sweep_log);
      ImGui::TextWrapped("Interleaved DJI / O4 / Wi-Fi RID. RX pauses during processing; logs report blind intervals. DF unavailable.");
    }
    if (source == 0) {
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160);
      ImGui::InputText("IQ file", path, sizeof path);
#ifdef _WIN32
      ImGui::SameLine();
      if (ImGui::Button("Browse...")) {
        wchar_t selected[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof dialog;
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFilter = L"IQ recordings\0*.iq;*.cf32;*.sc16;*.sc8;*.bin\0All files\0*.*\0";
        dialog.lpstrFile = selected; dialog.nMaxFile = 32768;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
          if (!WideCharToMultiByte(CP_UTF8, 0, selected, -1, path, sizeof path, nullptr, nullptr)) path[0] = 0;
        }
      }
#endif
      ImGui::SetNextItemWidth(110);
      ImGui::Combo("Format", &format, "cf32\0sc16\0sc8\0");
      ImGui::SameLine(); ImGui::Checkbox("Auto-center DJI bursts", &auto_center);
      ImGui::TextDisabled("Raw interleaved I/Q; .meta sidecar overrides format/rate. Extract HDF5/RAR first.");
    } else {
      ImGui::SetNextItemWidth(400); ImGui::InputText("Device arguments", device, sizeof device);
      ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputText("Antenna", antenna, sizeof antenna);
    }
    if (source) rate = odid || source == 2 ? 20 : 15.36;
    ImGui::BeginDisabled(source != 0);
    ImGui::SetNextItemWidth(100); ImGui::InputDouble("MSPS", &rate, 0, 0, "%.3f");
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(source == 2);
    ImGui::SetNextItemWidth(110); ImGui::InputDouble("Center MHz", &center, 0, 0, "%.3f");
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputDouble("Gain dB", &gain, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputDouble("Seconds (0=all)", &seconds, 0, 0, "%.2f");
    ImGui::EndDisabled();
    if (running) ImGui::TextDisabled("Disconnect before changing source settings.");
  }
  void apply(Config& cfg) const {
    if (!cfg.pair_manifest.empty() || !cfg.replay_ch1.empty() || cfg.df) return;
    if (!std::isfinite(rate) || rate < 10 || rate > 250 || !std::isfinite(center) || center < 1 || center > 6000 ||
        !std::isfinite(gain) || gain < 0 || gain > 100 || !std::isfinite(seconds) || seconds < 0 || seconds > 86400 ||
        (source != 2 && odid && (rate < 20 || auto_center))) throw std::invalid_argument("Invalid source settings: rate 10-250 MSPS, center 1-6000 MHz; Wi-Fi RID needs >=20 MSPS and no auto-center.");
    if (source && (!device[0] || !antenna[0])) throw std::invalid_argument("Enter X310 device arguments and antenna.");
    cfg.device = source ? device : ""; cfg.replay = source ? "" : path;
    cfg.fmt = static_cast<SampleFormat>(format); cfg.rate = rate * 1e6;
    cfg.center_freq = center * 1e6; cfg.gain = gain; cfg.secs = seconds;
    cfg.antenna = antenna; cfg.odid = odid && source != 2; cfg.auto_center = source == 0 && auto_center;
    cfg.sweep = source == 2;
    cfg.sweep_profile = sweep_profile == 0 ? "fast" : sweep_profile == 2 ? "thorough" : "balanced";
    cfg.sweep_include_52 = include_52; cfg.sweep_all_wifi = all_wifi; cfg.sweep_jsonl = sweep_log;
  }
};

void draw_usrp_bar(const UsrpStatus& s, bool& want_start, bool& want_stop) {
  ImGui::BeginChild("usrp_bar", ImVec2(0, s.sweep_visits ? 130 : 112), true);
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
  ImGui::TextDisabled("Samples %llu | timestamp gaps %llu | DSP drops %llu (RF/O4 %llu)",
      static_cast<unsigned long long>(s.samples_received), static_cast<unsigned long long>(s.discontinuities),
      static_cast<unsigned long long>(s.dropped_windows),static_cast<unsigned long long>(s.dropped_rf_windows));
  if (s.sweep_visits) ImGui::Text("Sweep visits %llu | valid %llu", static_cast<unsigned long long>(s.sweep_visits),
                                static_cast<unsigned long long>(s.sweep_valid_visits));
  ImGui::EndChild();
}

void draw_drones(const std::vector<ui::DroneTrack>& tracks, const ui::RfSummary& rf) {
  ImGui::BeginChild("drones", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f), true);
  ImGui::SeparatorText("Drones detected");
  ImGui::TextDisabled("Unknown RF: %llu bursts | peak RSSI %.0f dBFS | last SNR %.0f dB | BW %.1f MHz",
                      static_cast<unsigned long long>(rf.bursts_total), rf.peak_rssi_dbfs,
                      rf.last_snr_db, rf.last_bw_hz / 1e6);
  ImGui::TextDisabled("(RF energy incl. Wi-Fi/baseline — not a drone count)");
  ImGui::TextWrapped("Experimental O4-like structure: %llu / %llu windows; identity unverified",
      static_cast<unsigned long long>(rf.o4_candidate_windows),
      static_cast<unsigned long long>(rf.o4_windows));
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

void draw_packets(const std::vector<ui::PacketRow>& packets, ui::PacketSelection& selection) {
  ImGui::BeginChild("packets", ImVec2(0, 0), true);
  ImGui::SeparatorText("Decoded packets");
  ImGui::TextDisabled("Click a packet row for its information and map. Unparsed rows have no telemetry.");
  if (std::any_of(packets.begin(), packets.end(), [](const ui::PacketRow& p) { return !p.source.empty(); }))
    ImGui::TextDisabled("ODID/ASTM rows: 802.11 FCS-valid F3411 Remote ID; seq = message counter, pilot = operator location.");
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
      const std::string row_label = (p.telemetry_parsed ? std::to_string(p.sequence) : "CRC OK") +
          "##packet_" + std::to_string(p.packet_id);
      if (ImGui::Selectable(row_label.c_str(), selection.packet && selection.packet->packet_id==p.packet_id,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        selection.select(p);
        std::fprintf(stderr,"ui_selection: packet=%llu drone_location=%s center=%.7f,%.7f zoom=%d\n",
            static_cast<unsigned long long>(p.packet_id),ui::packet_drone_location(p)?"valid":"unavailable",
            selection.lat,selection.lon,selection.zoom);
      }
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
      if (ui::packet_drone_location(p)) {
        ImGui::TableNextColumn(); ImGui::Text("%.6f, %.6f", p.drone_lat, p.drone_lon);
        ImGui::TableNextColumn(); ImGui::Text("%.0f/%.0f", p.altitude_m, p.height_m);
      } else {
        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
      }
      ImGui::TableNextColumn();
      if (p.crc_ok && p.pilot_valid && ui::valid_location(p.pilot_lat,p.pilot_lon)) ImGui::Text("%.6f, %.6f", p.pilot_lat, p.pilot_lon);
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      if (p.crc_ok && p.home_valid && ui::valid_location(p.home_lat,p.home_lon)) ImGui::Text("%.6f, %.6f", p.home_lat, p.home_lon);
      else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

void draw_packet_details(ui::PacketSelection& selection, const std::vector<ui::PacketRow>& rows) {
  ImGui::BeginChild("packet_details",ImVec2(0,selection.packet?230:100),true);
  ImGui::SeparatorText("Selected packet");
  if (!selection.packet) {
    ImGui::TextWrapped("Click a decoded packet on the left to inspect its telemetry and payload. The map defaults to Punjab, India.");
  } else {
    const auto& p=*selection.packet;
    ImGui::Text("Packet #%llu | sequence %u",static_cast<unsigned long long>(p.packet_id),p.sequence);
    if (std::none_of(rows.begin(),rows.end(),[&](const auto& r){return r.packet_id==p.packet_id;}))
      ImGui::TextDisabled("Retained selection: this packet has left the live log.");
    ImGui::TextWrapped("Source: %s | serial: %s | type: %s",p.source.empty()?"DJI DroneID":p.source.c_str(),
                        p.serial.empty()?"unavailable":p.serial.c_str(),p.type.empty()?"unavailable":p.type.c_str());
    ImGui::Text("Received: %lld ms since Unix epoch",static_cast<long long>(p.time_ms));
    ImGui::TextWrapped("Format: %s | %s: %s | inner CRC16: %s",p.payload_format.c_str(),
        p.source.empty()?"CRC24":"802.11 FCS",p.crc_ok?"OK":"FAIL",p.inner_crc16.c_str());
    if (ui::packet_drone_location(p)) {
      ImGui::Text("Drone: %.6f, %.6f",p.drone_lat,p.drone_lon);
      ImGui::Text("Altitude / height: %.1f / %.1f m",p.altitude_m,p.height_m);
      if(ImGui::SmallButton("Center drone")) {selection.lat=p.drone_lat;selection.lon=p.drone_lon;selection.zoom=14;}
    } else ImGui::TextWrapped("No valid drone position in this packet. Punjab is the default view, not a detected location.");
    if(p.crc_ok && p.telemetry_parsed && p.pilot_valid && ui::valid_location(p.pilot_lat,p.pilot_lon)) {
      ImGui::Text("Pilot/operator: %.6f, %.6f",p.pilot_lat,p.pilot_lon);
      if(ImGui::SmallButton("Center pilot/operator")) {selection.lat=p.pilot_lat;selection.lon=p.pilot_lon;selection.zoom=14;}
    }
    if(p.crc_ok && p.telemetry_parsed && p.home_valid && ui::valid_location(p.home_lat,p.home_lon)) {
      ImGui::Text("Home: %.6f, %.6f",p.home_lat,p.home_lon);
      if(ImGui::SmallButton("Center home")) {selection.lat=p.home_lat;selection.lon=p.home_lon;selection.zoom=14;}
    }
    if(!p.telemetry_parsed) ImGui::TextWrapped("Payload preserved; telemetry format unsupported or invalid.");
    if(!p.payload_hex.empty()) {
      if(ImGui::SmallButton("Copy payload hex")) ImGui::SetClipboardText(p.payload_hex.c_str());
      ImGui::TextWrapped("Payload: %s",p.payload_hex.c_str());
    }
    if(ImGui::SmallButton("Clear selection")) selection.reset();
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
  SourceEditor source_editor(cfg);
  ui::PacketSelection selection;
  auto map_view=std::make_unique<ui::MapView>();

  auto stop_worker = [&]() {
    if (!running) return;
    stop.store(true);
  };
  auto start_worker = [&]() {
    if (running) { stop_worker(); restart_requested = true; return; }
    try { source_editor.apply(active_cfg); }
    catch (const std::exception& e) {
      UsrpStatus s; s.state = UsrpStatus::State::Error; s.label = "Source settings"; s.detail = e.what();
      model.set_status(s); return;
    }
    if (!active_cfg.pair_manifest.empty() || !active_cfg.replay_ch1.empty()) {
      stop.store(false);
      worker_done.store(false);
      worker = std::thread([&, source_cfg = active_cfg] { paired_replay_worker(model, source_cfg, stop); worker_done.store(true); });
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
  if (!cfg.device.empty() || !cfg.replay.empty() || !cfg.pair_manifest.empty()) start_worker();

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
    source_editor.draw(running, active_cfg.df || !active_cfg.pair_manifest.empty() || !active_cfg.replay_ch1.empty());
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
    const float dir_w = std::max(420.0f, ImGui::GetContentRegionAvail().x * .40f);
    const float left_w = std::max(220.0f, ImGui::GetContentRegionAvail().x - dir_w - 8);

    ImGui::BeginChild("left", ImVec2(left_w, 0), false);
    draw_drones(tracks, rf);
    draw_packets(packets, selection);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("mid", ImVec2(0, 0), false);
    draw_packet_details(selection, packets);
    map_view->draw(selection);
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
  map_view.reset(); // release textures while the OpenGL context still exists
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
      "  paired DF replay: --pair-manifest FILE [--replay CH0 --replay-ch1 CH1]\n"
      "                    [--df-cal FILE | --df-cal-samples N] [--df-ref splitter|ota_broadside]\n"
      "                    [--df-spacing-m M] [--df-jsonl FILE]\n"
      "  live source:   --device ARGS [--freq HZ] [--gain dB] [--ant NAME]  (needs a UHD build)\n"
      "  Open Drone ID: --odid  decode ASTM F3411 Wi-Fi Beacon/NAN instead of DJI DroneID\n"
      "                 (replay >= 20 MSPS, default --rate 20e6; live 20 MSPS, default --freq 2.437e9)\n"
      "  direction find: --df --df-cal FILE  (live hardware DF disabled until DF-5 bench approval)\n", p);
  std::fprintf(stderr,"  automatic sweep: --device ARGS --sweep [--sweep-profile fast|balanced|thorough]\n"
                      "                   [--sweep-include-52] [--sweep-all-wifi] [--sweep-jsonl FILE]\n");
}

int main(int argc, char** argv) {
  try {
  Config cfg;
  bool rate_set = false, freq_set = false;
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
    else if (a == "--rate") { cfg.rate = number(); rate_set = true; }
    else if (a == "--secs") cfg.secs = number();
    else if (a == "--auto-center") cfg.auto_center = true;
    else if (a == "--freq-offset") cfg.freq_offset = number();
    else if (a == "--device") cfg.device = next();
    else if (a == "--freq") { cfg.center_freq = number(); freq_set = true; }
    else if (a == "--gain") cfg.gain = number();
    else if (a == "--ant") cfg.antenna = next();
    else if (a == "--df") cfg.df = true;
    else if (a == "--odid") cfg.odid = true;
    else if (a == "--sweep") cfg.sweep = true;
    else if (a == "--sweep-profile") cfg.sweep_profile = next();
    else if (a == "--sweep-include-52") cfg.sweep_include_52 = true;
    else if (a == "--sweep-all-wifi") cfg.sweep_all_wifi = true;
    else if (a == "--sweep-jsonl") cfg.sweep_jsonl = next();
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

  if (cfg.sweep) {
    droneid::SweepPlan plan(cfg.sweep_profile, cfg.sweep_include_52, cfg.sweep_all_wifi);
    (void)plan;
    if (cfg.odid || cfg.df || !cfg.replay.empty() || !cfg.replay_ch1.empty() || !cfg.pair_manifest.empty() || cfg.device.empty())
      throw std::invalid_argument("--sweep requires --device and excludes replay/DF/--odid");
  }
  if (cfg.odid) {
    if (!rate_set) cfg.rate = droneid::ui::kOdidLiveRate;
    if (!freq_set) cfg.center_freq = droneid::ui::kOdidDefaultCenter;
    if (cfg.df || cfg.auto_center || !cfg.pair_manifest.empty() || !cfg.replay_ch1.empty() || cfg.rate < 20e6)
      throw std::invalid_argument("--odid is single-channel, needs >= 20 MSPS and excludes DF/paired/auto-center");
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
    std::printf("  o4_evidence: windows=%llu structure=%llu candidates=%llu identity=unverified\n",
                static_cast<unsigned long long>(rf.o4_windows),
                static_cast<unsigned long long>(rf.o4_structure_windows),
                static_cast<unsigned long long>(rf.o4_candidate_windows));
    std::printf("  processing: packet_drops=%llu rf_o4_drops=%llu total_packets=%llu retained_packets=%zu\n",
        static_cast<unsigned long long>(st.dropped_windows-st.dropped_rf_windows),
        static_cast<unsigned long long>(st.dropped_rf_windows),
        static_cast<unsigned long long>(model.packet_count()),model.packets().size());
    for (const auto& track : model.tracks()) {
      std::printf("  track: serial=%s count=%llu corr=%.3f", track.serial.c_str(),
                  static_cast<unsigned long long>(track.count), track.corr);
      if (!track.source.empty())
        std::printf(" source=%s type=\"%s\" gen=\"%s\"", track.source.c_str(), track.type.c_str(),
                    track.generation.c_str());
      if (track.has_bearing) {
        std::printf(" bearing_deg=%.4f mirrored_deg=%.4f uncertainty_deg=%.4f ambiguous=%s",
                    track.bearing_deg, track.mirrored_deg, track.bearing_uncertainty_deg,
                    track.bearing_ambiguous ? "true" : "false");
      }
      std::printf("\n");
    }
    for (const auto& packet : model.packets()) {
      if (!packet.source.empty()) {
        std::printf("  packet: source=%s serial=%s seq=%u format=\"%s\" drone=%s", packet.source.c_str(),
                    packet.serial.c_str(), packet.sequence, packet.payload_format.c_str(),
                    packet.drone_valid ? "valid" : "none");
        if (packet.drone_valid)
          std::printf(" lat=%.7f lon=%.7f alt=%.1f height=%.1f", packet.drone_lat, packet.drone_lon,
                      packet.altitude_m, packet.height_m);
        if (packet.pilot_valid) std::printf(" operator=%.7f,%.7f", packet.pilot_lat, packet.pilot_lon);
        std::printf(" payload=%s\n", packet.payload_hex.c_str());
        continue;
      }
      std::printf("  packet: telemetry=%s frame=%s format=%s inner_crc16=%s\n",
                  packet.telemetry_parsed ? "parsed" : "unsupported", packet.payload_hex.c_str(),
                  packet.payload_format.c_str(), packet.inner_crc16.c_str());
    }
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
