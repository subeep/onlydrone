// src/odid/wifi_dsss.cpp — IEEE 802.11b DSSS PHY, 1 Mb/s DBPSK and 2 Mb/s DQPSK, long and short
// PLCP preambles. Runs at 22 MSPS (2 samples per 11 Mchip/s Barker chip). Receiver: Barker
// matched filter, periodicity sync, early/late symbol tracking, decision-directed carrier
// tracking, self-synchronizing descrambler, SFD search, PLCP CRC-16. CCK (5.5/11 Mb/s) PSDUs are
// detected and counted but not demodulated. Transmitter: fixture synthesis only.
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <complex>

#include "wifi_internal.hpp"

namespace droneid::odid {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 22e6;
constexpr int kSps = 2;               // samples per chip
constexpr int kSym = 11 * kSps;       // samples per symbol
constexpr int kBarker[11] = {1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1};
constexpr uint16_t kLongSfd = 0x05CF;   // 0xF3A0 sent LSB first, read MSB-first in time order
constexpr uint16_t kShortSfd = 0xF3A0;  // 0x05CF sent LSB first

void push_lsb_first(std::vector<uint8_t>& bits, unsigned value, int n) {
  for (int i = 0; i < n; ++i) bits.push_back(static_cast<uint8_t>((value >> i) & 1));
}

// DQPSK phase step for dibit (d0 first in time): 00 -> 0, 01 -> pi/2, 11 -> pi, 10 -> 3pi/2.
double dqpsk_step(uint8_t d0, uint8_t d1) {
  if (!d0 && !d1) return 0;
  if (!d0 && d1) return kPi / 2;
  if (d0 && d1) return kPi;
  return 3 * kPi / 2;
}

}  // namespace

namespace detail {

std::vector<uint8_t> plcp_crc16_bits(const std::vector<uint8_t>& bits32) {
  uint16_t crc = 0xFFFF;
  for (const auto b : bits32) {
    const bool fb = ((crc >> 15) & 1) ^ (b & 1);
    crc = static_cast<uint16_t>(crc << 1);
    if (fb) crc ^= 0x1021;
  }
  crc = static_cast<uint16_t>(~crc);
  std::vector<uint8_t> out;
  for (int i = 15; i >= 0; --i) out.push_back(static_cast<uint8_t>((crc >> i) & 1));
  return out;
}

}  // namespace detail

std::vector<cf32> synth_dsss_ppdu(const std::vector<uint8_t>& psdu, int rate_mbps, bool short_preamble,
                                  double fs_out) {
  if ((rate_mbps != 1 && rate_mbps != 2) || psdu.empty() || psdu.size() > 4000) return {};
  std::vector<uint8_t> bits;
  if (short_preamble) {
    bits.assign(56, 0);
    push_lsb_first(bits, 0x05CF, 16);
  } else {
    bits.assign(128, 1);
    push_lsb_first(bits, 0xF3A0, 16);
  }
  std::vector<uint8_t> header;
  push_lsb_first(header, rate_mbps == 1 ? 0x0A : 0x14, 8);
  push_lsb_first(header, 0x04, 8);  // SERVICE: locked clocks
  push_lsb_first(header, static_cast<unsigned>(psdu.size() * 8 / rate_mbps), 16);
  const auto crc = detail::plcp_crc16_bits(header);
  header.insert(header.end(), crc.begin(), crc.end());
  const std::size_t header_at = bits.size();
  bits.insert(bits.end(), header.begin(), header.end());
  const std::size_t psdu_at = bits.size();
  for (const auto byte : psdu) push_lsb_first(bits, byte, 8);
  // Self-synchronizing scrambler: out = in ^ out[-4] ^ out[-7].
  unsigned state = short_preamble ? 0x1B : 0x6C;  // z1..z7, z1 in bit 0
  for (auto& b : bits) {
    const uint8_t s = static_cast<uint8_t>(((state >> 3) ^ (state >> 6)) & 1);
    b ^= s;
    state = ((state << 1) | b) & 0x7F;
  }
  // Differential modulation; header is DQPSK after a short preamble.
  std::vector<double> phases;
  double phase = 0;
  for (std::size_t i = 0; i < bits.size();) {
    const bool two = (i >= psdu_at && rate_mbps == 2) || (short_preamble && i >= header_at && i < psdu_at);
    if (two) { phase += dqpsk_step(bits[i], bits[i + 1]); i += 2; }
    else { phase += bits[i] ? kPi : 0.0; i += 1; }
    phases.push_back(phase);
  }
  std::vector<cf32> chips;
  chips.reserve(phases.size() * kSym + 64);
  for (int i = 0; i < 32; ++i) chips.emplace_back(0, 0);
  for (const double p : phases) {
    const cf32 sym = std::polar(1.0f, static_cast<float>(p));
    for (int c = 0; c < 11; ++c)
      for (int s = 0; s < kSps; ++s) chips.push_back(sym * static_cast<float>(kBarker[c]));
  }
  for (int i = 0; i < 32; ++i) chips.emplace_back(0, 0);
  return resample(chips, kFs, fs_out);
}

