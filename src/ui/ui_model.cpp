// src/ui/ui_model.cpp — implementation of the thread-safe UI model.
// GPL-3.0-or-later.
#include "ui_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace droneid::ui {

namespace {
std::string json_string(const std::string& value) {
  std::string result = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      result += '\\';
      result += static_cast<char>(byte);
    } else if (byte < 0x20) {
      char encoded[7];
      std::snprintf(encoded, sizeof(encoded), "\\u%04x", byte);
      result += encoded;
    } else {
      result += static_cast<char>(byte);
    }
  }
  return result + "\"";
}
}

const char* to_string(UsrpStatus::State s) {
  switch (s) {
    case UsrpStatus::State::Disconnected: return "Disconnected";
    case UsrpStatus::State::Connecting:   return "Connecting";
    case UsrpStatus::State::Connected:    return "Connected";
    case UsrpStatus::State::Error:        return "Error";
  }
  return "?";
}

const char* to_string(DfUiState s) {
  switch (s) {
    case DfUiState::Disconnected: return "Disconnected";
    case DfUiState::Uncalibrated: return "Uncalibrated";
    case DfUiState::Calibrating:  return "Calibrating";
    case DfUiState::Valid:        return "Valid";
    case DfUiState::Stale:        return "Stale";
    case DfUiState::Error:        return "Error";
  }
  return "?";
}

std::string DfBearingEvent::to_jsonl() const {
  std::ostringstream os;
  os << std::setprecision(9);
  os << "{\"event\":\"df_bearing\",\"burst_id\":" << burst_id
     << ",\"time_ms\":" << time_ms
     << ",\"start_sample\":" << start_sample
     << ",\"end_sample\":" << end_sample
     << ",\"session_id\":" << json_string(session_id)
     << ",\"config_epoch\":" << config_epoch
     << ",\"cal_id\":" << json_string(cal_id)
     << ",\"classification\":" << json_string(classification)
     << ",\"serial\":" << (serial.empty() ? "null" : json_string(serial))
     << ",\"valid\":" << (valid ? "true" : "false");
  if (valid) {
    os << ",\"broadside_deg\":" << broadside_deg
       << ",\"mirrored_deg\":" << mirrored_deg
       << ",\"uncertainty_deg\":" << uncertainty_deg;
  } else {
    os << ",\"broadside_deg\":null,\"mirrored_deg\":null,\"uncertainty_deg\":null";
  }
  os << ",\"coherence\":" << coherence
     << ",\"phase_std_deg\":" << phase_std_deg
     << ",\"snr_db\":" << snr_db
     << ",\"ambiguous_front_back\":" << (ambiguous_front_back ? "true" : "false")
     << ",\"rejection_reason\":" << (rejection_reason.empty() ? "null" : json_string(rejection_reason))
     << ",\"absolute_position\":null}\n";
  return os.str();
}

void UiModel::set_status(const UsrpStatus& s) {
  std::lock_guard<std::mutex> g(m_);
  status_ = s;
}

UsrpStatus UiModel::status() const {
  std::lock_guard<std::mutex> g(m_);
  return status_;
}

std::string track_key(const std::string& source, const std::string& serial) {
  return source.empty() ? serial : source + ":" + serial;
}

void UiModel::add_packet(const PacketRow& p) {
  std::lock_guard<std::mutex> g(m_);
  packets_.push_back(p);
  while (packets_.size() > packet_cap_) packets_.pop_front();

  if (p.serial.empty() || !p.crc_ok) return;

  const std::string key = track_key(p.source, p.serial);
  DroneTrack& t = tracks_[key];
  t.key = key;
  t.source = p.source;
  t.serial = p.serial;
  t.type = p.type;
  t.decoded = true;
  t.count += 1;
  t.last_ms = p.time_ms;
}

std::vector<PacketRow> UiModel::packets() const {
  std::lock_guard<std::mutex> g(m_);
  return std::vector<PacketRow>(packets_.begin(), packets_.end());
}

void UiModel::upsert_track(const DroneTrack& in) {
  std::lock_guard<std::mutex> g(m_);
  DroneTrack& t = tracks_[in.key];
  const std::string key = in.key;
  // Merge: keep existing decoded/serial if the incoming row doesn't supply them.
  const bool had_decoded = t.decoded;
  const std::string had_serial = t.serial;
  const auto count = t.count;
  const bool had_bearing = t.has_bearing;
  const auto bearing = t.bearing_deg;
  const auto mirrored = t.mirrored_deg;
  const auto uncertainty = t.bearing_uncertainty_deg;
  const auto quality = t.bearing_quality;
  const auto ambiguous = t.bearing_ambiguous;
  const auto start_sample = t.bearing_start_sample;
  const auto end_sample = t.bearing_end_sample;
  t = in;
  t.key = key;
  t.decoded = had_decoded || in.decoded;
  t.count = std::max(count, in.count);
  if (had_bearing && !in.has_bearing) {
    t.has_bearing = true;
    t.bearing_deg = bearing;
    t.mirrored_deg = mirrored;
    t.bearing_uncertainty_deg = uncertainty;
    t.bearing_quality = quality;
    t.bearing_ambiguous = ambiguous;
    t.bearing_start_sample = start_sample;
    t.bearing_end_sample = end_sample;
  }
  if (in.serial.empty() && !had_serial.empty()) t.serial = had_serial;
}

