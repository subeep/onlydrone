// ASTM F3411 message parser and Wi-Fi Beacon / NAN MAC framing tests (no DSP).
// Byte vectors are assembled by hand from the F3411 field layout, independently of the encoder.
#include "droneid/odid.h"
#include "droneid/odid_wifi.h"
#include "droneid/test/microtest.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace droneid::odid;

namespace {

std::vector<uint8_t> v(std::initializer_list<int> bytes) {
  std::vector<uint8_t> out;
  for (const int b : bytes) out.push_back(static_cast<uint8_t>(b));
  return out;
}

std::vector<uint8_t> text_message(uint8_t header, uint8_t byte1, const char* text) {
  std::vector<uint8_t> m(25, 0);
  m[0] = header;
  m[1] = byte1;
  std::memcpy(m.data() + 2, text, std::strlen(text));
  return m;
}

// Location: status airborne, AGL height, west (EW=1) 90 -> 270 deg, 10 m/s, -3 m/s,
// 51.4779 / -0.0015, baro 120 m, geo 125.5 m, height 50 m, h_acc 10, v_acc 4, speed_acc 2,
// baro_acc 3, timestamp 1234.5 s past the hour, ts accuracy 0.5 s.
const std::vector<uint8_t> kLocation = v({0x12, 0x26, 0x5A, 0x28, 0xFA, 0x78, 0xE7, 0xAE, 0x1E, 0x68, 0xC5,
                                          0xFF, 0xFF, 0xC0, 0x08, 0xCB, 0x08, 0x34, 0x08, 0x4A, 0x32, 0x39,
                                          0x30, 0x05, 0x00});
// System: EU classification, fixed operator location 51.4769 / -0.0005, 3 aircraft, 50 m radius,
// ceiling 200 m, floor 0 m, category 2 class 3, operator altitude 70 m, timestamp 245000000.
const std::vector<uint8_t> kSystem = v({0x42, 0x06, 0x68, 0xC0, 0xAE, 0x1E, 0x78, 0xEC, 0xFF, 0xFF, 0x03,
                                        0x00, 0x05, 0x60, 0x09, 0xD0, 0x07, 0x23, 0x5C, 0x08, 0x40, 0x67,
                                        0x9A, 0x0E, 0x00});
const std::vector<uint8_t> kBasicId = text_message(0x02, 0x12, "1581F5FKD229400E1Z");
const std::vector<uint8_t> kSelfId = text_message(0x32, 0x00, "Recreational");
const std::vector<uint8_t> kOperatorId = text_message(0x52, 0x00, "FIN87astrdge12k8");

std::vector<uint8_t> pack_of(const std::vector<std::vector<uint8_t>>& messages, uint8_t header = 0xF2) {
  std::vector<uint8_t> p = {header, 25, static_cast<uint8_t>(messages.size())};
  for (const auto& m : messages) p.insert(p.end(), m.begin(), m.end());
  return p;
}

}  // namespace

TEST(Odid, BasicIdVector) {
  UasData u;
  const auto r = decode(kBasicId, u);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.type == MessageType::BasicId);
  EXPECT_EQ(u.basic_ids.size(), std::size_t(1));
  EXPECT_EQ(u.basic_ids[0].id_type, 1);
  EXPECT_EQ(u.basic_ids[0].ua_type, 2);
  EXPECT_EQ(u.primary_id(), std::string("1581F5FKD229400E1Z"));
  EXPECT_EQ(std::string(ua_type_name(2)), std::string("Helicopter/Multirotor"));
  EXPECT_EQ(u.protocol_version, 2);
}

TEST(Odid, LocationScalingVector) {
  UasData u;
  EXPECT_TRUE(decode(kLocation, u).ok);
  EXPECT_TRUE(u.location.has_value());
  const auto& l = *u.location;
  EXPECT_EQ(l.status, 2);
  EXPECT_EQ(l.height_type, 1);
  EXPECT_NEAR(l.direction_deg, 270.0, 1e-9);
  EXPECT_NEAR(l.speed_h_mps, 10.0, 1e-9);
  EXPECT_NEAR(l.speed_v_mps, -3.0, 1e-9);
  EXPECT_NEAR(l.lat, 51.4779, 1e-9);
  EXPECT_NEAR(l.lon, -0.0015, 1e-9);
  EXPECT_NEAR(l.alt_baro_m, 120.0, 1e-9);
  EXPECT_NEAR(l.alt_geo_m, 125.5, 1e-9);
  EXPECT_NEAR(l.height_m, 50.0, 1e-9);
  EXPECT_EQ(l.h_accuracy, 10);
  EXPECT_EQ(l.v_accuracy, 4);
  EXPECT_EQ(l.speed_accuracy, 2);
  EXPECT_EQ(l.baro_accuracy, 3);
  EXPECT_TRUE(l.timestamp_valid());
  EXPECT_NEAR(l.timestamp_s(), 1234.5, 1e-9);
  EXPECT_EQ(l.ts_accuracy, 5);
}

