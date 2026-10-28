// src/ui/ui_model.h — thread-safe state the operator UI renders.
//
// This is deliberately independent of the decoder/radio backend: the worker thread
// translates backend results into these plain structs and the render thread only reads
// snapshots. No backend headers are included here, so the model is unit-testable headless.
// GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace droneid::ui {

struct UsrpStatus {
  enum class State { Disconnected, Connecting, Connected, Error };
  State state = State::Disconnected;
  std::string label;        // "X310 326CF02" or "Replay: file.iq"
  std::string detail;       // free-text (addr, error reason, ...)
  double rate_hz = 0;
  double center_hz = 0;
  double gain_db = 0;
  bool ref_locked = false;
  std::uint64_t overflows = 0;
  std::uint64_t samples_received = 0;
  std::uint64_t dropped_windows = 0;
  std::uint64_t discontinuities = 0;
};

const char* to_string(UsrpStatus::State s);

enum class DfUiState {
  Disconnected,
  Uncalibrated,
  Calibrating,
  Valid,
  Stale,
  Error
};

const char* to_string(DfUiState s);

// DF-4: Thread-safe snapshot of DF calibration lifecycle, provenance, and rejection counters.
struct DfUiStatus {
  DfUiState state = DfUiState::Disconnected;
  std::string reason = "single antenna / DF off";
  std::string session_id;
  std::uint64_t config_epoch = 0;
  std::string rig_id;
  std::string reference_method = "none";  // "splitter", "ota_broadside", or "none"
  double center_hz = 0;
  double passband_hz = 0;
  double spacing_m = 0;
  double orientation_deg = 0;
  double cal_age_seconds = 0;
  std::int64_t updated_ms = 0;
  std::uint64_t accepted_bearings = 0;
  std::uint64_t rejected_bearings = 0;
  std::uint64_t rejected_uncalibrated = 0;
  std::uint64_t rejected_coherence = 0;
  std::uint64_t rejected_clipping = 0;
  std::uint64_t rejected_dispersion = 0;
  std::uint64_t rejected_snr = 0;
  std::uint64_t rejected_passband = 0;
  std::uint64_t rejected_overlap = 0;
  std::uint64_t dropped_pairs = 0;
};

// DF-4: Operational JSONL bearing event (emitted for both accepted and rejected bursts).
// Never derives a GPS coordinate from an angle alone.
struct DfBearingEvent {
  std::uint64_t burst_id = 0;
  std::uint64_t start_sample = 0;
  std::uint64_t end_sample = 0;
  std::string session_id;
  std::uint64_t config_epoch = 0;
  std::string cal_id;
  std::string classification = "unknown_rf";  // "decoded_droneid" or "unknown_rf"
  std::string serial;
  bool valid = false;
  double broadside_deg = 0;
  double mirrored_deg = 0;
  double uncertainty_deg = 0;
  double coherence = 0;
  double phase_std_deg = 0;
  double snr_db = 0;
  bool ambiguous_front_back = true;
  std::string rejection_reason;
  std::int64_t time_ms = 0;

  std::string to_jsonl() const;
};

// One decoded DroneID frame row for the packet log.
struct PacketRow {
  std::int64_t time_ms = 0;        // wall-clock when added
  std::uint16_t sequence = 0;
  std::string serial;
  std::string type;
  double drone_lat = 0, drone_lon = 0, altitude_m = 0, height_m = 0;
  double pilot_lat = 0, pilot_lon = 0, home_lat = 0, home_lon = 0;
  bool pilot_valid = false, home_valid = false;
  bool crc_ok = true;
  bool telemetry_parsed = false;
  std::string payload_hex;
  std::string payload_format = "unknown";
  std::string inner_crc16 = "unchecked";
  std::string source;              // "" = DJI DroneID (default); "ODID/ASTM" = F3411 Remote ID
  bool drone_valid = true;         // false when the frame carried no usable position
};

