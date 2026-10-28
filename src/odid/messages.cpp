// src/odid/messages.cpp — ASTM F3411 message decode/encode (see droneid/odid.h).
// Clean-room implementation; scaling rules cross-checked against opendroneid-core-c (Apache-2.0).
// GPL-3.0-or-later.
#include "droneid/odid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace droneid::odid {
namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }

double latlon(const uint8_t* p) { return static_cast<int32_t>(le32(p)) / 1e7; }
double altitude(uint16_t v) { return v * 0.5 - 1000.0; }

int32_t enc_latlon(double deg) {
  return static_cast<int32_t>(std::clamp<double>(std::llround(deg * 1e7), -1.8e9, 1.8e9));
}
uint16_t enc_alt(double m) {
  return static_cast<uint16_t>(std::clamp<long>(std::lround((m + 1000.0) / 0.5), 0, 65535));
}

// Basic ID / Operator ID text. Serial and CAA ids are ASCII; UUID / session ids may be binary.
std::string id_text(const uint8_t* p, std::size_t n, bool ascii) {
  std::size_t len = n;
  while (len > 0 && p[len - 1] == 0) --len;
  bool printable = true;
  for (std::size_t i = 0; i < len; ++i) printable &= p[i] >= 0x20 && p[i] < 0x7F;
  if (ascii || printable) {
    std::string s;
    for (std::size_t i = 0; i < len; ++i) s += (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '?';
    return s;
  }
  static const char hex[] = "0123456789abcdef";
  std::string s = "0x";
  for (std::size_t i = 0; i < n; ++i) { s += hex[p[i] >> 4]; s += hex[p[i] & 15]; }
  return s;
}

bool in_range(double lat, double lon) { return std::abs(lat) <= 90.0 && std::abs(lon) <= 180.0; }

void put_text(uint8_t* p, std::size_t n, const std::string& s) {
  std::memcpy(p, s.data(), std::min(n, s.size()));
}

// Decodes one non-pack message. Returns an empty string on success, else the rejection reason.
std::string decode_one(const uint8_t* m, UasData& out) {
  const auto type = message_type(m[0]);
  const int version = m[0] & 0x0F;
  if (version > kMaxProtocolVersion) return "unsupported protocol version " + std::to_string(version);
  switch (type) {
    case MessageType::BasicId: {
      BasicId v;
      v.id_type = m[1] >> 4;
      v.ua_type = m[1] & 0x0F;
      v.uas_id = id_text(m + 2, 20, v.id_type == 1 || v.id_type == 2);
      auto it = std::find_if(out.basic_ids.begin(), out.basic_ids.end(),
                             [&](const BasicId& b) { return b.id_type == v.id_type; });
      if (it != out.basic_ids.end()) *it = v;
      else if (static_cast<int>(out.basic_ids.size()) < kMaxBasicIds) out.basic_ids.push_back(v);
      else out.basic_ids.back() = v;
      break;
    }
    case MessageType::Location: {
      Location v;
      v.status = m[1] >> 4;
      v.height_type = (m[1] >> 2) & 1;
      const bool ew = (m[1] >> 1) & 1, mult = m[1] & 1;
      v.direction_deg = m[2] + (ew ? 180.0 : 0.0);
      v.speed_h_mps = mult ? m[3] * 0.75 + 255 * 0.25 : m[3] * 0.25;
      v.speed_v_mps = static_cast<int8_t>(m[4]) * 0.5;
      v.lat = latlon(m + 5);
      v.lon = latlon(m + 9);
      v.alt_baro_m = altitude(le16(m + 13));
      v.alt_geo_m = altitude(le16(m + 15));
      v.height_m = altitude(le16(m + 17));
      v.h_accuracy = m[19] & 0x0F;
      v.v_accuracy = m[19] >> 4;
      v.speed_accuracy = m[20] & 0x0F;
      v.baro_accuracy = m[20] >> 4;
      v.timestamp_raw = le16(m + 21);
      v.ts_accuracy = m[23] & 0x0F;
      if (!in_range(v.lat, v.lon)) return "location latitude/longitude out of range";
      if (v.direction_deg > 360 && v.direction_deg != 361) return "location direction out of range";
      if (v.timestamp_raw != 0xFFFF && v.timestamp_raw > 36000) return "location timestamp out of range";
      out.location = v;
      break;
    }
    case MessageType::Auth: {
      AuthPage v;
      v.auth_type = m[1] >> 4;
      v.page = m[1] & 0x0F;
      if (v.page == 0) {
        v.last_page_index = m[2];
        v.length = m[3];
        v.timestamp = le32(m + 4);
        v.data.assign(m + 8, m + 25);
        if (v.last_page_index >= kMaxAuthPages) return "auth last page index out of range";
        if (v.length > 17 + 23 * v.last_page_index) return "auth length exceeds pages";
      } else {
        v.data.assign(m + 2, m + 25);
      }
      out.auth[v.page] = v;
      break;
    }
    case MessageType::SelfId: {
      SelfId v;
      v.desc_type = m[1];
      v.desc = id_text(m + 2, 23, true);
      out.self_id = v;
      break;
    }
    case MessageType::System: {
      System v;
      v.operator_location_type = m[1] & 0x03;
      v.classification_type = (m[1] >> 2) & 0x07;
      v.operator_lat = latlon(m + 2);
      v.operator_lon = latlon(m + 6);
      v.area_count = le16(m + 10);
      v.area_radius_m = static_cast<uint16_t>(m[12] * 10);
      v.area_ceiling_m = altitude(le16(m + 13));
      v.area_floor_m = altitude(le16(m + 15));
      v.class_eu = m[17] & 0x0F;
      v.category_eu = m[17] >> 4;
      v.operator_alt_geo_m = altitude(le16(m + 18));
      v.timestamp = le32(m + 20);
      if (!in_range(v.operator_lat, v.operator_lon)) return "operator latitude/longitude out of range";
      out.system = v;
      break;
    }
    case MessageType::OperatorId: {
      OperatorId v;
      v.id_type = m[1];
      v.id = id_text(m + 2, 20, true);
      out.operator_id = v;
      break;
    }
    case MessageType::Pack: return "nested message pack";
    case MessageType::Invalid: return "invalid message type " + std::to_string(m[0] >> 4);
  }
  out.protocol_version = std::max(out.protocol_version, version);
  return {};
}

}  // namespace

