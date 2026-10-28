// src/odid/wifi_ofdm.cpp — IEEE 802.11a/g legacy (non-HT) OFDM PHY, 20 MHz, 6-54 Mb/s.
// Receiver: STS autocorrelation detect + coarse CFO, LTS cross-correlation timing + fine CFO,
// LTS channel estimate, pilot phase/slope tracking, max-log soft demap, soft Viterbi (K=7,
// 133/171 octal), descramble. Transmitter: fixture synthesis only.
// GPL-3.0-or-later.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

#include "wifi_internal.hpp"

namespace droneid::odid {
namespace detail {

const std::array<uint8_t, 127>& scrambler_ones() {
  static const auto v = [] {
    std::array<uint8_t, 127> s{};
    uint8_t state = 0x7F;
    for (auto& b : s) {
      b = ((state >> 6) ^ (state >> 3)) & 1;
      state = static_cast<uint8_t>(((state << 1) | b) & 0x7F);
    }
    return s;
  }();
  return v;
}

std::vector<uint8_t> conv_encode(const std::vector<uint8_t>& bits) {
  std::vector<uint8_t> out;
  out.reserve(bits.size() * 2);
  unsigned state = 0;
  for (const auto b : bits) {
    const unsigned reg = (static_cast<unsigned>(b & 1) << 6) | state;
    out.push_back(static_cast<uint8_t>(__builtin_parity(reg & 0133)));
    out.push_back(static_cast<uint8_t>(__builtin_parity(reg & 0171)));
    state = reg >> 1;
  }
  return out;
}

}  // namespace detail

namespace {

using detail::conv_encode;
using detail::fft64;
using detail::scrambler_ones;
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 20e6;
constexpr int kBackoff = 3;  // FFT window starts this many samples into the cyclic prefix

struct Rate {
  int mbps;
  uint8_t r[4];  // R1..R4 as transmitted
  int nbpsc, ncbps, ndbps;
  int punct;     // 0 = 1/2, 1 = 2/3, 2 = 3/4
};
constexpr Rate kRates[] = {
    {6, {1, 1, 0, 1}, 1, 48, 24, 0},  {9, {1, 1, 1, 1}, 1, 48, 36, 2},
    {12, {0, 1, 0, 1}, 2, 96, 48, 0}, {18, {0, 1, 1, 1}, 2, 96, 72, 2},
    {24, {1, 0, 0, 1}, 4, 192, 96, 0}, {36, {1, 0, 1, 1}, 4, 192, 144, 2},
    {48, {0, 0, 0, 1}, 6, 288, 192, 1}, {54, {0, 0, 1, 1}, 6, 288, 216, 2}};

const Rate* rate_by_mbps(int mbps) {
  for (const auto& r : kRates) if (r.mbps == mbps) return &r;
  return nullptr;
}
const Rate* rate_by_bits(const uint8_t* b) {
  for (const auto& r : kRates)
    if (std::equal(r.r, r.r + 4, b)) return &r;
  return nullptr;
}

const std::array<int, 48>& data_carriers() {
  static const auto v = [] {
    std::array<int, 48> a{};
    int i = 0;
    for (int k = -26; k <= 26; ++k)
      if (k != 0 && std::abs(k) != 7 && std::abs(k) != 21) a[i++] = k;
    return a;
  }();
  return v;
}
constexpr int kPilotK[4] = {-21, -7, 7, 21};
constexpr float kPilotBase[4] = {1, 1, 1, -1};
int bin(int k) { return (k + 64) % 64; }

// Long training symbol L_{-26..26}.
constexpr int8_t kLts[53] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1,
                             1, -1, 1, -1, 1, 1, 1, 1, 0, 1, -1, -1, 1, 1, -1, 1, -1, 1,
                             -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};
float lts_k(int k) { return kLts[k + 26]; }

float pilot_polarity(int n) { return scrambler_ones()[n % 127] ? -1.0f : 1.0f; }

const std::vector<cf32>& lts_time() {
  static const auto v = [] {
    std::array<cf32, 64> f{}, t{};
    for (int k = -26; k <= 26; ++k) f[bin(k)] = cf32(lts_k(k), 0);
    fft64(f.data(), t.data(), true);
    std::vector<cf32> out(64);
    for (int i = 0; i < 64; ++i) out[i] = t[i] / std::sqrt(52.0f);
    return out;
  }();
  return v;
}

int parity(unsigned v) { return __builtin_parity(v); }

// Puncture keep-masks over one period of mother-code output (A0 B0 A1 B1 ...).
const std::vector<uint8_t>& punct_mask(int p) {
  static const std::vector<uint8_t> m[3] = {{1, 1}, {1, 1, 1, 0}, {1, 1, 1, 0, 0, 1}};
  return m[p];
}

std::vector<uint8_t> puncture(const std::vector<uint8_t>& coded, int p) {
  const auto& mask = punct_mask(p);
  std::vector<uint8_t> out;
  for (std::size_t i = 0; i < coded.size(); ++i)
    if (mask[i % mask.size()]) out.push_back(coded[i]);
  return out;
}

std::vector<float> depuncture(const std::vector<float>& llr, int p, std::size_t mother_len) {
  const auto& mask = punct_mask(p);
  std::vector<float> out(mother_len, 0.0f);
  std::size_t j = 0;
  for (std::size_t i = 0; i < mother_len && j < llr.size(); ++i)
    if (mask[i % mask.size()]) out[i] = llr[j++];
  return out;
}

// Soft Viterbi. llr > 0 favours bit 1. Returns nbits decoded bits.
std::vector<uint8_t> viterbi(const std::vector<float>& llr, std::size_t nbits, bool terminated) {
  constexpr float kNeg = -1e30f;
  std::array<float, 64> metric;
  metric.fill(kNeg);
  metric[0] = 0;
  std::vector<std::array<uint8_t, 64>> decision(nbits);
  static const auto outputs = [] {
    std::array<std::array<uint8_t, 2>, 128> o{};
    for (unsigned reg = 0; reg < 128; ++reg)
      o[reg] = {static_cast<uint8_t>(parity(reg & 0133)), static_cast<uint8_t>(parity(reg & 0171))};
    return o;
  }();
  for (std::size_t t = 0; t < nbits; ++t) {
    const float la = llr[2 * t], lb = llr[2 * t + 1];
    std::array<float, 64> next;
    for (unsigned ns = 0; ns < 64; ++ns) {
      const unsigned b = ns >> 5;
      float best = kNeg;
      uint8_t pick = 0;
      for (unsigned lsb = 0; lsb < 2; ++lsb) {
        const unsigned s = ((ns << 1) & 0x3F) | lsb;
        if (metric[s] <= kNeg) continue;
        const unsigned reg = (b << 6) | s;
        const float m = metric[s] + (outputs[reg][0] ? la : -la) + (outputs[reg][1] ? lb : -lb);
        if (m > best) { best = m; pick = static_cast<uint8_t>(lsb); }
      }
      next[ns] = best;
      decision[t][ns] = pick;
    }
    // Renormalize to keep floats bounded on long frames.
    const float top = *std::max_element(next.begin(), next.end());
    for (auto& v : next) v = v <= kNeg ? kNeg : v - top;
    metric = next;
  }
  unsigned ns = terminated ? 0 : static_cast<unsigned>(std::max_element(metric.begin(), metric.end()) - metric.begin());
  std::vector<uint8_t> bits(nbits);
  for (std::size_t t = nbits; t-- > 0;) {
    bits[t] = static_cast<uint8_t>(ns >> 5);
    ns = ((ns << 1) & 0x3F) | decision[t][ns];
  }
  return bits;
}

// --- interleaver ---
std::vector<int> interleave_map(int ncbps, int nbpsc) {
  const int s = std::max(nbpsc / 2, 1);
  std::vector<int> j_of_k(ncbps);
  for (int k = 0; k < ncbps; ++k) {
    const int i = (ncbps / 16) * (k % 16) + k / 16;
    j_of_k[k] = s * (i / s) + (i + ncbps - (16 * i / ncbps)) % s;
  }
  return j_of_k;
}

// --- constellation ---
float norm_factor(int nbpsc) {
  switch (nbpsc) {
    case 1: return 1.0f;
    case 2: return 1.0f / std::sqrt(2.0f);
    case 4: return 1.0f / std::sqrt(10.0f);
    default: return 1.0f / std::sqrt(42.0f);
  }
}

float axis_level(const uint8_t* b, int nb) {
  if (nb == 1) return b[0] ? 1.0f : -1.0f;
  if (nb == 2) {  // 00 -3, 01 -1, 11 1, 10 3
    const float mag = b[1] ? 1.0f : 3.0f;
    return b[0] ? mag : -mag;
  }
  // 000 -7, 001 -5, 011 -3, 010 -1, 110 1, 111 3, 101 5, 100 7
  const float mag = b[1] ? (b[2] ? 3.0f : 1.0f) : (b[2] ? 5.0f : 7.0f);
  return b[0] ? mag : -mag;
}

cf32 map_symbol(const uint8_t* b, int nbpsc) {
  const float n = norm_factor(nbpsc);
  if (nbpsc == 1) return cf32(b[0] ? 1.0f : -1.0f, 0);
  const int half = nbpsc / 2;
  return cf32(axis_level(b, half), axis_level(b + half, half)) * n;
}

// Max-log LLRs for one axis value y (already scaled to the unnormalized grid).
void axis_llr(float y, int nb, float w, float* out) {
  out[0] = w * y;
  if (nb >= 2) out[1] = w * (nb == 2 ? 2.0f - std::abs(y) : 4.0f - std::abs(y));
  if (nb == 3) out[2] = w * (2.0f - std::abs(std::abs(y) - 4.0f));
}

void demap(cf32 y, int nbpsc, float w, float* out) {
  if (nbpsc == 1) { out[0] = w * y.real(); return; }
  const float s = 1.0f / norm_factor(nbpsc);
  const int half = nbpsc / 2;
  axis_llr(y.real() * s, half, w, out);
  axis_llr(y.imag() * s, half, w, out + half);
}

std::array<cf32, 64> ofdm_symbol(const std::vector<cf32>& data48, int polarity_index) {
  std::array<cf32, 64> f{};
  const auto& dc = data_carriers();
  for (int i = 0; i < 48; ++i) f[bin(dc[i])] = data48[i];
  const float p = pilot_polarity(polarity_index);
  for (int i = 0; i < 4; ++i) f[bin(kPilotK[i])] = cf32(kPilotBase[i] * p, 0);
  std::array<cf32, 64> t{};
  fft64(f.data(), t.data(), true);
  return t;
}

void append_symbol(std::vector<cf32>& out, const std::array<cf32, 64>& t) {
  const float scale = 1.0f / std::sqrt(52.0f);
  for (int i = 48; i < 64; ++i) out.push_back(t[i] * scale);
  for (int i = 0; i < 64; ++i) out.push_back(t[i] * scale);
}

std::vector<cf32> modulate_bits(const std::vector<uint8_t>& coded, int nbpsc, int ncbps, int polarity_index) {
  const auto map = interleave_map(ncbps, nbpsc);
  std::vector<uint8_t> inter(ncbps);
  for (int k = 0; k < ncbps; ++k) inter[map[k]] = coded[k];
  std::vector<cf32> d(48);
  for (int i = 0; i < 48; ++i) d[i] = map_symbol(&inter[i * nbpsc], nbpsc);
  std::vector<cf32> out;
  append_symbol(out, ofdm_symbol(d, polarity_index));
  return out;
}

// Equalized frequency-domain symbol with pilot phase + slope correction applied.
struct Equalized {
  std::array<cf32, 48> data;
  std::array<float, 48> weight;
};

Equalized equalize(const cf32* time64, const std::array<cf32, 64>& h, int polarity_index) {
  std::array<cf32, 64> f{};
  fft64(time64, f.data(), false);
  const float p = pilot_polarity(polarity_index);
  // Common phase from the pilot sum, then a least-squares slope across the four pilots.
  cf32 acc(0, 0);
  float res[4];
  for (int i = 0; i < 4; ++i) {
    const int k = kPilotK[i];
    const cf32 y = f[bin(k)] / h[bin(k)] * (kPilotBase[i] * p);
    acc += y;
    res[i] = std::arg(y);
  }
  const float common = std::arg(acc);
  float num = 0, den = 0;
  for (int i = 0; i < 4; ++i) {
    float r = res[i] - common;
    while (r > kPi) r -= 2 * kPi;
    while (r < -kPi) r += 2 * kPi;
    num += kPilotK[i] * r;
    den += kPilotK[i] * kPilotK[i];
  }
  const float slope = num / den;
  Equalized e;
  const auto& dc = data_carriers();
  for (int i = 0; i < 48; ++i) {
    const int k = dc[i];
    const cf32 hk = h[bin(k)];
    e.data[i] = f[bin(k)] / hk * std::polar(1.0f, -(common + slope * k));
    e.weight[i] = std::norm(hk);
  }
  return e;
}

// Descramble in place: the first 7 bits (SERVICE bits 0-6, zero before scrambling) reveal the
// scrambler sequence, which then follows s[n] = s[n-4] ^ s[n-7].
void descramble(std::vector<uint8_t>& bits) {
  if (bits.size() < 7) return;
  std::vector<uint8_t> s(bits.size());
  for (std::size_t n = 0; n < bits.size(); ++n) {
    s[n] = n < 7 ? bits[n] : static_cast<uint8_t>(s[n - 4] ^ s[n - 7]);
    bits[n] ^= s[n];
  }
}

}  // namespace

