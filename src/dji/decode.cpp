// src/dji/decode.cpp — DJI DroneID burst detection + demodulation + decode.
// Ported from proto17/dji_droneid matlab/updated_scripts (process_file.m, find_sto_cp.m,
// extract_ofdm_symbol_samples.m, calculate_channel.m, quantize_qpsk.m, extract_bursts_from_file.m).
// GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "droneid/dji.h"
#include "droneid/file_source.h"
#include "droneid/logging.h"
#include "dsp.hpp"

namespace droneid::dji {
namespace {

using dsp::cf32;

std::vector<int> cp_schedule(double fs) {
  const int L = long_cp_len(fs), S = short_cp_len(fs);
  return {L, S, S, S, S, S, S, S, L};
}

// find_sto_cp.m: best burst start via cyclic-prefix autocorrelation (symbols 2..9).
int find_sto_cp(const std::vector<cf32>& b, double fs) {
  const int N = fft_size(fs);
  const auto cps = cp_schedule(fs);
  int full = N * kNumSymbols;
  for (int c : cps) full += c;
  const int num_tests = static_cast<int>(b.size()) - full;
  if (num_tests <= 0) return 0;

  int best_idx = 0;
  double best = -1;
  for (int off = 0; off < num_tests; ++off) {
    int pos = off;
    double acc = 0;
    for (int s = 0; s < kNumSymbols; ++s) {
      const int cp = cps[s];
      if (s >= 1) {  // skip symbol 1 (not always present)
        cf32 dot(0, 0);
        const int tail = pos + cp + N - cp;  // == pos + N
        for (int k = 0; k < cp; ++k) dot += std::conj(b[pos + k]) * b[tail + k];
        acc += std::abs(dot);
      }
      pos += cp + N;
    }
    if (acc > best) { best = acc; best_idx = off; }
  }
  return best_idx;
}

// extract_ofdm_symbol_samples.m: freq-domain (fftshift(fft)) of each symbol, CP removed.
std::vector<std::vector<cf32>> extract_symbols(const std::vector<cf32>& b, double fs, int start) {
  const int N = fft_size(fs);
  const auto cps = cp_schedule(fs);
  std::vector<std::vector<cf32>> freq(kNumSymbols);
  int off = start;
  for (int s = 0; s < kNumSymbols; ++s) {
    off += cps[s];  // skip CP
    std::vector<cf32> sym(b.begin() + off, b.begin() + off + N);
    freq[s] = dsp::fftshift(dsp::fft(sym));
    off += N;
  }
  return freq;
}

// calculate_channel.m: taps = fftshift(fft(create_zc)) ./ received_zc, at the data carriers.
std::vector<cf32> channel_taps(const std::vector<cf32>& rx_zc_freq, double fs, int sym) {
  const int N = fft_size(fs);
  const auto gold = dsp::fftshift(dsp::fft(create_zc(N, sym)));
  const auto carriers = data_carrier_indices(fs);
  std::vector<cf32> taps(carriers.size());
  for (size_t i = 0; i < carriers.size(); ++i) {
    const cf32 r = rx_zc_freq[carriers[i]];
    taps[i] = (std::abs(r) > 1e-12f) ? gold[carriers[i]] / r : cf32(0, 0);
  }
  return taps;
}

// quantize_qpsk.m hard decision -> 2 bits per carrier.
void quantize_qpsk(const std::vector<cf32>& c, std::vector<uint8_t>& out) {
  out.resize(c.size() * 2);
  for (size_t i = 0; i < c.size(); ++i) {
    const float re = c[i].real(), im = c[i].imag();
    uint8_t b0, b1;
    if (re > 0 && im > 0)      { b0 = 0; b1 = 0; }
    else if (re > 0 && im < 0) { b0 = 0; b1 = 1; }
    else if (re < 0 && im > 0) { b0 = 1; b1 = 0; }
    else if (re < 0 && im < 0) { b0 = 1; b1 = 1; }
    else                       { b0 = 0; b1 = 0; }
    out[2 * i] = b0;
    out[2 * i + 1] = b1;
  }
}

bool align_zc_pair(const std::vector<cf32>& samples, double sample_rate,
                   double coarse_cfo, int& start, double& cfo_hz) {
  const int transform_size = fft_size(sample_rate);
  const int long_prefix = long_cp_len(sample_rate);
  const int short_prefix = short_cp_len(sample_rate);
  const int frame_size = 9 * transform_size + 2 * long_prefix + 7 * short_prefix;
  const int max_start = static_cast<int>(samples.size()) - frame_size;
  if (max_start < 0) return false;

  const int first_offset = 3 * transform_size + long_prefix + 3 * short_prefix;
  const int second_offset = first_offset + 2 * (transform_size + short_prefix);
  const int search_size = transform_size + max_start;
  const std::vector<cf32> first(samples.begin() + first_offset,
                                samples.begin() + first_offset + search_size);
  const std::vector<cf32> second(samples.begin() + second_offset,
                                 samples.begin() + second_offset + search_size);
  const auto first_reference = create_zc(transform_size, 4);
  const auto second_reference = create_zc(transform_size, 6);
  float best_score = 0;
  for (int carrier_shift = -16; carrier_shift <= 16; ++carrier_shift) {
    const double candidate_cfo = coarse_cfo + carrier_shift * 15e3;
    auto first_shifted = first;
    auto second_shifted = second;
    dsp::freq_shift(first_shifted, -candidate_cfo, sample_rate);
    dsp::freq_shift(second_shifted, -candidate_cfo, sample_rate);
    const auto first_scores = dsp::normalized_corr(first_shifted, first_reference);
    const auto second_scores = dsp::normalized_corr(second_shifted, second_reference);
    for (int candidate_start = 0; candidate_start <= max_start; ++candidate_start) {
      const float score = first_scores[candidate_start] * second_scores[candidate_start];
      if (score > best_score) {
        best_score = score;
        start = candidate_start;
        cfo_hz = candidate_cfo;
      }
    }
  }
  return best_score > 0;
}

// Demodulate one extracted burst window -> CRC check + payload.
bool decode_burst_window(const std::vector<cf32>& win_in, double fs, DecodedBurst& burst,
                         bool low_pass) {
  const int N = fft_size(fs);
  std::vector<cf32> b = win_in;

  if (low_pass) b = dsp::fir_lowpass(b, 10e6 / 2.0 * (fs / 15.36e6), fs, 50);

  int start = find_sto_cp(b, fs);

  // Coarse CFO from symbol 4's short CP (process_file.m).
  const int L = long_cp_len(fs), S = short_cp_len(fs);
  const int zc_start = L + 3 * N + 3 * S;  // start of symbol-4 data
  if (start + zc_start + N > static_cast<int>(b.size())) return false;
  cf32 dot(0, 0);
  for (int k = 0; k < S; ++k)
    dot += std::conj(b[start + zc_start - S + k]) * b[start + zc_start - S + N + k];
  const double coarse_cfo = std::arg(dot) * fs / (2 * M_PI * N);
  if (!align_zc_pair(b, fs, coarse_cfo, start, burst.cfo_hz)) return false;
  std::vector<cf32> burst_s(b.begin() + start, b.end());
  dsp::freq_shift(burst_s, -burst.cfo_hz, fs);

  const auto freq = extract_symbols(burst_s, fs, 0);
  const auto carriers = data_carrier_indices(fs);
  const auto chan = channel_taps(freq[3], fs, 4);  // symbol 4 (0-based 3)

  // Demod all 9 symbols to bits (equalized).
  std::vector<std::vector<uint8_t>> bits(kNumSymbols);
  for (int s = 0; s < kNumSymbols; ++s) {
    std::vector<cf32> dc(carriers.size());
    for (size_t i = 0; i < carriers.size(); ++i) dc[i] = freq[s][carriers[i]] * chan[i];
    quantize_qpsk(dc, bits[s]);
  }

  // Descramble: symbols [2,3,5,7,8,9] (1-based) = 0-based {1,2,4,6,7,8}, row-major, XOR gold.
  const int kSel[6] = {1, 2, 4, 6, 7, 8};
  std::vector<uint8_t> payload_bits;
  payload_bits.reserve(7200);
  for (int si : kSel)
    payload_bits.insert(payload_bits.end(), bits[si].begin(), bits[si].end());
  const auto scr = generate_scrambler_seq(7200, scrambler_x2_init());
  for (size_t i = 0; i < payload_bits.size(); ++i) payload_bits[i] ^= scr[i];

  std::vector<uint8_t> out176;
  burst.crc_ok = turbo_decode_frame(payload_bits, out176);
  if (burst.crc_ok) burst.payload = out176;
  return burst.crc_ok;
}

}  // namespace

// Detect + decode within one buffer. Peaks whose burst start is < accept_limit are kept
// (accept_limit < 0 means keep all), so chunked callers avoid double-counting the overlap.
// index_base is the global sample index of iq[0]: sets the freq-shift phase and reported index.
static std::vector<DecodedBurst> detect_decode_buffer(const std::vector<cf32>& iq_in,
                                                      const DecodeOptions& opt,
                                                      int64_t index_base, long accept_limit) {
  std::vector<DecodedBurst> results;
  const double fs = opt.sample_rate;
  const int N = fft_size(fs), L = long_cp_len(fs), S = short_cp_len(fs);

  std::vector<cf32> iq = iq_in;
  if (opt.freq_offset != 0.0)
    dsp::freq_shift(iq, opt.freq_offset, fs, static_cast<double>(index_base));

  const auto zc = create_zc(N, 4);
  const auto score = dsp::normalized_corr(iq, zc);

  const int zc_seq_offset = 3 * N + L + 3 * S;
  const int padding = 64;
  const int burst_len = 2 * padding + 2 * L + 7 * S + 9 * N;
  const int win = 10;
  int last_taken = -1000000;

  for (int i = 0; i < static_cast<int>(score.size()); ++i) {
    if (score[i] < opt.corr_threshold) continue;
    int lo = std::max(0, i - win / 2), hi = std::min<int>(score.size() - 1, i + win / 2);
    bool is_peak = true;
    for (int j = lo; j <= hi; ++j) if (score[j] > score[i]) { is_peak = false; break; }
    if (!is_peak || i - last_taken < win) continue;
    last_taken = i;

    const long start = static_cast<long>(i) - zc_seq_offset - padding;
    if (start < 0 || start + burst_len > static_cast<long>(iq.size())) continue;
    if (accept_limit >= 0 && start >= accept_limit) continue;
    std::vector<cf32> window(iq.begin() + start, iq.begin() + start + burst_len);

    DecodedBurst burst;
    burst.sample_index = index_base + start;
    burst.corr = score[i];
    decode_burst_window(window, fs, burst, opt.low_pass);
    results.push_back(std::move(burst));
  }
  return results;
}

std::vector<DecodedBurst> decode_samples(const std::vector<cf32>& iq_in, const DecodeOptions& opt) {
  return detect_decode_buffer(iq_in, opt, 0, -1);
}

std::vector<OffsetScore> scan_freq_offsets(const std::vector<cf32>& iq, double fs,
                                           double lo, double hi, double step) {
  const int N = fft_size(fs);
  const auto zc = create_zc(N, 4);
  std::vector<OffsetScore> out;
  for (double f = lo; f <= hi + 1e-6; f += step) {
    std::vector<cf32> x = iq;
    if (f != 0.0) dsp::freq_shift(x, f, fs);
    const auto sc = dsp::normalized_corr(x, zc);
    float mx = 0; int64_t at = 0;
    for (int i = 0; i < static_cast<int>(sc.size()); ++i)
      if (sc[i] > mx) { mx = sc[i]; at = i; }
    out.push_back({f, mx, at});
  }
  return out;
}

std::vector<cf32> synthesize_burst(const std::vector<uint8_t>& scr, double fs) {
  const int N = fft_size(fs);
  const auto cps = cp_schedule(fs);
  const auto carriers = data_carrier_indices(fs);

  std::vector<std::vector<cf32>> sym(kNumSymbols);
  // Symbol 1 (idx0): all zeros; symbols 4,6 (idx3,5): ZC; rest: QPSK data.
  { std::vector<cf32> f(N, cf32(0, 0)); sym[0] = dsp::ifft(dsp::fftshift(f)); }
  sym[3] = create_zc(N, 4);
  sym[5] = create_zc(N, 6);

  const int kSel[6] = {1, 2, 4, 6, 7, 8};
  for (int c = 0; c < 6; ++c) {
    const uint8_t* chunk = &scr[static_cast<size_t>(c) * 1200];
    std::vector<cf32> f(N, cf32(0, 0));
    for (int j = 0; j < kDataCarriers; ++j) {
      const float re = chunk[2 * j] ? -1.0f : 1.0f;
      const float im = chunk[2 * j + 1] ? -1.0f : 1.0f;
      f[carriers[j]] = cf32(re, im);
    }
    sym[kSel[c]] = dsp::ifft(dsp::fftshift(f));
  }

  std::vector<cf32> burst;
  for (int s = 0; s < kNumSymbols; ++s) {
    const int cp = cps[s];
    for (int k = N - cp; k < N; ++k) burst.push_back(sym[s][k]);   // cyclic prefix
    for (int k = 0; k < N; ++k) burst.push_back(sym[s][k]);
  }
  return burst;
}

std::vector<DecodedBurst> decode_file(const std::string& path, SampleFormat fmt,
                                      const DecodeOptions& opt) {
  FileSource src(path, fmt, opt.sample_rate);
  DecodeOptions o = opt;
  o.sample_rate = src.sample_rate();
  const double fs = o.sample_rate;
  const int N = fft_size(fs), L = long_cp_len(fs), S = short_cp_len(fs);

  const int zc_seq_offset = 3 * N + L + 3 * S;
  const int burst_len = 2 * 64 + 2 * L + 7 * S + 9 * N;
  const size_t overlap = static_cast<size_t>(zc_seq_offset + burst_len + 256);
  const size_t chunk = 4u << 20;  // ~4M samples/chunk -> bounded memory on long captures
  const size_t cap = opt.max_samples ? opt.max_samples : static_cast<size_t>(-1);

  std::vector<DecodedBurst> results;
  std::vector<cf32> window, blk(1u << 16);
  int64_t window_start = 0, t = 0;
  size_t total = 0;
  bool eof = false;

  while (!eof) {
    while (window.size() < chunk + overlap && total < cap) {
      const size_t request = std::min({blk.size(), chunk + overlap - window.size(), cap - total});
      size_t got = src.recv(blk.data(), request, t);
      if (got == 0) { eof = true; break; }
      window.insert(window.end(), blk.begin(), blk.begin() + got);
      total += got;
    }
    if (window.size() < static_cast<size_t>(burst_len)) break;

    const bool final_window = eof || total >= cap;
    const long accept = final_window ? -1 : static_cast<long>(chunk);
    auto part = detect_decode_buffer(window, o, window_start, accept);
    results.insert(results.end(), part.begin(), part.end());

    if (final_window) break;
    window_start += static_cast<int64_t>(chunk);
    window.erase(window.begin(), window.begin() + chunk);
  }
  return results;
}

}  // namespace droneid::dji