TEST(Odid, LocationSpeedMultiplierAndUnknowns) {
  auto m = kLocation;
  m[1] = 0x01;  // speed multiplier set, EW = 0
  m[3] = 100;   // 100 * 0.75 + 63.75
  UasData u;
  EXPECT_TRUE(decode(m, u).ok);
  EXPECT_NEAR(u.location->speed_h_mps, 138.75, 1e-9);
  EXPECT_NEAR(u.location->direction_deg, 90.0, 1e-9);
  // F3411 "unknown" encodings.
  m[1] = 0x03;  // EW + mult
  m[2] = 181;   // 181 + 180 = 361 (unknown)
  m[3] = 255;   // 255 m/s (unknown)
  m[4] = 126;   // 63 m/s (unknown)
  std::fill(m.begin() + 5, m.begin() + 19, 0);  // lat/lon 0, altitudes -1000 m
  m[21] = 0xFF;
  m[22] = 0xFF;
  UasData unknown;
  EXPECT_TRUE(decode(m, unknown).ok);
  EXPECT_NEAR(unknown.location->direction_deg, 361.0, 1e-9);
  EXPECT_NEAR(unknown.location->speed_h_mps, 255.0, 1e-9);
  EXPECT_NEAR(unknown.location->speed_v_mps, 63.0, 1e-9);
  EXPECT_FALSE(unknown.location->position_valid());
  EXPECT_NEAR(unknown.location->alt_geo_m, -1000.0, 1e-9);
  EXPECT_FALSE(unknown.location->timestamp_valid());
}

TEST(Odid, LocationRejectsOutOfRange) {
  auto m = kLocation;
  const int32_t lat = 950000000;  // 95 degrees
  std::memcpy(m.data() + 5, &lat, 4);
  UasData u;
  auto r = decode(m, u);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(u.location.has_value());
  m = kLocation;
  m[1] = 0x02;
  m[2] = 200;  // 380 deg, not the 361 marker
  EXPECT_FALSE(decode(m, u).ok);
  m = kLocation;
  m[21] = 0x41;  // 36161 > 36000 tenths
  m[22] = 0x8D;
  EXPECT_FALSE(decode(m, u).ok);
}

TEST(Odid, SystemVector) {
  UasData u;
  EXPECT_TRUE(decode(kSystem, u).ok);
  const auto& s = *u.system;
  EXPECT_EQ(s.operator_location_type, 2);
  EXPECT_EQ(s.classification_type, 1);
  EXPECT_NEAR(s.operator_lat, 51.4769, 1e-9);
  EXPECT_NEAR(s.operator_lon, -0.0005, 1e-9);
  EXPECT_EQ(s.area_count, 3);
  EXPECT_EQ(s.area_radius_m, 50);
  EXPECT_NEAR(s.area_ceiling_m, 200.0, 1e-9);
  EXPECT_NEAR(s.area_floor_m, 0.0, 1e-9);
  EXPECT_EQ(s.category_eu, 2);
  EXPECT_EQ(s.class_eu, 3);
  EXPECT_NEAR(s.operator_alt_geo_m, 70.0, 1e-9);
  EXPECT_EQ(s.timestamp, 245000000u);
}