void UiModel::set_bearing(const std::string& key, float deg, float quality, bool ambiguous) {
  const float mirrored = static_cast<float>(std::remainder(180.0 - deg, 360.0));
  set_bearing_detailed(key, deg, mirrored, 0.0f, quality, ambiguous, 0, 0);
}

void UiModel::set_bearing_detailed(const std::string& key, float deg, float mirrored_deg,
                                   float uncertainty_deg, float quality, bool ambiguous,
                                   std::uint64_t start_sample, std::uint64_t end_sample) {
  std::lock_guard<std::mutex> g(m_);
  auto it = tracks_.find(key);
  if (it == tracks_.end()) return;
  if (df_status_.state != DfUiState::Valid) return;
  it->second.has_bearing = true;
  it->second.bearing_deg = deg;
  it->second.mirrored_deg = mirrored_deg;
  it->second.bearing_uncertainty_deg = uncertainty_deg;
  it->second.bearing_quality = quality;
  it->second.bearing_ambiguous = ambiguous;
  it->second.bearing_start_sample = start_sample;
  it->second.bearing_end_sample = end_sample;
}

std::vector<DroneTrack> UiModel::tracks() const {
  std::lock_guard<std::mutex> g(m_);
  std::vector<DroneTrack> out;
  out.reserve(tracks_.size());
  for (const auto& kv : tracks_) out.push_back(kv.second);
  return out;
}

void UiModel::record_rf_burst(double rssi_dbfs, double snr_db, double bw_hz, double center_hz,
                              std::int64_t ms) {
  std::lock_guard<std::mutex> g(m_);
  rf_.bursts_total += 1;
  rf_.last_rssi_dbfs = rssi_dbfs;
  rf_.last_snr_db = snr_db;
  rf_.last_bw_hz = bw_hz;
  rf_.last_center_hz = center_hz;
  rf_.last_ms = ms;
  if (rssi_dbfs > rf_.peak_rssi_dbfs) rf_.peak_rssi_dbfs = rssi_dbfs;
}

RfSummary UiModel::rf_summary() const {
  std::lock_guard<std::mutex> g(m_);
  return rf_;
}

void UiModel::set_df_status(const DfUiStatus& s) {
  std::lock_guard<std::mutex> g(m_);
  df_status_ = s;
  if (s.state != DfUiState::Valid) {
    for (auto& kv : tracks_) kv.second.has_bearing = false;
  }
}

DfUiStatus UiModel::df_status() const {
  std::lock_guard<std::mutex> g(m_);
  return df_status_;
}

void UiModel::invalidate_bearings(DfUiState new_state, const std::string& reason) {
  std::lock_guard<std::mutex> g(m_);
  df_status_.state = new_state;
  df_status_.reason = reason;
  for (auto& kv : tracks_) kv.second.has_bearing = false;
}

void UiModel::record_df_event(const DfBearingEvent& event) {
  std::lock_guard<std::mutex> g(m_);
  df_events_.push_back(event);
  while (df_events_.size() > packet_cap_) df_events_.pop_front();
  if (event.valid) {
    ++df_status_.accepted_bearings;
  } else {
    ++df_status_.rejected_bearings;
    const auto track = tracks_.find(event.serial.empty() ? "unknown_rf" : event.serial);
    if (track != tracks_.end()) track->second.has_bearing = false;
    const auto& r = event.rejection_reason;
    if (r.find("calibration") != std::string::npos || r.find("Uncalibrated") != std::string::npos ||
        r.find("Stale") != std::string::npos || r.find("Disconnected") != std::string::npos)
      ++df_status_.rejected_uncalibrated;
    else if (r.find("coherence") != std::string::npos)
      ++df_status_.rejected_coherence;
    else if (r.find("clipping") != std::string::npos)
      ++df_status_.rejected_clipping;
    else if (r.find("dispersion") != std::string::npos)
      ++df_status_.rejected_dispersion;
    else if (r.find("SNR") != std::string::npos)
      ++df_status_.rejected_snr;
    else if (r.find("passband") != std::string::npos)
      ++df_status_.rejected_passband;
    else if (r.find("overlap") != std::string::npos || r.find("multipath") != std::string::npos)
      ++df_status_.rejected_overlap;
  }
}

