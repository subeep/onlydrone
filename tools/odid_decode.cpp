// tools/odid_decode.cpp — offline ASTM F3411 Open Drone ID decoder for recorded Wi-Fi IQ.
// Reads a raw IQ file (cf32/sc16/sc8, .meta sidecar honoured), demodulates 802.11 legacy OFDM
// and DSSS 1/2 Mb/s frames, and prints every FCS-valid Wi-Fi Beacon / NAN frame carrying an
// F3411 payload. Optional JSONL output keeps per-frame evidence (incl. MPDU hex).
// GPL-3.0-or-later.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "droneid/file_source.h"
#include "droneid/odid_wifi.h"

using namespace droneid;
using namespace droneid::odid;

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s FILE [--format cf32|sc16|sc8] [--rate HZ] [--freq-offset HZ] [--secs S]\n"
      "            [--no-ofdm] [--no-dsss] [--jsonl OUT] [--min-frames N] [--quiet]\n"
      "  Input should be centered on the Wi-Fi channel (e.g. 2437 MHz = ch 6) at >= 20 MSPS.\n"
      "  Exit status 3 if fewer than --min-frames ODID frames were decoded.\n", p);
}

static std::string json_escape(const std::string& s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20) o += '?';
    else o += c;
  }
  return o;
}