std::vector<cf32> synth_ofdm_ppdu(const std::vector<uint8_t>& psdu, int rate_mbps, double fs_out,
                                  uint8_t seed) {
  const Rate* r = rate_by_mbps(rate_mbps);
  if (!r || psdu.empty() || psdu.size() > 4095) return {};
  std::vector<cf32> out;
  // Short training: 10 x 16 samples.
  {
    static const int sts_k[12] = {-24, -20, -16, -12, -8, -4, 4, 8, 12, 16, 20, 24};
    static const int sts_s[12] = {1, -1, 1, -1, -1, 1, -1, -1, 1, 1, 1, 1};
    std::array<cf32, 64> f{}, t{};
    const float a = std::sqrt(13.0f / 6.0f);
    for (int i = 0; i < 12; ++i) f[bin(sts_k[i])] = cf32(a * sts_s[i], a * sts_s[i]);
    fft64(f.data(), t.data(), true);
    for (int n = 0; n < 160; ++n) out.push_back(t[n % 64] / std::sqrt(52.0f));
  }
  const auto& lts = lts_time();
  for (int n = 32; n < 64; ++n) out.push_back(lts[n]);
  for (int rep = 0; rep < 2; ++rep) out.insert(out.end(), lts.begin(), lts.end());
  // SIGNAL.
  std::vector<uint8_t> sig(24, 0);
  std::copy(r->r, r->r + 4, sig.begin());
  for (int i = 0; i < 12; ++i) sig[5 + i] = (psdu.size() >> i) & 1;
  int par = 0;
  for (int i = 0; i < 17; ++i) par ^= sig[i];
  sig[17] = static_cast<uint8_t>(par);
  const auto sig_sym = modulate_bits(conv_encode(sig), 1, 48, 0);
  out.insert(out.end(), sig_sym.begin(), sig_sym.end());
  // DATA.
  const std::size_t nbits = 16 + 8 * psdu.size() + 6;
  const std::size_t nsym = (nbits + r->ndbps - 1) / r->ndbps;
  std::vector<uint8_t> bits(nsym * r->ndbps, 0);
  for (std::size_t i = 0; i < psdu.size(); ++i)
    for (int b = 0; b < 8; ++b) bits[16 + 8 * i + b] = (psdu[i] >> b) & 1;
  uint8_t state = seed & 0x7F;
  if (!state) state = 0x5D;
  for (auto& b : bits) {
    const uint8_t s = ((state >> 6) ^ (state >> 3)) & 1;
    state = static_cast<uint8_t>(((state << 1) | s) & 0x7F);
    b ^= s;
  }
  std::fill(bits.begin() + 16 + 8 * psdu.size(), bits.begin() + nbits, 0);
  const auto coded = puncture(conv_encode(bits), r->punct);
  for (std::size_t s = 0; s < nsym; ++s) {
    std::vector<uint8_t> chunk(coded.begin() + s * r->ncbps, coded.begin() + (s + 1) * r->ncbps);
    const auto sym = modulate_bits(chunk, r->nbpsc, r->ncbps, static_cast<int>(s + 1));
    out.insert(out.end(), sym.begin(), sym.end());
  }
  return resample(out, kFs, fs_out);
}

