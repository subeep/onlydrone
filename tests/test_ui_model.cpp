// tests/test_ui_model.cpp — headless tests for the UI model (no window needed).
// GPL-3.0-or-later.
#include <string>

#include "../src/ui/ui_model.h"
#include "droneid/test/microtest.h"

using namespace droneid::ui;

TEST(UiChecklist, MarkerDetection) {
  EXPECT_EQ(checklist_marker("- [ ] todo"), ' ');
  EXPECT_EQ(checklist_marker("- [x] done"), 'x');
  EXPECT_EQ(checklist_marker("  - [/] wip"), '/');
  EXPECT_EQ(checklist_marker("not an item"), char(0));
  EXPECT_EQ(checklist_marker("## heading"), char(0));
  EXPECT_EQ(checklist_marker("- [] malformed"), char(0));
}

TEST(UiChecklist, ParseGroupsAndItems) {
  const std::string md =
      "# Title\n"
      "intro prose\n"
      "## Phase 0\n"
      "- [x] scaffold\n"
      "- [ ] more\n"
      "## Phase 3\n"
      "some text\n"
      "- [/] ui shell\n";
  UiModel m;
  EXPECT_TRUE(m.load_checklist_text(md));
  auto groups = m.checklist();
  EXPECT_EQ(groups.size(), size_t(2));
  EXPECT_EQ(groups[0].title, std::string("Phase 0"));
  EXPECT_EQ(groups[0].items.size(), size_t(2));
  EXPECT_EQ(groups[0].items[0].state, 'x');
  EXPECT_EQ(groups[0].items[0].text, std::string("scaffold"));
  EXPECT_EQ(groups[1].title, std::string("Phase 3"));
  EXPECT_EQ(groups[1].items[0].state, '/');
}

TEST(UiChecklist, InPlaceEditPreservesProse) {
  const std::string md =
      "## Phase 3\n"
      "important prose line\n"
      "- [ ] ui shell\n"
      "- [ ] packets panel\n";
  UiModel m;
  m.load_checklist_text(md);
  m.set_checklist_item(0, 0, 'x');
  m.set_checklist_item(0, 1, '/');
  const std::string out = m.checklist_markdown();
  EXPECT_TRUE(out.find("important prose line") != std::string::npos);
  EXPECT_TRUE(out.find("- [x] ui shell") != std::string::npos);
  EXPECT_TRUE(out.find("- [/] packets panel") != std::string::npos);
  // reparse reflects the new states
  UiModel m2;
  m2.load_checklist_text(out);
  auto g = m2.checklist();
  EXPECT_EQ(g[0].items[0].state, 'x');
  EXPECT_EQ(g[0].items[1].state, '/');
}

TEST(UiModel, PacketRingBounded) {
  UiModel m(8);
  for (int i = 0; i < 20; ++i) {
    PacketRow p;
    p.serial = "S" + std::to_string(i % 3);
    p.time_ms = i;
    m.add_packet(p);
  }
  EXPECT_EQ(m.packets().size(), size_t(8));
  EXPECT_EQ(m.packets().back().time_ms, int64_t(19));  // newest kept
  // three distinct serials -> three tracks
  EXPECT_EQ(m.tracks().size(), size_t(3));
}

TEST(UiModel, TrackUpsertAndBearing) {
  UiModel m;
  DroneTrack t;
  t.key = "RF1";
  t.generation = "O4";
  t.rssi_dbfs = -55;
  t.decoded = false;
  m.upsert_track(t);
  m.set_bearing("RF1", 37.5f, 0.8f, true);
  auto tracks = m.tracks();
  EXPECT_EQ(tracks.size(), size_t(1));
  EXPECT_TRUE(tracks[0].has_bearing);
  EXPECT_NEAR(tracks[0].bearing_deg, 37.5, 1e-4);
  EXPECT_FALSE(tracks[0].decoded);
}

TEST(UiModel, MetricsUpdatesPreservePacketCounts) {
  UiModel model;
  PacketRow packet;
  packet.serial = "REAL-SERIAL";
  model.add_packet(packet);
  model.add_packet(packet);
  model.set_bearing(packet.serial, 12, 0.9, true);
  DroneTrack metrics;
  metrics.key = packet.serial;
  metrics.corr = 0.85;
  model.upsert_track(metrics);
  const auto track = model.tracks().front();
  EXPECT_EQ(track.count, uint64_t(2));
  EXPECT_TRUE(track.decoded);
  EXPECT_EQ(track.serial, packet.serial);
  EXPECT_TRUE(track.has_bearing);
}

TEST(UiModel, UnsupportedPayloadDoesNotInventTrack) {
  UiModel model;
  PacketRow packet;
  packet.payload_hex = "8010494e4650";
  model.add_packet(packet);
  EXPECT_EQ(model.packets().size(), std::size_t(1));
  EXPECT_TRUE(model.tracks().empty());
}

MICROTEST_MAIN
