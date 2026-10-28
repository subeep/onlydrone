// src/odid/wifi_decoder.cpp — top-level Open Drone ID Wi-Fi decoding (PHY -> MAC -> F3411) and
// the overlapping-window stream wrapper.
// GPL-3.0-or-later.
#include <algorithm>
#include <cstdlib>
#include <future>

#include "wifi_internal.hpp"

namespace droneid::odid {

const char* to_string(WifiPhy p) { return p == WifiPhy::Ofdm ? "ofdm" : "dsss"; }

void WifiStats::add(const WifiStats& o) {
  ofdm_sync += o.ofdm_sync; ofdm_signal_ok += o.ofdm_signal_ok; ofdm_signal_bad += o.ofdm_signal_bad;
  dsss_sync += o.dsss_sync; dsss_header_ok += o.dsss_header_ok; dsss_header_bad += o.dsss_header_bad;
  dsss_unsupported_rate += o.dsss_unsupported_rate;
  fcs_ok += o.fcs_ok; fcs_fail += o.fcs_fail;
  odid_frames += o.odid_frames; non_odid_frames += o.non_odid_frames; odid_rejected += o.odid_rejected;
}

namespace detail {

std::vector<PhyFrame> decode_block(const std::vector<cf32>& iq, const WifiDecodeOptions& opt,
                                   const BlockLimits& lim, WifiStats& st) {
  // OFDM (20 MSPS) and DSSS (22 MSPS, 2 samples/chip) run concurrently on the same block.
  WifiStats dsss_stats;
  auto dsss = std::async(std::launch::async, [&] {
    if (!opt.dsss) return std::vector<PhyFrame>{};
    return dsss_receive(resample(iq, opt.sample_rate, 22e6, opt.freq_offset), opt.sample_rate, opt, lim, dsss_stats);
  });
  std::vector<PhyFrame> out;
  if (opt.ofdm) out = ofdm_receive(resample(iq, opt.sample_rate, 20e6, opt.freq_offset), opt.sample_rate, opt, lim, st);
  auto f = dsss.get();
  st.add(dsss_stats);
  out.insert(out.end(), std::make_move_iterator(f.begin()), std::make_move_iterator(f.end()));
  std::sort(out.begin(), out.end(), [](const PhyFrame& a, const PhyFrame& b) { return a.sample_index < b.sample_index; });
  return out;
}

}  // namespace detail

namespace {

std::vector<OdidDetection> to_odid(std::vector<PhyFrame> frames, WifiStats& st) {
  std::vector<OdidDetection> out;
  for (auto& f : frames) {
    if (!f.fcs_ok) continue;
    OdidDetection d;
    const std::vector<uint8_t> mpdu(f.mpdu.begin(), f.mpdu.end() - 4);
    std::string why;
    if (parse_odid_mpdu(mpdu, d.frame, &why)) {
      ++st.odid_frames;
      d.phy = std::move(f);
      out.push_back(std::move(d));
    } else if (why.rfind("F3411", 0) == 0) {
      ++st.odid_rejected;
    } else {
      ++st.non_odid_frames;
    }
  }
  return out;
}

}  // namespace

std::vector<PhyFrame> decode_wifi_phy(const std::vector<cf32>& iq, const WifiDecodeOptions& opt, WifiStats* stats) {
  WifiStats st;
  auto out = detail::decode_block(iq, opt, {}, st);
  if (stats) stats->add(st);
  return out;
}

std::vector<OdidDetection> decode_odid_wifi(const std::vector<cf32>& iq, const WifiDecodeOptions& opt, WifiStats* stats) {
  WifiStats st;
  auto out = to_odid(detail::decode_block(iq, opt, {}, st), st);
  if (stats) stats->add(st);
  return out;
}

OdidWifiStream::OdidWifiStream(const WifiDecodeOptions& opt, std::size_t window, double overlap_seconds)
    : opt_(opt), window_(std::max<std::size_t>(window, 1 << 14)),
      overlap_(static_cast<std::size_t>(overlap_seconds * opt.sample_rate) + 4096) {}

std::vector<OdidDetection> OdidWifiStream::feed(const cf32* samples, std::size_t count, bool final) {
  if (count) buffer_.insert(buffer_.end(), samples, samples + count);
  std::vector<OdidDetection> out;
  while (buffer_.size() >= window_ + overlap_ || (final && !buffer_.empty())) {
    const std::size_t len = std::min(buffer_.size(), window_ + overlap_);
    const bool last = final && len == buffer_.size();
    const std::vector<cf32> block(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(len));
    detail::BlockLimits lim;
    if (base_ != 0) lim.accept_from = 0;
    if (!last) lim.accept_before = static_cast<int64_t>(window_);
    auto found = to_odid(detail::decode_block(block, opt_, lim, stats_), stats_);
    for (auto& d : found) {
      d.phy.sample_index += static_cast<int64_t>(base_);
      out.push_back(std::move(d));
    }
    const std::size_t consumed = last ? len : window_;
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    base_ += consumed;
  }
  // Belt and braces against a frame whose start estimate straddles a window edge: drop repeats of
  // the same MPDU within 64 samples.
  std::vector<OdidDetection> unique;
  for (auto& d : out) {
    bool dup = false;
    for (const auto& r : recent_)
      dup |= r.first == d.phy.mpdu && std::llabs(r.second - d.phy.sample_index) < 64;
    if (dup) continue;
    recent_.emplace_back(d.phy.mpdu, d.phy.sample_index);
    if (recent_.size() > 16) recent_.erase(recent_.begin());
    unique.push_back(std::move(d));
  }
  return unique;
}

}  // namespace droneid::odid
