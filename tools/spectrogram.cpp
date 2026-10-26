// tools/spectrogram.cpp — STFT waterfall to PNG, for visually finding DroneID bursts.
// Time on X (left=start), frequency on Y (center freq in the middle, fftshifted).
// Each column integrates power over its time slice, so short bursts still register.
// GPL-3.0-or-later.
#include <fftw3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "droneid/file_source.h"
#include "droneid/logging.h"

using namespace droneid;

namespace {

void write_chunk(std::FILE* f, const char* type, const std::uint8_t* data, std::size_t len) {
  std::uint8_t lb[4] = {std::uint8_t(len >> 24), std::uint8_t(len >> 16),
                        std::uint8_t(len >> 8), std::uint8_t(len)};
  std::fwrite(lb, 1, 4, f);
  std::fwrite(type, 1, 4, f);
  if (len) std::fwrite(data, 1, len, f);
  uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
  if (len) crc = crc32(crc, reinterpret_cast<const Bytef*>(data), len);
  std::uint8_t cb[4] = {std::uint8_t(crc >> 24), std::uint8_t(crc >> 16),
                        std::uint8_t(crc >> 8), std::uint8_t(crc)};
  std::fwrite(cb, 1, 4, f);
}

bool write_png(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgb) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  std::fwrite(sig, 1, 8, f);
  std::uint8_t ihdr[13] = {std::uint8_t(w >> 24), std::uint8_t(w >> 16), std::uint8_t(w >> 8),
                           std::uint8_t(w),       std::uint8_t(h >> 24), std::uint8_t(h >> 16),
                           std::uint8_t(h >> 8),  std::uint8_t(h),       8, 2, 0, 0, 0};
  write_chunk(f, "IHDR", ihdr, 13);
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(h) * (w * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);  // filter type 0
    const std::uint8_t* row = &rgb[static_cast<std::size_t>(y) * w * 3];
    raw.insert(raw.end(), row, row + w * 3);
  }
  uLongf clen = compressBound(raw.size());
  std::vector<std::uint8_t> comp(clen);
  compress2(comp.data(), &clen, raw.data(), raw.size(), 6);
  write_chunk(f, "IDAT", comp.data(), clen);
  write_chunk(f, "IEND", nullptr, 0);
  std::fclose(f);
  return true;
}

// Simple "jet"-style colormap for v in [0,1].
void colormap(double v, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
  v = std::clamp(v, 0.0, 1.0);
  auto ch = [](double x) { return static_cast<std::uint8_t>(255.0 * std::clamp(1.5 - std::fabs(x), 0.0, 1.0)); };
  r = ch(4 * v - 3);
  g = ch(4 * v - 2);
  b = ch(4 * v - 1);
}

double percentile(std::vector<float> v, double pct) {
  if (v.empty()) return 0;
  auto k = static_cast<std::size_t>(pct / 100.0 * (v.size() - 1));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

}  // namespace

static void usage(const char* p) {
  std::fprintf(stderr,
      "usage: %s FILE --out PNG [--format cf32|sc16] [--rate HZ] [--fft N]\n"
      "            [--cols W] [--secs S]\n", p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string path = argv[1];
  std::string out;
  SampleFormat fmt = SampleFormat::sc16;
  double rate = sig::kChannelRate, secs = 8.0;
  int nfft = 1024, cols = 1600;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--out") out = next();
    else if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate") rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--fft")  nfft = std::atoi(next().c_str());
    else if (a == "--cols") cols = std::atoi(next().c_str());
    else if (a == "--secs") secs = std::strtod(next().c_str(), nullptr);
    else { usage(argv[0]); return 2; }
  }
  if (out.empty()) { usage(argv[0]); return 2; }

  FileSource src(path, fmt, rate);
  rate = src.sample_rate();
  const auto want = static_cast<std::size_t>(rate * secs);
  std::vector<cf32> iq;
  iq.reserve(want);
  {
    std::vector<cf32> blk(1u << 16);
    int64_t t = 0;
    while (iq.size() < want) {
      std::size_t got = src.recv(blk.data(), blk.size(), t);
      if (got == 0) break;
      iq.insert(iq.end(), blk.begin(), blk.begin() + std::min(got, want - iq.size()));
    }
  }
  if (iq.size() < static_cast<std::size_t>(nfft)) { LOG_ERROR("too few samples"); return 1; }

  const std::size_t N = iq.size();
  const std::size_t col_w = std::max<std::size_t>(nfft, N / cols);
  const int W = static_cast<int>(N / col_w);
  const int H = nfft;

  auto* in = fftwf_alloc_complex(nfft);
  auto* o = fftwf_alloc_complex(nfft);
  fftwf_plan plan = fftwf_plan_dft_1d(nfft, in, o, FFTW_FORWARD, FFTW_ESTIMATE);

  // Column power in dB (row 0 = top). fftshift so DC is centered.
  std::vector<float> db(static_cast<std::size_t>(W) * H, -200.0f);
  std::vector<float> flat;
  flat.reserve(static_cast<std::size_t>(W) * H);
  for (int c = 0; c < W; ++c) {
    const std::size_t base = static_cast<std::size_t>(c) * col_w;
    std::vector<double> acc(nfft, 0.0);
    int nseg = 0;
    for (std::size_t off = 0; off + nfft <= col_w; off += nfft / 2) {
      for (int k = 0; k < nfft; ++k) {
        const cf32& s = iq[base + off + k];
        const double win = 0.5 - 0.5 * std::cos(2 * M_PI * k / (nfft - 1));
        in[k][0] = static_cast<float>(s.real() * win);
        in[k][1] = static_cast<float>(s.imag() * win);
      }
      fftwf_execute(plan);
      for (int k = 0; k < nfft; ++k) acc[k] += o[k][0] * o[k][0] + o[k][1] * o[k][1];
      ++nseg;
    }
    for (int k = 0; k < nfft; ++k) {
      const int shifted = (k + nfft / 2) % nfft;       // DC to center
      const int row = H - 1 - shifted;                  // low row index = top
      const double p = nseg ? acc[k] / nseg : 1e-20;
      const float v = static_cast<float>(10.0 * std::log10(p + 1e-20));
      db[static_cast<std::size_t>(row) * W + c] = v;
      flat.push_back(v);
    }
  }
  fftwf_destroy_plan(plan);
  fftwf_free(in);
  fftwf_free(o);

  const double lo = percentile(flat, 40.0);   // noise floor-ish
  const double hi = percentile(flat, 99.7);   // bright bursts
  const double span = (hi > lo) ? (hi - lo) : 1.0;

  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(W) * H * 3);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const double norm = (db[static_cast<std::size_t>(y) * W + x] - lo) / span;
      std::uint8_t r, g, b;
      colormap(norm, r, g, b);
      const std::size_t idx = (static_cast<std::size_t>(y) * W + x) * 3;
      rgb[idx] = r; rgb[idx + 1] = g; rgb[idx + 2] = b;
    }

  if (!write_png(out, W, H, rgb)) { LOG_ERROR("cannot write %s", out.c_str()); return 1; }
  std::printf("%s : %dx%d  (%.3f s, %.1f ms/col, %.1f kHz/row, dB %.0f..%.0f)\n",
              out.c_str(), W, H, N / rate, col_w / rate * 1e3, rate / nfft / 1e3, lo, hi);
  return 0;
}
