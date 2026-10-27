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
};

// One tracked source (decoded drone or raw RF emitter), keyed by serial or an RF id.
struct DroneTrack {
  std::string key;
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
  float bearing_quality = 0;
  bool bearing_ambiguous = true;
};

// Aggregate "unknown RF" activity — NOT a per-drone count (energy bursts incl. Wi-Fi/baseline).
struct RfSummary {
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
  std::vector<DroneTrack> tracks() const;

  void record_rf_burst(double rssi_dbfs, double snr_db, double bw_hz, double center_hz,
                       std::int64_t ms);
  RfSummary rf_summary() const;

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
  std::deque<PacketRow> packets_;
  std::map<std::string, DroneTrack> tracks_;
  RfSummary rf_;
  std::vector<std::string> md_lines_;
  std::vector<ChecklistGroup> groups_;

  void reparse_locked();
};

// Returns the marker char if `line` is a task-list item ("- [ ] ..."), else 0.
// Exposed for tests.
char checklist_marker(const std::string& line);

}  // namespace droneid::ui