// One tracked source (decoded drone or raw RF emitter), keyed by serial or an RF id.
struct DroneTrack {
  std::string key;
  std::string source;              // "" = DJI DroneID / raw RF; "ODID/ASTM" = F3411 Remote ID
  std::string serial;
  std::string type;
  std::string generation;
  float rssi_dbfs = -300;
  float snr_db = -300;
  float corr = 0;
  std::uint64_t count = 0;
  std::int64_t last_ms = 0;
  bool decoded = false;
  bool has_bearing = false;
  float bearing_deg = 0;
  float mirrored_deg = 0;
  float bearing_uncertainty_deg = 0;
  float bearing_quality = 0;
  bool bearing_ambiguous = true;
  std::uint64_t bearing_start_sample = 0;
  std::uint64_t bearing_end_sample = 0;
};

// Aggregate "unknown RF" activity — NOT a per-drone count (energy bursts incl. Wi-Fi/baseline).
struct RfSummary {
  std::uint64_t o4_windows = 0;
  std::uint64_t o4_structure_windows = 0;
  std::uint64_t o4_candidate_windows = 0;
  double o4_last_cyclic_correlation = 0;
  std::uint64_t bursts_total = 0;
  double peak_rssi_dbfs = -300;
  double last_rssi_dbfs = -300;
  double last_snr_db = -300;
  double last_bw_hz = 0;
  double last_center_hz = 0;
  std::int64_t last_ms = 0;
};

struct ChecklistItem {
  char state = ' ';   // ' ' todo, 'x' done, '/' in progress
  std::string text;
  int line = -1;      // index into the raw md lines (for in-place rewrite)
};
struct ChecklistGroup {
  std::string title;
  std::vector<ChecklistItem> items;
};

// Thread-safe. Writers = worker thread(s); reader = render thread (snapshots).
class UiModel {
 public:
  explicit UiModel(std::size_t packet_capacity = 500) : packet_cap_(packet_capacity) {}

  void set_status(const UsrpStatus& s);
  UsrpStatus status() const;

  // Adds a packet to the bounded log and upserts the matching track.
  void add_packet(const PacketRow& p);
  std::vector<PacketRow> packets() const;   // newest last

  void upsert_track(const DroneTrack& t);   // merges by key
  void set_bearing(const std::string& key, float deg, float quality, bool ambiguous);
  void set_bearing_detailed(const std::string& key, float deg, float mirrored_deg,
                            float uncertainty_deg, float quality, bool ambiguous,
                            std::uint64_t start_sample = 0, std::uint64_t end_sample = 0);
  std::vector<DroneTrack> tracks() const;

  void record_rf_burst(double rssi_dbfs, double snr_db, double bw_hz, double center_hz,
                       std::int64_t ms);
  RfSummary rf_summary() const;
  void record_o4_evidence(bool structure_match, bool candidate, double cyclic_correlation);
  void reset_o4_evidence();

  // --- DF-4 state & operational logging ---
  void set_df_status(const DfUiStatus& s);
  DfUiStatus df_status() const;
  void invalidate_bearings(DfUiState new_state, const std::string& reason);
  void record_df_event(const DfBearingEvent& event);
  std::vector<DfBearingEvent> df_events() const;
  std::string df_summary_jsonl() const;

  // --- checklist (progress.md style) ---
  // Parses GitHub-style task lists under "## " headings; keeps the raw lines so a save
  // rewrites only the checkbox marker in place, preserving all other prose.
  bool load_checklist_text(const std::string& markdown);
  bool load_checklist_file(const std::string& path);
  std::vector<ChecklistGroup> checklist() const;
  void set_checklist_item(std::size_t group, std::size_t item, char state);
  std::string checklist_markdown() const;              // current md text
  bool save_checklist_file(const std::string& path) const;

 private:
  mutable std::mutex m_;
  std::size_t packet_cap_;
  UsrpStatus status_;
  DfUiStatus df_status_;
  std::deque<PacketRow> packets_;
  std::deque<DfBearingEvent> df_events_;
  std::map<std::string, DroneTrack> tracks_;
  RfSummary rf_;
  std::vector<std::string> md_lines_;
  std::vector<ChecklistGroup> groups_;

  void reparse_locked();
};

// Track key for a decoded packet: DJI rows keep the bare serial; other sources are namespaced so a
// drone broadcasting both DJI DroneID and Remote ID shows as two tracks, one per source.
std::string track_key(const std::string& source, const std::string& serial);

// Returns the marker char if `line` is a task-list item ("- [ ] ..."), else 0.
// Exposed for tests.
char checklist_marker(const std::string& line);

}  // namespace droneid::ui