MessageType message_type(uint8_t header) {
  const int t = header >> 4;
  if (t <= 5) return static_cast<MessageType>(t);
  if (t == 0xF) return MessageType::Pack;
  return MessageType::Invalid;
}

const char* to_string(MessageType type) {
  switch (type) {
    case MessageType::BasicId: return "BasicID";
    case MessageType::Location: return "Location";
    case MessageType::Auth: return "Auth";
    case MessageType::SelfId: return "SelfID";
    case MessageType::System: return "System";
    case MessageType::OperatorId: return "OperatorID";
    case MessageType::Pack: return "MessagePack";
    case MessageType::Invalid: break;
  }
  return "Invalid";
}

const char* ua_type_name(uint8_t t) {
  static const char* names[16] = {
      "None", "Aeroplane", "Helicopter/Multirotor", "Gyroplane", "Hybrid lift (VTOL)",
      "Ornithopter", "Glider", "Kite", "Free balloon", "Captive balloon", "Airship",
      "Free fall/Parachute", "Rocket", "Tethered powered aircraft", "Ground obstacle", "Other"};
  return names[t & 15];
}

const char* id_type_name(uint8_t t) {
  switch (t) {
    case 0: return "None";
    case 1: return "Serial (CTA-2063-A)";
    case 2: return "CAA registration";
    case 3: return "UTM UUID";
    case 4: return "Specific session";
  }
  return "Reserved";
}

const char* status_name(uint8_t s) {
  switch (s) {
    case 0: return "Undeclared";
    case 1: return "Ground";
    case 2: return "Airborne";
    case 3: return "Emergency";
    case 4: return "RID system failure";
  }
  return "Reserved";
}

