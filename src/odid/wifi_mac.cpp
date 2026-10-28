// src/odid/wifi_mac.cpp — 802.11 MPDU handling for Open Drone ID (beacon vendor IE, NAN SDF).
// Frame layouts follow ASTM F3411-22a / opendroneid-core-c wifi.c (Apache-2.0, reference only).
// GPL-3.0-or-later.
#include <algorithm>
#include <cstdio>

#include "droneid/odid_wifi.h"

namespace droneid::odid {
namespace {

constexpr uint8_t kAstmOui[3] = {0xFA, 0x0B, 0xBC};
constexpr uint8_t kAstmOuiType = 0x0D;
constexpr uint8_t kWfaOui[3] = {0x50, 0x6F, 0x9A};
constexpr uint8_t kNanOuiType = 0x13;
// First 6 bytes of SHA-256("org.opendroneid.remoteid").
constexpr uint8_t kOdidServiceId[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};
constexpr uint8_t kNanClusterId[6] = {0x50, 0x6F, 0x9A, 0x01, 0x00, 0xFF};
constexpr uint8_t kNanNetworkId[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};

bool fail(std::string* why, const char* reason) {
  if (why) *why = reason;
  return false;
}

bool decode_payload(const uint8_t* p, std::size_t n, WifiOdidFrame& out, std::string* why) {
  if (n < 1 + kMessageSize) return fail(why, "ODID service info too short");
  out.message_counter = p[0];
  out.payload.assign(p + 1, p + n);
  out.uas = UasData{};
  out.result = decode(out.payload, out.uas);
  if (!out.result.ok) {
    if (why) *why = "F3411 decode failed: " + out.result.error;
    return false;
  }
  return true;
}

void put_header(std::vector<uint8_t>& f, uint8_t fc0, const uint8_t* da, const Mac& sa, const uint8_t* bssid) {
  f = {fc0, 0x00, 0x00, 0x00};
  f.insert(f.end(), da, da + 6);
  f.insert(f.end(), sa.begin(), sa.end());
  f.insert(f.end(), bssid, bssid + 6);
  f.push_back(0);
  f.push_back(0);
}

}  // namespace

std::string mac_string(const Mac& mac) {
  char s[18];
  std::snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return s;
}

const char* to_string(WifiTransport t) { return t == WifiTransport::Beacon ? "wifi_beacon" : "wifi_nan"; }

uint32_t crc32_ieee(const uint8_t* data, std::size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool fcs_valid(const std::vector<uint8_t>& f) {
  if (f.size() < 5) return false;
  const uint32_t crc = crc32_ieee(f.data(), f.size() - 4);
  const auto* p = f.data() + f.size() - 4;
  return crc == (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

std::vector<uint8_t> append_fcs(std::vector<uint8_t> f) {
  const uint32_t crc = crc32_ieee(f.data(), f.size());
  for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(crc >> (8 * i)));
  return f;
}

bool parse_odid_mpdu(const std::vector<uint8_t>& f, WifiOdidFrame& out, std::string* why) {
  if (f.size() < 24) return fail(why, "MPDU shorter than a management header");
  const uint8_t fc0 = f[0], fc1 = f[1];
  if ((fc0 & 0x03) != 0) return fail(why, "unknown 802.11 protocol version");
  if (((fc0 >> 2) & 0x03) != 0) return fail(why, "not a management frame");
  const int subtype = fc0 >> 4;
  std::size_t pos = 24 + ((fc1 & 0x80) ? 4 : 0);  // +HT Control when the Order bit is set
  if (f.size() < pos) return fail(why, "truncated header");
  std::copy_n(f.begin() + 10, 6, out.src.begin());
  std::copy_n(f.begin() + 16, 6, out.bssid.begin());
  out.ssid.clear();

  if (subtype == 8) {  // beacon
    out.transport = WifiTransport::Beacon;
    pos += 12;         // timestamp, beacon interval, capability
    bool nan_beacon = false;
    while (pos + 2 <= f.size()) {
      const uint8_t id = f[pos], len = f[pos + 1];
      const std::size_t body = pos + 2;
      if (body + len > f.size()) return fail(why, "truncated information element");
      if (id == 0) out.ssid.assign(f.begin() + body, f.begin() + body + len);
      if (id == 0xDD && len >= 4) {
        if (std::equal(kAstmOui, kAstmOui + 3, f.begin() + body) && f[body + 3] == kAstmOuiType)
          return decode_payload(f.data() + body + 4, len - 4u, out, why);
        if (std::equal(kWfaOui, kWfaOui + 3, f.begin() + body) && f[body + 3] == kNanOuiType)
          nan_beacon = true;
      }
      pos = body + len;
    }
    return fail(why, nan_beacon ? "NAN synchronization beacon (no ODID payload)" : "beacon without ODID vendor element");
  }

  if (subtype == 13 || subtype == 14) {  // action / action no-ack: NAN Service Discovery Frame
    out.transport = WifiTransport::Nan;
    if (f.size() < pos + 6) return fail(why, "truncated action frame");
    if (f[pos] != 0x04 || f[pos + 1] != 0x09 || !std::equal(kWfaOui, kWfaOui + 3, f.begin() + pos + 2) ||
        f[pos + 5] != kNanOuiType)
      return fail(why, "action frame is not a NAN service discovery frame");
    pos += 6;
    while (pos + 3 <= f.size()) {
      const uint8_t id = f[pos];
      const std::size_t len = f[pos + 1] | (f[pos + 2] << 8);
      const std::size_t body = pos + 3, end = body + len;
      if (end > f.size()) return fail(why, "truncated NAN attribute");
      if (id == 0x03 && len >= 9 && std::equal(kOdidServiceId, kOdidServiceId + 6, f.begin() + body)) {
        const uint8_t control = f[body + 8];
        std::size_t p = body + 9;
        if (control & 0x04) { if (p >= end) break; p += 1 + f[p]; }  // matching filter
        if (control & 0x08) { if (p >= end) break; p += 1 + f[p]; }  // service response filter
        if (control & 0x40) p += 2;                                   // binding bitmap
        if (!(control & 0x10) || p >= end) return fail(why, "NAN ODID descriptor without service info");
        const std::size_t info_len = f[p];
        if (p + 1 + info_len > end) return fail(why, "NAN service info overruns attribute");
        return decode_payload(f.data() + p + 1, info_len, out, why);
      }
      pos = end;
    }
    return fail(why, "NAN frame without the ODID service descriptor");
  }
  return fail(why, "management subtype not used by ODID");
}

std::vector<uint8_t> build_beacon_mpdu(const Mac& src, const std::string& ssid, uint8_t counter,
                                       const std::vector<uint8_t>& pack) {
  static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::vector<uint8_t> f;
  put_header(f, 0x80, broadcast, src, src.data());
  for (int i = 0; i < 8; ++i) f.push_back(static_cast<uint8_t>(0x11 * i));  // timestamp
  f.insert(f.end(), {0x64, 0x00, 0x20, 0x04});  // 100 TU, short preamble + short slot
  f.push_back(0x00);
  f.push_back(static_cast<uint8_t>(ssid.size()));
  f.insert(f.end(), ssid.begin(), ssid.end());
  f.insert(f.end(), {0x01, 0x01, 0x8C});         // supported rates: 6 Mb/s (basic)
  f.push_back(0xDD);
  f.push_back(static_cast<uint8_t>(4 + 1 + pack.size()));
  f.insert(f.end(), kAstmOui, kAstmOui + 3);
  f.push_back(kAstmOuiType);
  f.push_back(counter);
  f.insert(f.end(), pack.begin(), pack.end());
  return append_fcs(std::move(f));
}

std::vector<uint8_t> build_nan_action_mpdu(const Mac& src, uint8_t counter, const std::vector<uint8_t>& pack) {
  std::vector<uint8_t> f;
  put_header(f, 0xD0, kNanNetworkId, src, kNanClusterId);
  f.insert(f.end(), {0x04, 0x09});
  f.insert(f.end(), kWfaOui, kWfaOui + 3);
  f.push_back(kNanOuiType);
  const std::size_t info_len = 1 + pack.size();
  const std::size_t sda_len = 6 + 3 + 1 + info_len;
  f.push_back(0x03);
  f.push_back(static_cast<uint8_t>(sda_len & 0xFF));
  f.push_back(static_cast<uint8_t>(sda_len >> 8));
  f.insert(f.end(), kOdidServiceId, kOdidServiceId + 6);
  f.insert(f.end(), {0x01, 0x00, 0x10, static_cast<uint8_t>(info_len), counter});
  f.insert(f.end(), pack.begin(), pack.end());
  f.insert(f.end(), {0x0E, 0x04, 0x00, 0x01, 0x00, 0x02, counter});  // service descriptor extension
  return append_fcs(std::move(f));
}

}  // namespace droneid::odid