TEST(Odid, SelfIdOperatorIdAndAuth) {
  UasData u;
  EXPECT_TRUE(decode(kSelfId, u).ok);
  EXPECT_TRUE(decode(kOperatorId, u).ok);
  EXPECT_EQ(u.self_id->desc, std::string("Recreational"));
  EXPECT_EQ(u.operator_id->id, std::string("FIN87astrdge12k8"));

  std::vector<uint8_t> page0(25, 0), page1(25, 0);
  page0[0] = 0x22; page0[1] = 0x10; page0[2] = 1; page0[3] = 30;
  page0[4] = 0x78; page0[5] = 0x56; page0[6] = 0x34; page0[7] = 0x12;
  for (int i = 8; i < 25; ++i) page0[i] = static_cast<uint8_t>(0xA0 + i);
  page1[0] = 0x22; page1[1] = 0x11;
  for (int i = 2; i < 25; ++i) page1[i] = static_cast<uint8_t>(i);
  EXPECT_TRUE(decode(page0, u).ok);
  EXPECT_TRUE(decode(page1, u).ok);
  EXPECT_EQ(u.auth.size(), std::size_t(2));
  EXPECT_EQ(u.auth[0].auth_type, 1);
  EXPECT_EQ(u.auth[0].last_page_index, 1);
  EXPECT_EQ(u.auth[0].length, 30);
  EXPECT_EQ(u.auth[0].timestamp, 0x12345678u);
  EXPECT_EQ(u.auth[0].data.size(), std::size_t(17));
  EXPECT_EQ(u.auth[1].data.size(), std::size_t(23));
  EXPECT_EQ(u.auth[1].data[0], 2);

  auto bad = page0;
  bad[2] = 16;  // last page index beyond 15
  EXPECT_FALSE(decode(bad, u).ok);
  bad = page0;
  bad[2] = 0;
  bad[3] = 18;  // 18 bytes cannot fit in page 0 alone
  EXPECT_FALSE(decode(bad, u).ok);
}

TEST(Odid, BinaryIdsAreHex) {
  std::vector<uint8_t> m(25, 0);
  m[0] = 0x02;
  m[1] = 0x42;  // specific session id
  m[2] = 0x01; m[3] = 0xFF;
  UasData u;
  EXPECT_TRUE(decode(m, u).ok);
  EXPECT_EQ(u.basic_ids[0].uas_id.substr(0, 6), std::string("0x01ff"));
  EXPECT_EQ(u.basic_ids[0].uas_id.size(), std::size_t(42));
}

TEST(Odid, MessagePack) {
  const auto pack = pack_of({kBasicId, kLocation, kSelfId, kSystem, kOperatorId});
  UasData u;
  const auto r = decode(pack, u);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.type == MessageType::Pack);
  EXPECT_EQ(r.decoded, 5);
  EXPECT_EQ(r.rejected, 0);
  EXPECT_EQ(u.primary_id(), std::string("1581F5FKD229400E1Z"));
  EXPECT_NEAR(u.location->lat, 51.4779, 1e-9);
  EXPECT_NEAR(u.system->operator_lat, 51.4769, 1e-9);
  // Trailing bytes after a complete pack are tolerated (Wi-Fi element padding).
  auto padded = pack;
  padded.push_back(0);
  EXPECT_TRUE(decode(padded, u).ok);
}

TEST(Odid, MalformedPacks) {
  UasData u;
  auto p = pack_of({kBasicId, kLocation});
  p[1] = 24;
  EXPECT_FALSE(decode(p, u).ok);                                   // single message size
  p = pack_of({kBasicId, kLocation});
  p[2] = 0;
  EXPECT_FALSE(decode(p, u).ok);                                   // empty
  p[2] = 10;
  EXPECT_FALSE(decode(p, u).ok);                                   // > 9 messages
  p = pack_of({kBasicId, kLocation});
  p.resize(p.size() - 1);
  EXPECT_FALSE(decode(p, u).ok);                                   // truncated
  EXPECT_FALSE(decode(pack_of({kBasicId, pack_of({kLocation})}), u).ok);  // nested pack (wrong size too)
  auto nested = pack_of({kBasicId});
  nested[3] = 0xF2;
  EXPECT_FALSE(decode(nested, u).ok);                              // pack header inside a pack
  EXPECT_FALSE(decode(pack_of({kLocation, kLocation}), u).ok);     // duplicate location
  EXPECT_FALSE(decode(pack_of({kBasicId, kBasicId, kBasicId}), u).ok);  // three basic ids
  auto invalid = kBasicId;
  invalid[0] = 0x62;
  EXPECT_FALSE(decode(pack_of({invalid}), u).ok);                  // type 6 inside a pack
  EXPECT_FALSE(decode(invalid, u).ok);                             // type 6 alone
  auto v3 = kBasicId;
  v3[0] = 0x03;
  EXPECT_FALSE(decode(v3, u).ok);                                  // protocol version 3
  EXPECT_FALSE(decode(kBasicId.data(), 24, u).ok);                 // short buffer
  EXPECT_FALSE(decode(nullptr, 0, u).ok);
}