static std::string hex(const std::vector<uint8_t>& v) {
  static const char d[] = "0123456789abcdef";
  std::string s;
  for (const auto b : v) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string path = argv[1];
  SampleFormat fmt = SampleFormat::cf32;
  WifiDecodeOptions opt;
  double secs = 0;
  std::string jsonl;
  long min_frames = 0;
  bool quiet = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
      return argv[++i];
    };
    auto number = [&]() {
      const auto v = next();
      char* end = nullptr;
      const double d = std::strtod(v.c_str(), &end);
      if (!end || *end || !std::isfinite(d)) { usage(argv[0]); std::exit(2); }
      return d;
    };
    if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate") opt.sample_rate = number();
    else if (a == "--freq-offset") opt.freq_offset = number();
    else if (a == "--secs") secs = number();
    else if (a == "--no-ofdm") opt.ofdm = false;
    else if (a == "--no-dsss") opt.dsss = false;
    else if (a == "--jsonl") jsonl = next();
    else if (a == "--min-frames") min_frames = static_cast<long>(number());
    else if (a == "--quiet") quiet = true;
    else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else { usage(argv[0]); return 2; }
  }
  try {
    FileSource src(path, fmt, opt.sample_rate);
    opt.sample_rate = src.sample_rate();
    if (!std::isfinite(opt.sample_rate) || opt.sample_rate < 20e6 || opt.sample_rate > 250e6) {
      std::fprintf(stderr, "odid_decode: sample rate must be 20..250 MSPS (802.11 needs 20 MHz)\n");
      return 2;
    }
    if (std::abs(opt.freq_offset) > opt.sample_rate / 2 || secs < 0 || (!opt.ofdm && !opt.dsss)) {
      usage(argv[0]);
      return 2;
    }
    std::FILE* js = nullptr;
    if (!jsonl.empty() && !(js = std::fopen(jsonl.c_str(), "w"))) { std::perror("jsonl"); return 1; }
    OdidWifiStream stream(opt);
    const uint64_t limit = secs > 0 ? static_cast<uint64_t>(secs * opt.sample_rate) : UINT64_MAX;
    std::vector<cf32> block(1 << 18);
    uint64_t total = 0, frames = 0;
    std::map<std::string, uint64_t> per_uas;
    auto report = [&](const std::vector<OdidDetection>& found) {
      for (const auto& d : found) {
        ++frames;
        const auto& u = d.frame.uas;
        const std::string id = u.primary_id().empty() ? "mac:" + mac_string(d.frame.src) : u.primary_id();
        ++per_uas[id];
        const double t = static_cast<double>(d.phy.sample_index) / opt.sample_rate;
        if (!quiet) {
          std::printf("odid: t=%.6fs phy=%s rate=%g%s cfo=%+.0fHz snr=%.1fdB transport=%s mac=%s counter=%u id=%s",
                      t, to_string(d.phy.phy), d.phy.rate_mbps, d.phy.short_preamble ? "(short)" : "", d.phy.cfo_hz,
                      d.phy.snr_db, to_string(d.frame.transport), mac_string(d.frame.src).c_str(),
                      d.frame.message_counter, id.c_str());
          if (const auto* b = u.primary_basic_id()) std::printf(" ua=\"%s\"", ua_type_name(b->ua_type));
          if (u.location) {
            const auto& l = *u.location;
            std::printf(" status=%s lat=%.7f lon=%.7f alt_geo=%.1f height=%.1f speed=%.2f dir=%.0f",
                        status_name(l.status), l.lat, l.lon, l.alt_geo_m, l.height_m, l.speed_h_mps, l.direction_deg);
          }
          if (u.system) std::printf(" op_lat=%.7f op_lon=%.7f", u.system->operator_lat, u.system->operator_lon);
          if (u.operator_id) std::printf(" operator=%s", u.operator_id->id.c_str());
          if (u.self_id) std::printf(" self_id=\"%s\"", u.self_id->desc.c_str());
          std::printf(" msgs=%d rejected=%d\n", d.frame.result.decoded, d.frame.result.rejected);
        }
        if (js) {
          std::fprintf(js, "{\"t_s\":%.6f,\"sample\":%lld,\"phy\":\"%s\",\"rate_mbps\":%g,\"short_preamble\":%s,"
                           "\"cfo_hz\":%.1f,\"snr_db\":%.1f,\"quality\":%.3f,\"transport\":\"%s\",\"mac\":\"%s\","
                           "\"counter\":%u,\"uas_id\":\"%s\",\"msgs\":%d,\"rejected\":%d",
                       t, static_cast<long long>(d.phy.sample_index), to_string(d.phy.phy), d.phy.rate_mbps,
                       d.phy.short_preamble ? "true" : "false", d.phy.cfo_hz, d.phy.snr_db, d.phy.quality,
                       to_string(d.frame.transport), mac_string(d.frame.src).c_str(), d.frame.message_counter,
                       json_escape(u.primary_id()).c_str(), d.frame.result.decoded, d.frame.result.rejected);
          if (u.location)
            std::fprintf(js, ",\"lat\":%.7f,\"lon\":%.7f,\"alt_geo_m\":%.1f,\"height_m\":%.1f,\"speed_h_mps\":%.2f,"
                             "\"direction_deg\":%.0f,\"status\":%u,\"loc_timestamp_raw\":%u",
                         u.location->lat, u.location->lon, u.location->alt_geo_m, u.location->height_m,
                         u.location->speed_h_mps, u.location->direction_deg, u.location->status,
                         u.location->timestamp_raw);
          if (u.system)
            std::fprintf(js, ",\"operator_lat\":%.7f,\"operator_lon\":%.7f", u.system->operator_lat, u.system->operator_lon);
          if (u.operator_id) std::fprintf(js, ",\"operator_id\":\"%s\"", json_escape(u.operator_id->id).c_str());
          std::fprintf(js, ",\"mpdu_hex\":\"%s\"}\n", hex(d.phy.mpdu).c_str());
        }
      }
    };
    int64_t ts = 0;
    while (total < limit) {
      const auto got = src.recv(block.data(), std::min<uint64_t>(block.size(), limit - total), ts);
      if (!got) break;
      total += got;
      report(stream.feed(block.data(), got));
    }
    report(stream.feed(nullptr, 0, true));
    if (js) std::fclose(js);
    const auto& s = stream.stats();
    std::printf("summary: samples=%llu seconds=%.3f odid_frames=%llu uas=%zu ofdm_sync=%llu ofdm_signal_ok=%llu "
                "ofdm_signal_bad=%llu dsss_sync=%llu dsss_header_ok=%llu dsss_header_bad=%llu dsss_cck_skipped=%llu "
                "fcs_ok=%llu fcs_fail=%llu non_odid=%llu odid_rejected=%llu\n",
                static_cast<unsigned long long>(total), static_cast<double>(total) / opt.sample_rate,
                static_cast<unsigned long long>(frames), per_uas.size(),
                static_cast<unsigned long long>(s.ofdm_sync), static_cast<unsigned long long>(s.ofdm_signal_ok),
                static_cast<unsigned long long>(s.ofdm_signal_bad), static_cast<unsigned long long>(s.dsss_sync),
                static_cast<unsigned long long>(s.dsss_header_ok), static_cast<unsigned long long>(s.dsss_header_bad),
                static_cast<unsigned long long>(s.dsss_unsupported_rate), static_cast<unsigned long long>(s.fcs_ok),
                static_cast<unsigned long long>(s.fcs_fail), static_cast<unsigned long long>(s.non_odid_frames),
                static_cast<unsigned long long>(s.odid_rejected));
    for (const auto& [id, n] : per_uas)
      std::printf("  uas: id=%s frames=%llu\n", id.c_str(), static_cast<unsigned long long>(n));
    return static_cast<long>(frames) < min_frames ? 3 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "odid_decode: %s\n", e.what());
    return 1;
  }
}