std::vector<DfBearingEvent> UiModel::df_events() const {
  std::lock_guard<std::mutex> g(m_);
  return std::vector<DfBearingEvent>(df_events_.begin(), df_events_.end());
}

std::string UiModel::df_summary_jsonl() const {
  std::lock_guard<std::mutex> g(m_);
  std::ostringstream os;
  os << "{\"event\":\"df_summary\",\"state\":\"" << to_string(df_status_.state) << "\""
     << ",\"reason\":" << json_string(df_status_.reason)
     << ",\"session_id\":" << json_string(df_status_.session_id)
     << ",\"config_epoch\":" << df_status_.config_epoch
     << ",\"reference_method\":" << json_string(df_status_.reference_method)
     << ",\"accepted_bearings\":" << df_status_.accepted_bearings
     << ",\"rejected_bearings\":" << df_status_.rejected_bearings
     << ",\"rejected_uncalibrated\":" << df_status_.rejected_uncalibrated
     << ",\"rejected_coherence\":" << df_status_.rejected_coherence
     << ",\"rejected_clipping\":" << df_status_.rejected_clipping
     << ",\"rejected_dispersion\":" << df_status_.rejected_dispersion
     << ",\"rejected_snr\":" << df_status_.rejected_snr
     << ",\"rejected_passband\":" << df_status_.rejected_passband
     << ",\"rejected_overlap\":" << df_status_.rejected_overlap
     << ",\"dropped_pairs\":" << df_status_.dropped_pairs
     << ",\"absolute_position\":null}\n";
  return os.str();
}

// --- checklist ---

char checklist_marker(const std::string& line) {
  // Match optional leading whitespace, then "- [X] " where X is one char.
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i + 5 > line.size()) return 0;
  if (line[i] != '-' || line[i + 1] != ' ' || line[i + 2] != '[' || line[i + 4] != ']') return 0;
  if (i + 5 >= line.size() || line[i + 5] != ' ') return 0;
  return line[i + 3];
}

static std::size_t marker_index(const std::string& line) {
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  return i + 3;  // position of the char between the brackets
}

void UiModel::reparse_locked() {
  groups_.clear();
  ChecklistGroup* current = nullptr;
  ChecklistGroup preamble;  // items before any heading
  preamble.title = "";
  for (int idx = 0; idx < static_cast<int>(md_lines_.size()); ++idx) {
    const std::string& line = md_lines_[idx];
    if (line.rfind("## ", 0) == 0) {
      groups_.push_back(ChecklistGroup{line.substr(3), {}});
      current = &groups_.back();
      continue;
    }
    const char mk = checklist_marker(line);
    if (mk == ' ' || mk == 'x' || mk == 'X' || mk == '/') {
      ChecklistItem item;
      item.state = (mk == 'X') ? 'x' : mk;
      item.line = idx;
      const std::size_t pos = marker_index(line);
      item.text = (pos + 3 <= line.size()) ? line.substr(pos + 3) : "";
      if (current) current->items.push_back(item);
      else preamble.items.push_back(item);
    }
  }
  if (!preamble.items.empty()) groups_.insert(groups_.begin(), preamble);
}

bool UiModel::load_checklist_text(const std::string& markdown) {
  std::lock_guard<std::mutex> g(m_);
  md_lines_.clear();
  std::istringstream in(markdown);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    md_lines_.push_back(line);
  }
  reparse_locked();
  return true;
}

bool UiModel::load_checklist_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  return load_checklist_text(ss.str());
}

std::vector<ChecklistGroup> UiModel::checklist() const {
  std::lock_guard<std::mutex> g(m_);
  return groups_;
}

void UiModel::set_checklist_item(std::size_t group, std::size_t item, char state) {
  std::lock_guard<std::mutex> g(m_);
  if (group >= groups_.size()) return;
  auto& items = groups_[group].items;
  if (item >= items.size()) return;
  const int line = items[item].line;
  if (line < 0 || line >= static_cast<int>(md_lines_.size())) return;
  const std::size_t pos = marker_index(md_lines_[line]);
  if (pos >= md_lines_[line].size()) return;
  md_lines_[line][pos] = state;        // rewrite only the marker, preserve prose
  items[item].state = state;
}

std::string UiModel::checklist_markdown() const {
  std::lock_guard<std::mutex> g(m_);
  std::string out;
  for (const auto& l : md_lines_) { out += l; out += '\n'; }
  return out;
}

bool UiModel::save_checklist_file(const std::string& path) const {
  const std::string text = checklist_markdown();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << text;
  return out.good();
}

}  // namespace droneid::ui