namespace detail {

std::vector<PhyFrame> ofdm_receive(const std::vector<cf32>& x, double input_rate, const WifiDecodeOptions& opt,
                                   const BlockLimits& lim, WifiStats& st) {
  std::vector<PhyFrame> frames;
  const int64_t n = static_cast<int64_t>(x.size());
  if (n < 400) return frames;
  const double to_input = input_rate / kFs;
  // Prefix sums for the lag-16 autocorrelation and the energy of both halves.
  std::vector<std::complex<double>> pc(n - 16 + 1);
  std::vector<double> pe(n + 1);
  pe[0] = 0;
  for (int64_t i = 0; i < n; ++i) pe[i + 1] = pe[i] + std::norm(x[i]);
  pc[0] = 0;
  for (int64_t i = 0; i + 16 < n; ++i)
    pc[i + 1] = pc[i] + std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i + 16]));
  auto metric = [&](int64_t i) {
    const auto p = pc[i + 48] - pc[i];
    const double e1 = pe[i + 48] - pe[i], e2 = pe[i + 64] - pe[i + 16];
    return e1 * e2 > 1e-30 ? std::norm(p) / (e1 * e2) : 0.0;
  };
  const auto& lts = lts_time();
  const double thr = static_cast<double>(opt.ofdm_sync_threshold) * opt.ofdm_sync_threshold;
  const int64_t scan_end = std::min<int64_t>(
      n - 64 - 1, lim.accept_before == INT64_MAX ? n : static_cast<int64_t>(lim.accept_before / to_input) + 64);
  int run = 0;
  bool armed = true;
  for (int64_t i = 0; i < scan_end; ++i) {
    const double m = metric(i);
    if (m < thr) { run = 0; armed = true; continue; }
    if (!armed || ++run < 32) continue;
    const int64_t start = i - 31;
    run = 0;
    armed = false;
    ++st.ofdm_sync;
    // Coarse CFO over up to 112 lag-16 products inside the short training field.
    const int64_t c_end = std::min<int64_t>(start + 112, static_cast<int64_t>(pc.size()) - 1);
    const double w1 = -std::arg(pc[c_end] - pc[start]) / 16.0;
    // LTS timing: maximise |c(t)| + |c(t+64)| around the expected position (start + 192).
    const int64_t lo = start + 100, hi = std::min<int64_t>(start + 300, n - 128 - 80);
    if (hi <= lo) continue;
    std::vector<std::complex<double>> seg(static_cast<std::size_t>(hi + 128 - lo));
    for (std::size_t k = 0; k < seg.size(); ++k)
      seg[k] = std::complex<double>(x[lo + static_cast<int64_t>(k)]) *
               std::polar(1.0, -w1 * static_cast<double>(lo + static_cast<int64_t>(k) - start));
    std::vector<double> corr(static_cast<std::size_t>(hi + 64 - lo + 1));
    for (std::size_t t = 0; t < corr.size(); ++t) {
      std::complex<double> acc = 0;
      for (int k = 0; k < 64; ++k) acc += seg[t + k] * std::conj(std::complex<double>(lts[k]));
      corr[t] = std::abs(acc);
    }
    double best = 0;
    int64_t t1 = -1;
    for (int64_t t = lo; t <= hi; ++t) {
      const double v = corr[t - lo] + corr[t - lo + 64];
      if (v > best) { best = v; t1 = t; }
    }
    if (t1 < 0) continue;
    // Normalized pair correlation: (|c1|+|c2|) / (sqrt(E1*Eref) + sqrt(E2*Eref)), Eref = 64.
    const double e1 = pe[t1 + 64] - pe[t1], e2 = pe[t1 + 128] - pe[t1 + 64];
    const double quality = best / (std::sqrt(e1 * 64) + std::sqrt(e2 * 64) + 1e-30);
    if (quality < opt.ofdm_lts_threshold) continue;
    // Fine CFO from the two LTS copies.
    std::complex<double> acc = 0;
    for (int k = 0; k < 64; ++k)
      acc += std::complex<double>(x[t1 + k]) * std::conj(std::complex<double>(x[t1 + 64 + k]));
    const double w = w1 - std::arg(acc * std::polar(1.0, w1 * 64.0)) / 64.0;
    auto derot = [&](int64_t from, int len, cf32* out) {
      for (int k = 0; k < len; ++k) {
        const auto ph = std::polar(1.0, -w * static_cast<double>(from + k - t1));
        out[k] = cf32(static_cast<float>((std::complex<double>(x[from + k]) * ph).real()),
                      static_cast<float>((std::complex<double>(x[from + k]) * ph).imag()));
      }
    };
    std::array<cf32, 64> l1t, l2t, l1, l2, h;
    derot(t1 - kBackoff, 64, l1t.data());
    derot(t1 + 64 - kBackoff, 64, l2t.data());
    fft64(l1t.data(), l1.data());
    fft64(l2t.data(), l2.data());
    double sig = 0, noise = 0;
    h.fill(cf32(1, 0));
    for (int k = -26; k <= 26; ++k) {
      if (!k) continue;
      const int b = bin(k);
      h[b] = (l1[b] + l2[b]) * 0.5f / lts_k(k);
      sig += std::norm(h[b]);
      noise += std::norm(l1[b] - l2[b]) * 0.5;
    }
    // SIGNAL field.
    std::array<cf32, 64> sym;
    const int64_t sig_at = t1 + 128 + 16 - kBackoff;
    derot(sig_at, 64, sym.data());
    auto eq = equalize(sym.data(), h, 0);
    std::vector<float> llr(48);
    {
      std::vector<float> raw(48);
      for (int i = 0; i < 48; ++i) demap(eq.data[i], 1, eq.weight[i], &raw[i]);
      const auto map = interleave_map(48, 1);
      for (int k = 0; k < 48; ++k) llr[k] = raw[map[k]];
    }
    const auto sigbits = viterbi(llr, 24, true);
    int par = 0;
    for (int b = 0; b < 18; ++b) par ^= sigbits[b];
    const Rate* rate = rate_by_bits(sigbits.data());
    std::size_t length = 0;
    for (int b = 0; b < 12; ++b) length |= static_cast<std::size_t>(sigbits[5 + b]) << b;
    bool tail_zero = true;
    for (int b = 18; b < 24; ++b) tail_zero &= sigbits[b] == 0;
    if (par || !rate || sigbits[4] || !tail_zero || length < 14 || length > opt.max_psdu_bytes) {
      ++st.ofdm_signal_bad;
      continue;
    }
    ++st.ofdm_signal_ok;
    const std::size_t nbits = 16 + 8 * length + 6;
    const std::size_t nsym = (nbits + rate->ndbps - 1) / rate->ndbps;
    const int64_t frame_end = t1 + 128 + 80 + 80 * static_cast<int64_t>(nsym);
    const int64_t est_start = static_cast<int64_t>(std::llround((t1 - 192) * to_input));
    if (est_start < lim.accept_from || est_start >= lim.accept_before) {
      i = std::max(i, frame_end - 1);
      armed = true;  // a following frame may start right after this one (SIFS)
      continue;
    }
    if (frame_end > n) continue;  // truncated by the block edge
    std::vector<float> coded;
    coded.reserve(nsym * rate->ncbps);
    const auto map = interleave_map(rate->ncbps, rate->nbpsc);
    std::vector<float> raw(rate->ncbps), deint(rate->ncbps);
    for (std::size_t s = 0; s < nsym; ++s) {
      derot(t1 + 128 + 80 * static_cast<int64_t>(s + 1) + 16 - kBackoff, 64, sym.data());
      eq = equalize(sym.data(), h, static_cast<int>(s + 1));
      for (int d = 0; d < 48; ++d) demap(eq.data[d], rate->nbpsc, eq.weight[d], &raw[d * rate->nbpsc]);
      for (int k = 0; k < rate->ncbps; ++k) deint[k] = raw[map[k]];
      coded.insert(coded.end(), deint.begin(), deint.end());
    }
    const std::size_t total_bits = nsym * rate->ndbps;
    auto bits = viterbi(depuncture(coded, rate->punct, 2 * total_bits), total_bits, false);
    descramble(bits);
    PhyFrame f;
    f.phy = WifiPhy::Ofdm;
    f.rate_mbps = rate->mbps;
    f.sample_index = est_start;
    f.cfo_hz = w * kFs / (2 * kPi);
    f.quality = static_cast<float>(quality);
    f.snr_db = static_cast<float>(10 * std::log10(std::max(sig - noise / 2, 1e-30) / std::max(noise, 1e-30)));
    f.mpdu.assign(length, 0);
    for (std::size_t b = 0; b < 8 * length; ++b) f.mpdu[b / 8] |= static_cast<uint8_t>(bits[16 + b] << (b % 8));
    f.fcs_ok = fcs_valid(f.mpdu);
    ++(f.fcs_ok ? st.fcs_ok : st.fcs_fail);
    frames.push_back(std::move(f));
    i = std::max(i, frame_end - 1);
    armed = true;
  }
  return frames;
}

}  // namespace detail
}  // namespace droneid::odid