namespace detail {

std::vector<PhyFrame> dsss_receive(const std::vector<cf32>& x, double input_rate, const WifiDecodeOptions& opt,
                                   const BlockLimits& lim, WifiStats& st) {
  std::vector<PhyFrame> frames;
  const int64_t n = static_cast<int64_t>(x.size());
  if (n < 64 * kSym) return frames;
  const double to_input = input_rate / kFs;
  const int64_t m = n - 10 * kSps;
  std::vector<cf32> c(static_cast<std::size_t>(m));
  std::vector<float> r(static_cast<std::size_t>(m));
  for (int64_t i = 0; i < m; ++i) {
    cf32 acc(0, 0);
    float e = 0;
    for (int k = 0; k < 11; ++k) {
      const cf32 v = x[i + kSps * k];
      acc += kBarker[k] > 0 ? v : -v;
      e += std::norm(v);
    }
    c[i] = acc;
    r[i] = e > 1e-20f ? std::norm(acc) / (11 * e) : 0.0f;
  }
  // Periodicity metric over 16 symbols.
  constexpr int kAvg = 16;
  const int64_t lim_r = m - kAvg * kSym;
  if (lim_r <= 0) return frames;
  auto big_r = [&](int64_t i) {
    float s = 0;
    for (int k = 0; k < kAvg; ++k) s += r[i + k * kSym];
    return s / kAvg;
  };
  const int64_t scan_end = std::min<int64_t>(
      lim_r, lim.accept_before == INT64_MAX ? lim_r : static_cast<int64_t>(lim.accept_before / to_input) + 145 * kSym);
  const float thr = opt.dsss_sync_threshold;
  for (int64_t i = 0; i < scan_end;) {
    if (r[i] < thr || big_r(i) < thr) { ++i; continue; }
    // Pick the best symbol phase within one symbol period.
    int64_t t = i;
    float best = big_r(i);
    for (int64_t k = i + 1; k < std::min<int64_t>(i + kSym, lim_r); ++k) {
      const float v = big_r(k);
      if (v > best) { best = v; t = k; }
    }
    ++st.dsss_sync;
    // Demodulate symbols on the fly; state machine: search SFD -> header -> PSDU.
    double phi = 0;     // carrier phase drift per symbol
    {
      std::complex<double> acc = 0;
      for (int k = 1; k < kAvg && t + k * kSym < m; ++k) {
        const auto d = std::complex<double>(c[t + k * kSym]) * std::conj(std::complex<double>(c[t + (k - 1) * kSym]));
        acc += d * d;
      }
      phi = std::arg(acc) / 2;
    }
    int64_t pos = t;
    std::complex<double> prev = c[pos];
    float early_late = 0;
    auto next_symbol = [&](std::complex<double>& diff) -> bool {
      pos += kSym;
      if (pos + 1 >= m) return false;
      const float e = std::abs(c[pos - 1]), mid = std::abs(c[pos]), l = std::abs(c[pos + 1]);
      early_late = 0.8f * early_late + 0.2f * (l - e) / (mid + 1e-20f);
      if (early_late > 0.25f && l > mid) { ++pos; early_late = 0; }
      else if (early_late < -0.25f && e > mid) { --pos; early_late = 0; }
      const std::complex<double> cur = c[pos];
      diff = cur * std::conj(prev) * std::polar(1.0, -phi);
      prev = cur;
      return true;
    };
    std::vector<uint8_t> rx;  // received (scrambled) bits, for the descrambler
    auto descramble_push = [&](uint8_t b) {
      rx.push_back(b);
      const std::size_t k = rx.size() - 1;
      const uint8_t d4 = k >= 4 ? rx[k - 4] : 0, d7 = k >= 7 ? rx[k - 7] : 0;
      return static_cast<uint8_t>(b ^ d4 ^ d7);
    };
    auto dbpsk = [&](uint8_t& out) {
      std::complex<double> d;
      if (!next_symbol(d)) return false;
      const uint8_t bit = d.real() < 0;
      const double err = std::arg(d * (bit ? -1.0 : 1.0));
      phi += 0.05 * err;
      out = descramble_push(bit);
      return true;
    };
    auto dqpsk = [&](uint8_t& b0, uint8_t& b1) {
      std::complex<double> d;
      if (!next_symbol(d)) return false;
      int q = static_cast<int>(std::lround(std::arg(d) / (kPi / 2)));
      q = ((q % 4) + 4) % 4;
      static const uint8_t d0s[4] = {0, 0, 1, 1}, d1s[4] = {0, 1, 1, 0};
      const double err = std::arg(d * std::polar(1.0, -q * kPi / 2));
      phi += 0.05 * err;
      b0 = descramble_push(d0s[q]);
      b1 = descramble_push(d1s[q]);
      return true;
    };
    // SFD search: SYNC is at most 128 symbols, so allow 160 after sync.
    uint16_t reg = 0;
    int found = 0;  // 1 = long, 2 = short
    int64_t sfd_end = 0;
    for (int k = 0; k < 170 && !found; ++k) {
      uint8_t b;
      if (!dbpsk(b)) break;
      reg = static_cast<uint16_t>((reg << 1) | b);
      if (k >= 15 + 7 && reg == kLongSfd) found = 1;
      else if (k >= 15 + 7 && reg == kShortSfd) found = 2;
      sfd_end = pos;
    }
    if (!found) { i = t + 64 * kSym; continue; }
    std::vector<uint8_t> hdr;
    bool ok = true;
    while (ok && hdr.size() < 48) {
      if (found == 1) { uint8_t b; ok = dbpsk(b); hdr.push_back(b); }
      else { uint8_t b0, b1; ok = dqpsk(b0, b1); hdr.push_back(b0); hdr.push_back(b1); }
    }
    if (!ok) break;
    const std::vector<uint8_t> prot(hdr.begin(), hdr.begin() + 32), crc_rx(hdr.begin() + 32, hdr.end());
    const bool crc_ok = plcp_crc16_bits(prot) == crc_rx;
    unsigned signal = 0, length_us = 0;
    for (int b = 0; b < 8; ++b) signal |= static_cast<unsigned>(hdr[b]) << b;
    for (int b = 0; b < 16; ++b) length_us |= static_cast<unsigned>(hdr[16 + b]) << b;
    const int64_t preamble_syms = found == 1 ? 144 : 72;
    // sfd_end is the start of the last SFD symbol, preamble symbol (preamble_syms - 1).
    const int64_t est_start = static_cast<int64_t>(std::llround((sfd_end - (preamble_syms - 1) * kSym) * to_input));
    if (!crc_ok) { ++st.dsss_header_bad; i = pos; continue; }
    ++st.dsss_header_ok;
    if (signal != 0x0A && signal != 0x14) { ++st.dsss_unsupported_rate; i = pos; continue; }
    const int rate = signal == 0x0A ? 1 : 2;
    const std::size_t bytes = length_us * rate / 8;
    if (bytes < 14 || bytes > opt.max_psdu_bytes) { ++st.dsss_header_bad; i = pos; continue; }
    if (est_start < lim.accept_from || est_start >= lim.accept_before) {
      i = pos + static_cast<int64_t>(bytes * 8 / rate) * kSym;
      continue;
    }
    std::vector<uint8_t> payload;
    payload.reserve(bytes * 8);
    while (ok && payload.size() < bytes * 8) {
      if (rate == 1) { uint8_t b; ok = dbpsk(b); payload.push_back(b); }
      else { uint8_t b0, b1; ok = dqpsk(b0, b1); payload.push_back(b0); payload.push_back(b1); }
    }
    if (!ok) break;  // truncated by the block edge
    PhyFrame f;
    f.phy = WifiPhy::Dsss;
    f.rate_mbps = rate;
    f.short_preamble = found == 2;
    f.sample_index = est_start;
    f.cfo_hz = phi / (2 * kPi) * 1e6;
    f.quality = best;
    const double rr = std::clamp<double>(best, 0.1, 0.999);
    f.snr_db = static_cast<float>(10 * std::log10(std::max((11 * rr - 1) / (11 * (1 - rr)), 1e-3)));
    f.mpdu.assign(bytes, 0);
    for (std::size_t b = 0; b < bytes * 8; ++b) f.mpdu[b / 8] |= static_cast<uint8_t>(payload[b] << (b % 8));
    f.fcs_ok = fcs_valid(f.mpdu);
    ++(f.fcs_ok ? st.fcs_ok : st.fcs_fail);
    frames.push_back(std::move(f));
    i = pos + kSym;
  }
  return frames;
}

}  // namespace detail
}  // namespace droneid::odid