TEST(Odid, PackKeepsValidMessagesAndCountsRejected) {
  auto bad_location = kLocation;
  const int32_t lon = 1900000000;  // 190 degrees
  std::memcpy(bad_location.data() + 9, &lon, 4);
  UasData u;
  const auto r = decode(pack_of({kBasicId, bad_location, kOperatorId}), u);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.decoded, 2);
  EXPECT_EQ(r.rejected, 1);
  EXPECT_FALSE(u.location.has_value());
  EXPECT_EQ(u.primary_id(), std::string("1581F5FKD229400E1Z"));
}

TEST(Odid, EncoderRoundTrip) {
  BasicId b; b.id_type = 2; b.ua_type = 15; b.uas_id = "CAA-REG-123";
  Location l; l.status = 3; l.direction_deg = 359; l.speed_h_mps = 200.25; l.speed_v_mps = -12.5;
  l.lat = -33.8688197; l.lon = 151.2092955; l.alt_baro_m = 30; l.alt_geo_m = -20.5; l.height_m = 12;
  l.timestamp_raw = 35999;
  System s; s.operator_location_type = 1; s.operator_lat = 10.5; s.operator_lon = -20.25; s.area_radius_m = 2550;
  OperatorId o; o.id = "OP-1";
  SelfId d; d.desc_type = 1; d.desc = "emergency text";
  UasData u;
  EXPECT_TRUE(decode(encode_pack({encode(b), encode(l), encode(s), encode(o), encode(d)}), u).ok);
  EXPECT_EQ(u.primary_id(), std::string("CAA-REG-123"));
  EXPECT_EQ(u.basic_ids[0].ua_type, 15);
  EXPECT_NEAR(u.location->direction_deg, 359, 1e-9);
  EXPECT_NEAR(u.location->speed_h_mps, 200.25, 1e-9);
  EXPECT_NEAR(u.location->speed_v_mps, -12.5, 1e-9);
  EXPECT_NEAR(u.location->lat, -33.8688197, 1e-7);
  EXPECT_NEAR(u.location->lon, 151.2092955, 1e-7);
  EXPECT_NEAR(u.location->alt_geo_m, -20.5, 1e-9);
  EXPECT_EQ(u.location->timestamp_raw, 35999);
  EXPECT_EQ(u.system->area_radius_m, 2550);
  EXPECT_EQ(u.self_id->desc_type, 1);
  // Encoder output matches the hand-built vectors byte for byte.
  BasicId hb; hb.id_type = 1; hb.ua_type = 2; hb.uas_id = "1581F5FKD229400E1Z";
  const auto eb = encode(hb);
  EXPECT_TRUE(std::equal(eb.begin(), eb.end(), kBasicId.begin()));
  UasData ref;
  decode(kLocation, ref);
  const auto el = encode(*ref.location);
  EXPECT_TRUE(std::equal(el.begin(), el.end(), kLocation.begin()));
  decode(kSystem, ref);
  const auto es = encode(*ref.system);
  EXPECT_TRUE(std::equal(es.begin(), es.end(), kSystem.begin()));
}

TEST(Odid, MergeKeepsIdentityAcrossFrames) {
  UasData a, b, merged;
  decode(kBasicId, a);
  decode(kLocation, b);
  merge(merged, a);
  merge(merged, b);
  EXPECT_EQ(merged.primary_id(), std::string("1581F5FKD229400E1Z"));
  EXPECT_TRUE(merged.location.has_value());
}

