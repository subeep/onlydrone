// droneid/odid.h — ASTM F3411 Open Drone ID (Remote ID) message parser.
//
// Pure byte-level code: no DSP, no transport. Decodes the 25-byte F3411 messages (Basic ID,
// Location/Vector, Authentication, Self-ID, System, Operator ID) and the Message Pack that
// Wi-Fi Beacon / NAN carry. Field scaling and validity rules follow F3411-22a as implemented by
// opendroneid-core-c (Apache-2.0, consulted as a reference; see THIRD_PARTY.md). Encoders are
// provided only to build test fixtures; the application never transmits.
// GPL-3.0-or-later.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace droneid::odid {

constexpr std::size_t kMessageSize = 25;
constexpr int kMaxPackMessages = 9;
constexpr int kMaxAuthPages = 16;
constexpr int kMaxBasicIds = 2;
constexpr int kMaxProtocolVersion = 2;   // 0 = F3411-19, 1 = F3411-20, 2 = F3411-22

enum class MessageType : uint8_t {
  BasicId = 0, Location = 1, Auth = 2, SelfId = 3, System = 4, OperatorId = 5,
  Pack = 0xF, Invalid = 0xFF
};

using Message = std::array<uint8_t, kMessageSize>;

struct BasicId {
  uint8_t id_type = 0;       // 0 none, 1 serial (CTA-2063-A), 2 CAA registration, 3 UTM UUID, 4 session
  uint8_t ua_type = 0;       // 0 none .. 15 other (see ua_type_name)
  std::string uas_id;        // up to 20 chars, trailing NULs stripped
};

struct Location {
  uint8_t status = 0;        // 0 undeclared, 1 ground, 2 airborne, 3 emergency, 4 RID failure
  uint8_t height_type = 0;   // 0 above takeoff, 1 AGL
  double direction_deg = 361;     // 361 = unknown
  double speed_h_mps = 255;       // 255 = unknown
  double speed_v_mps = 63;        // 63 = unknown
  double lat = 0, lon = 0;        // degrees; both 0 = unknown
  double alt_baro_m = -1000, alt_geo_m = -1000, height_m = -1000;  // -1000 = unknown
  uint8_t h_accuracy = 0, v_accuracy = 0, baro_accuracy = 0, speed_accuracy = 0, ts_accuracy = 0;
  uint16_t timestamp_raw = 0xFFFF;  // 1/10 s since the hour; 0xFFFF = unknown

  bool position_valid() const { return lat != 0 || lon != 0; }
  bool timestamp_valid() const { return timestamp_raw != 0xFFFF; }
  double timestamp_s() const { return timestamp_raw / 10.0; }
};

struct AuthPage {
  uint8_t auth_type = 0;
  uint8_t page = 0;
  uint8_t last_page_index = 0;   // page 0 only
  uint8_t length = 0;            // page 0 only
  uint32_t timestamp = 0;        // page 0 only: seconds since 2019-01-01T00:00Z
  std::vector<uint8_t> data;     // 17 bytes (page 0) or 23 bytes
};

struct SelfId {
  uint8_t desc_type = 0;
  std::string desc;
};

struct System {
  uint8_t operator_location_type = 0;  // 0 takeoff, 1 live GNSS, 2 fixed
  uint8_t classification_type = 0;     // 0 undeclared, 1 EU
  double operator_lat = 0, operator_lon = 0;
  uint16_t area_count = 0;
  uint16_t area_radius_m = 0;
  double area_ceiling_m = -1000, area_floor_m = -1000;
  uint8_t category_eu = 0, class_eu = 0;
  double operator_alt_geo_m = -1000;
  uint32_t timestamp = 0;              // seconds since 2019-01-01T00:00Z

  bool operator_position_valid() const { return operator_lat != 0 || operator_lon != 0; }
};

struct OperatorId {
  uint8_t id_type = 0;
  std::string id;
};

// Everything decoded for one transmitter. Fields are optional because a single broadcast may
// carry any subset of messages.
struct UasData {
  int protocol_version = -1;
  std::vector<BasicId> basic_ids;          // at most kMaxBasicIds, one per id_type
  std::optional<Location> location;
  std::map<int, AuthPage> auth;            // keyed by page
  std::optional<SelfId> self_id;
  std::optional<System> system;
  std::optional<OperatorId> operator_id;

  // Preferred identity: serial number, then any other Basic ID.
  std::string primary_id() const;
  const BasicId* primary_basic_id() const;
};

struct DecodeResult {
  bool ok = false;
  MessageType type = MessageType::Invalid;
  int decoded = 0;     // messages accepted (1 for a single message, n for a pack)
  int rejected = 0;    // messages inside a valid pack that failed validation
  std::string error;   // first reason for a failure/rejection
};

// Decodes one message (25 bytes) or a Message Pack (3 + 25*n bytes) into `out`, merging with
// whatever `out` already holds. Structural errors (short buffer, bad pack header, illegal pack
// content) fail the whole call; inside a valid pack, individually invalid messages are rejected
// and counted while the rest are kept.
DecodeResult decode(const uint8_t* data, std::size_t size, UasData& out);
inline DecodeResult decode(const std::vector<uint8_t>& bytes, UasData& out) {
  return decode(bytes.data(), bytes.size(), out);
}

MessageType message_type(uint8_t header);
const char* to_string(MessageType type);
const char* ua_type_name(uint8_t ua_type);
const char* id_type_name(uint8_t id_type);
const char* status_name(uint8_t status);

// Copies the fields present in `from` over `into` (newer data wins).
void merge(UasData& into, const UasData& from);

// --- encoders (test fixtures only) ---
Message encode(const BasicId& v, uint8_t version = 2);
Message encode(const Location& v, uint8_t version = 2);
Message encode(const AuthPage& v, uint8_t version = 2);
Message encode(const SelfId& v, uint8_t version = 2);
Message encode(const System& v, uint8_t version = 2);
Message encode(const OperatorId& v, uint8_t version = 2);
std::vector<uint8_t> encode_pack(const std::vector<Message>& messages, uint8_t version = 2);

}  // namespace droneid::odid