const BasicId* UasData::primary_basic_id() const {
  for (const auto& b : basic_ids) if (b.id_type == 1 && !b.uas_id.empty()) return &b;
  for (const auto& b : basic_ids) if (!b.uas_id.empty()) return &b;
  return nullptr;
}

std::string UasData::primary_id() const {
  const auto* b = primary_basic_id();
  return b ? b->uas_id : std::string();
}

DecodeResult decode(const uint8_t* data, std::size_t size, UasData& out) {
  DecodeResult r;
  if (!data || size < kMessageSize) { r.error = "short message"; return r; }
  r.type = message_type(data[0]);
  if (r.type != MessageType::Pack) {
    UasData tmp = out;
    r.error = decode_one(data, tmp);
    r.ok = r.error.empty();
    if (r.ok) { out = std::move(tmp); r.decoded = 1; }
    return r;
  }
  if ((data[0] & 0x0F) > kMaxProtocolVersion) { r.error = "unsupported pack protocol version"; return r; }
  if (data[1] != kMessageSize) { r.error = "pack single message size is not 25"; return r; }
  const int count = data[2];
  if (count < 1 || count > kMaxPackMessages) { r.error = "pack message count out of range"; return r; }
  if (size < 3 + kMessageSize * static_cast<std::size_t>(count)) { r.error = "pack truncated"; return r; }
  // Content check, as in opendroneid-core-c checkPackContent(): no packs, at most two Basic IDs,
  // 16 auth pages and one of every other message type.
  int counts[6] = {};
  for (int i = 0; i < count; ++i) {
    const auto t = message_type(data[3 + kMessageSize * i]);
    if (t == MessageType::Pack || t == MessageType::Invalid) { r.error = "illegal message type inside pack"; return r; }
    ++counts[static_cast<int>(t)];
  }
  if (counts[0] > kMaxBasicIds || counts[1] > 1 || counts[2] > kMaxAuthPages || counts[3] > 1 ||
      counts[4] > 1 || counts[5] > 1) {
    r.error = "duplicate message types inside pack";
    return r;
  }
  for (int i = 0; i < count; ++i) {
    const auto reason = decode_one(data + 3 + kMessageSize * i, out);
    if (reason.empty()) ++r.decoded;
    else { ++r.rejected; if (r.error.empty()) r.error = reason; }
  }
  r.ok = r.decoded > 0;
  return r;
}

void merge(UasData& into, const UasData& from) {
  into.protocol_version = std::max(into.protocol_version, from.protocol_version);
  for (const auto& b : from.basic_ids) {
    auto it = std::find_if(into.basic_ids.begin(), into.basic_ids.end(),
                           [&](const BasicId& x) { return x.id_type == b.id_type; });
    if (it != into.basic_ids.end()) *it = b;
    else if (static_cast<int>(into.basic_ids.size()) < kMaxBasicIds) into.basic_ids.push_back(b);
  }
  if (from.location) into.location = from.location;
  for (const auto& [page, a] : from.auth) into.auth[page] = a;
  if (from.self_id) into.self_id = from.self_id;
  if (from.system) into.system = from.system;
  if (from.operator_id) into.operator_id = from.operator_id;
}

Message encode(const BasicId& v, uint8_t version) {
  Message m{};
  m[0] = static_cast<uint8_t>(0x00 | (version & 0x0F));
  m[1] = static_cast<uint8_t>((v.id_type << 4) | (v.ua_type & 0x0F));
  put_text(m.data() + 2, 20, v.uas_id);
  return m;
}