TEST(OdidMac, Crc32KnownValue) {
  const char* s = "123456789";
  EXPECT_EQ(crc32_ieee(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(OdidMac, BeaconRoundTrip) {
  const Mac mac = {0x60, 0x60, 0x1F, 0x12, 0x34, 0x56};
  const auto pack = pack_of({kBasicId, kLocation, kSystem});
  const auto f = build_beacon_mpdu(mac, "UAS-1", 7, pack);
  EXPECT_TRUE(fcs_valid(f));
  // Layout per opendroneid-core-c: 24 B header, 12 B fixed fields, SSID, rates, vendor IE.
  EXPECT_EQ(f[0], 0x80);
  EXPECT_EQ(f[36], 0x00);
  EXPECT_EQ(f[37], 5);
  const std::size_t ie = 36 + 2 + 5 + 3;
  EXPECT_EQ(f[ie], 0xDD);
  EXPECT_EQ(f[ie + 1], static_cast<uint8_t>(4 + 1 + pack.size()));
  EXPECT_TRUE(f[ie + 2] == 0xFA && f[ie + 3] == 0x0B && f[ie + 4] == 0xBC && f[ie + 5] == 0x0D);
  WifiOdidFrame out;
  std::string why;
  EXPECT_TRUE(parse_odid_mpdu(std::vector<uint8_t>(f.begin(), f.end() - 4), out, &why));
  EXPECT_TRUE(out.transport == WifiTransport::Beacon);
  EXPECT_EQ(out.ssid, std::string("UAS-1"));
  EXPECT_EQ(out.message_counter, 7);
  EXPECT_EQ(mac_string(out.src), std::string("60:60:1f:12:34:56"));
  EXPECT_EQ(out.uas.primary_id(), std::string("1581F5FKD229400E1Z"));
  EXPECT_EQ(out.result.decoded, 3);
}

TEST(OdidMac, NanRoundTrip) {
  const Mac mac = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  const auto f = build_nan_action_mpdu(mac, 200, pack_of({kBasicId, kLocation}));
  EXPECT_TRUE(fcs_valid(f));
  EXPECT_EQ(f[0], 0xD0);
  EXPECT_TRUE(f[4] == 0x51 && f[5] == 0x6F && f[9] == 0x00);           // NAN network id
  EXPECT_TRUE(f[16] == 0x50 && f[21] == 0xFF);                         // ASTM fixed cluster id
  EXPECT_TRUE(f[24] == 0x04 && f[25] == 0x09 && f[29] == 0x13);        // public action, WFA NAN
  EXPECT_TRUE(f[30] == 0x03 && f[33] == 0x88 && f[38] == 0x09);        // SDA + ODID service id
  WifiOdidFrame out;
  EXPECT_TRUE(parse_odid_mpdu(std::vector<uint8_t>(f.begin(), f.end() - 4), out));
  EXPECT_TRUE(out.transport == WifiTransport::Nan);
  EXPECT_EQ(out.message_counter, 200);
  EXPECT_NEAR(out.uas.location->lat, 51.4779, 1e-9);
}

TEST(OdidMac, RejectsNonOdidAndCorruptFrames) {
  const Mac mac = {0x02, 0, 0, 0, 0, 1};
  auto f = build_beacon_mpdu(mac, "X", 0, pack_of({kBasicId}));
  f[30] ^= 0x01;
  EXPECT_FALSE(fcs_valid(f));
  std::string why;
  WifiOdidFrame out;
  // Beacon whose vendor IE has another OUI.
  auto other = build_beacon_mpdu(mac, "X", 0, pack_of({kBasicId}));
  other.resize(other.size() - 4);
  other[36 + 2 + 1 + 3 + 2] = 0x00;
  EXPECT_FALSE(parse_odid_mpdu(other, out, &why));
  EXPECT_EQ(why, std::string("beacon without ODID vendor element"));
  // Information element overrunning the frame.
  auto truncated = build_beacon_mpdu(mac, "X", 0, pack_of({kBasicId}));
  truncated.resize(truncated.size() - 4 - 10);
  EXPECT_FALSE(parse_odid_mpdu(truncated, out, &why));
  EXPECT_EQ(why, std::string("truncated information element"));
  // Valid framing around a malformed pack.
  auto bad_pack = pack_of({kBasicId});
  bad_pack[1] = 26;
  auto bad = build_beacon_mpdu(mac, "X", 0, bad_pack);
  EXPECT_FALSE(parse_odid_mpdu(std::vector<uint8_t>(bad.begin(), bad.end() - 4), out, &why));
  EXPECT_EQ(why.rfind("F3411 decode failed", 0), std::size_t(0));
  // NAN descriptor without the service-info bit.
  auto nan = build_nan_action_mpdu(mac, 1, pack_of({kBasicId}));
  nan.resize(nan.size() - 4);
  nan[41] = 0x00;
  EXPECT_FALSE(parse_odid_mpdu(nan, out, &why));
  // Data frame and short frame.
  std::vector<uint8_t> data(40, 0);
  data[0] = 0x08;
  EXPECT_FALSE(parse_odid_mpdu(data, out, &why));
  EXPECT_EQ(why, std::string("not a management frame"));
  EXPECT_FALSE(parse_odid_mpdu(std::vector<uint8_t>(10, 0), out));
}

MICROTEST_MAIN