Message encode(const Location& v, uint8_t version) {
  Message m{};
  m[0] = static_cast<uint8_t>(0x10 | (version & 0x0F));
  int dir = static_cast<int>(std::lround(v.direction_deg));
  if (dir == 360) dir = 0;
  const bool ew = dir >= 180;
  if (ew) dir -= 180;
  bool mult = false;
  long speed;
  if (v.speed_h_mps <= 255 * 0.25) speed = std::lround(v.speed_h_mps / 0.25);
  else { mult = true; speed = std::lround((v.speed_h_mps - 255 * 0.25) / 0.75); }
  m[1] = static_cast<uint8_t>((v.status << 4) | ((v.height_type & 1) << 2) | (ew << 1) | mult);
  m[2] = static_cast<uint8_t>(std::clamp(dir, 0, 255));
  m[3] = static_cast<uint8_t>(std::clamp<long>(speed, 0, 255));
  m[4] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp<long>(std::lround(v.speed_v_mps / 0.5), -128, 127)));
  put32(m.data() + 5, static_cast<uint32_t>(enc_latlon(v.lat)));
  put32(m.data() + 9, static_cast<uint32_t>(enc_latlon(v.lon)));
  put16(m.data() + 13, enc_alt(v.alt_baro_m));
  put16(m.data() + 15, enc_alt(v.alt_geo_m));
  put16(m.data() + 17, enc_alt(v.height_m));
  m[19] = static_cast<uint8_t>((v.v_accuracy << 4) | (v.h_accuracy & 0x0F));
  m[20] = static_cast<uint8_t>((v.baro_accuracy << 4) | (v.speed_accuracy & 0x0F));
  put16(m.data() + 21, v.timestamp_raw);
  m[23] = v.ts_accuracy & 0x0F;
  return m;
}

Message encode(const AuthPage& v, uint8_t version) {
  Message m{};
  m[0] = static_cast<uint8_t>(0x20 | (version & 0x0F));
  m[1] = static_cast<uint8_t>((v.auth_type << 4) | (v.page & 0x0F));
  if (v.page == 0) {
    m[2] = v.last_page_index;
    m[3] = v.length;
    put32(m.data() + 4, v.timestamp);
    std::copy_n(v.data.begin(), std::min<std::size_t>(17, v.data.size()), m.begin() + 8);
  } else {
    std::copy_n(v.data.begin(), std::min<std::size_t>(23, v.data.size()), m.begin() + 2);
  }
  return m;
}

Message encode(const SelfId& v, uint8_t version) {
  Message m{};
  m[0] = static_cast<uint8_t>(0x30 | (version & 0x0F));
  m[1] = v.desc_type;
  put_text(m.data() + 2, 23, v.desc);
  return m;
}

Message encode(const System& v, uint8_t version) {
  Message m{};
  m[0] = static_cast<uint8_t>(0x40 | (version & 0x0F));
  m[1] = static_cast<uint8_t>(((v.classification_type & 7) << 2) | (v.operator_location_type & 3));
  put32(m.data() + 2, static_cast<uint32_t>(enc_latlon(v.operator_lat)));
  put32(m.data() + 6, static_cast<uint32_t>(enc_latlon(v.operator_lon)));
  put16(m.data() + 10, v.area_count);
  m[12] = static_cast<uint8_t>(std::min(255, v.area_radius_m / 10));
  put16(m.data() + 13, enc_alt(v.area_ceiling_m));
  put16(m.data() + 15, enc_alt(v.area_floor_m));
  m[17] = static_cast<uint8_t>((v.category_eu << 4) | (v.class_eu & 0x0F));
  put16(m.data() + 18, enc_alt(v.operator_alt_geo_m));
  put32(m.data() + 20, v.timestamp);
  return m;
}

Message encode(const OperatorId& v, uint8_t version) {
  Message m{};
  m[0] = static_cast<uint8_t>(0x50 | (version & 0x0F));
  m[1] = v.id_type;
  put_text(m.data() + 2, 20, v.id);
  return m;
}

std::vector<uint8_t> encode_pack(const std::vector<Message>& messages, uint8_t version) {
  std::vector<uint8_t> out = {static_cast<uint8_t>(0xF0 | (version & 0x0F)),
                              static_cast<uint8_t>(kMessageSize),
                              static_cast<uint8_t>(messages.size())};
  for (const auto& m : messages) out.insert(out.end(), m.begin(), m.end());
  return out;
}

}  // namespace droneid::odid
