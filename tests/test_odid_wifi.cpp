// Open Drone ID Wi-Fi PHY tests on SYNTHETIC IQ: 802.11 OFDM / DSSS fixtures carrying F3411
// Message Packs in Beacon and NAN frames. Covers FCS failures, scrambling (whitening), stream
// chunk/window boundaries, malformed and non-ODID frames, CFO / clock offset, and noise-only input.
#include "droneid/odid_wifi.h"
#include "droneid/test/microtest.h"
#include "wifi_internal.hpp"

#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace droneid;
using namespace droneid::odid;

namespace {

const Mac kMac = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x2A};

std::vector<uint8_t> sample_pack(int k = 0) {
  BasicId id; id.id_type = 1; id.ua_type = 2; id.uas_id = "TEST-UAS-" + std::to_string(k);
  Location loc; loc.status = 2; loc.lat = 47.3977 + 1e-4 * k; loc.lon = 8.5456; loc.alt_geo_m = 500; loc.height_m = 40;
  loc.timestamp_raw = static_cast<uint16_t>(100 + k);
  System sys; sys.operator_lat = 47.39; sys.operator_lon = 8.54;
  OperatorId op; op.id = "OP-TEST";
  return encode_pack({encode(id), encode(loc), encode(sys), encode(op)});
}

void add_at(std::vector<cf32>& buf, std::size_t at, const std::vector<cf32>& sig, float gain = 1.0f) {
  if (buf.size() < at + sig.size()) buf.resize(at + sig.size());
  for (std::size_t i = 0; i < sig.size(); ++i) buf[at + i] += sig[i] * gain;
}

void add_noise(std::vector<cf32>& buf, float snr_db, unsigned seed = 7) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> g(0.0f, std::sqrt(std::pow(10.0f, -snr_db / 10.0f) / 2.0f));
  for (auto& s : buf) s += cf32(g(rng), g(rng));
}

void mix(std::vector<cf32>& buf, double hz, double fs) {
  for (std::size_t n = 0; n < buf.size(); ++n)
    buf[n] *= std::polar(1.0f, static_cast<float>(std::fmod(2 * M_PI * hz / fs * static_cast<double>(n), 2 * M_PI)));
}

std::vector<cf32> frame_iq(const std::vector<cf32>& ppdu, double fs, float snr_db = 25) {
  std::vector<cf32> buf(static_cast<std::size_t>(0.0005 * fs), cf32(0, 0));
  add_at(buf, buf.size(), ppdu);
  buf.resize(buf.size() + static_cast<std::size_t>(0.0005 * fs));
  add_noise(buf, snr_db);
  return buf;
}

bool check_one(const std::vector<OdidDetection>& d, WifiPhy phy, double rate, WifiTransport transport,
               const std::string& id = "TEST-UAS-0") {
  if (d.size() != 1) { std::printf("    got %zu detections\n", d.size()); return false; }
  return d[0].phy.phy == phy && d[0].phy.rate_mbps == rate && d[0].phy.fcs_ok &&
         d[0].frame.transport == transport && d[0].frame.uas.primary_id() == id &&
         d[0].frame.uas.location && std::abs(d[0].frame.uas.location->lat - 47.3977) < 1e-6 &&
         d[0].frame.result.decoded == 4;
}

}  // namespace

TEST(WifiPhy, PlcpCrcMatchesStandardExample) {
  // IEEE 802.11 DSSS PLCP example: 1 Mb/s, SERVICE 0, LENGTH 192 us -> CRC 0101 1011 0101 0111.
  const std::string hdr = "01010000000000000000001100000000", want = "0101101101010111";
  std::vector<uint8_t> bits;
  for (const char c : hdr) bits.push_back(static_cast<uint8_t>(c - '0'));
  std::string got;
  for (const auto b : detail::plcp_crc16_bits(bits)) got += static_cast<char>('0' + b);
  EXPECT_EQ(got, want);
}

TEST(WifiPhy, OfdmScramblerSequence) {
  // IEEE 802.11 OFDM scrambler output from the all-ones state (127-bit period).
  const std::string want =
      "00001110111100101100100100000010001001100010111010110110000011001101010011100111101101000010101011111010"
      "01010001101110001111111";
  std::string got;
  for (const auto b : detail::scrambler_ones()) got += static_cast<char>('0' + b);
  EXPECT_EQ(got, want);
}

TEST(WifiPhy, ConvEncoderImpulseResponse) {
  // A single 1 reproduces the generators: g0 = 133 (1011011), g1 = 171 (1111001), MSB first.
  const auto out = detail::conv_encode({1, 0, 0, 0, 0, 0, 0});
  std::string a, b;
  for (std::size_t i = 0; i < out.size(); i += 2) { a += static_cast<char>('0' + out[i]); b += static_cast<char>('0' + out[i + 1]); }
  EXPECT_EQ(a, std::string("1011011"));
  EXPECT_EQ(b, std::string("1111001"));
}

TEST(WifiPhy, OfdmAllRatesBeaconAndNan) {
  for (const int rate : {6, 9, 12, 18, 24, 36, 48, 54}) {
    for (const auto transport : {WifiTransport::Beacon, WifiTransport::Nan}) {
      const auto mpdu = transport == WifiTransport::Beacon ? build_beacon_mpdu(kMac, "RID", 0, sample_pack())
                                                           : build_nan_action_mpdu(kMac, 0, sample_pack());
      const auto iq = frame_iq(synth_ofdm_ppdu(mpdu, rate), 20e6, 30);
      WifiDecodeOptions opt;
      const auto d = decode_odid_wifi(iq, opt);
      const bool ok = check_one(d, WifiPhy::Ofdm, rate, transport);
      if (!ok) std::printf("    OFDM %d Mb/s %s failed\n", rate, to_string(transport));
      EXPECT_TRUE(ok);
    }
  }
}

TEST(WifiPhy, DsssRatesAndPreambles) {
  for (const int rate : {1, 2}) {
    for (const bool short_preamble : {false, true}) {
      const auto mpdu = build_beacon_mpdu(kMac, "RID", 3, sample_pack());
      const auto iq = frame_iq(synth_dsss_ppdu(mpdu, rate, short_preamble), 20e6, 15);
      WifiDecodeOptions opt;
      const auto d = decode_odid_wifi(iq, opt);
      EXPECT_TRUE(check_one(d, WifiPhy::Dsss, rate, WifiTransport::Beacon));
      if (d.size() == 1) {
        EXPECT_EQ(d[0].phy.short_preamble, short_preamble);
        EXPECT_EQ(d[0].frame.message_counter, 3);
      }
    }
  }
}

TEST(WifiPhy, OtherSampleRatesCfoAndClockOffset) {
  struct Case { double fs; WifiPhy phy; int rate; double cfo; double ppm; };
  for (const auto& c : {Case{23.04e6, WifiPhy::Ofdm, 6, 80e3, 30}, Case{25e6, WifiPhy::Ofdm, 24, -150e3, -40},
                        Case{23.04e6, WifiPhy::Dsss, 1, 100e3, 40}, Case{50e6, WifiPhy::Dsss, 2, -60e3, -25}}) {
    const auto mpdu = build_nan_action_mpdu(kMac, 9, sample_pack());
    const double fs_tx = c.fs * (1 + c.ppm * 1e-6);
    auto iq = frame_iq(c.phy == WifiPhy::Ofdm ? synth_ofdm_ppdu(mpdu, c.rate, fs_tx)
                                              : synth_dsss_ppdu(mpdu, c.rate, false, fs_tx), c.fs, 25);
    mix(iq, c.cfo, c.fs);
    WifiDecodeOptions opt;
    opt.sample_rate = c.fs;
    const auto d = decode_odid_wifi(iq, opt);
    EXPECT_TRUE(check_one(d, c.phy, c.rate, WifiTransport::Nan));
    if (d.size() == 1 && c.phy == WifiPhy::Ofdm) EXPECT_NEAR(d[0].phy.cfo_hz, c.cfo, 2e3);
  }
}

TEST(WifiPhy, FrequencyOffsetOptionRecentresChannel) {
  // Signal sits 3 MHz below the capture centre (e.g. tuned 2440 MHz for channel 6 at 2437 MHz).
  auto iq = frame_iq(synth_ofdm_ppdu(build_beacon_mpdu(kMac, "RID", 0, sample_pack()), 6, 25e6), 25e6, 25);
  mix(iq, -3e6, 25e6);
  WifiDecodeOptions opt;
  opt.sample_rate = 25e6;
  opt.dsss = false;
  EXPECT_EQ(decode_odid_wifi(iq, opt).size(), std::size_t(0));
  opt.freq_offset = 3e6;
  EXPECT_TRUE(check_one(decode_odid_wifi(iq, opt), WifiPhy::Ofdm, 6, WifiTransport::Beacon));
}

TEST(WifiPhy, ScramblerSeedsAreTransparent) {
  const auto mpdu = build_beacon_mpdu(kMac, "RID", 0, sample_pack());
  std::vector<cf32> first;
  for (const uint8_t seed : {0x01, 0x2A, 0x5D, 0x7F}) {
    const auto ppdu = synth_ofdm_ppdu(mpdu, 12, 20e6, seed);
    if (first.empty()) first = ppdu;
    else EXPECT_TRUE(ppdu != first);  // different whitening on the air ...
    WifiDecodeOptions opt;
    const auto d = decode_odid_wifi(frame_iq(ppdu, 20e6, 30), opt);
    EXPECT_TRUE(check_one(d, WifiPhy::Ofdm, 12, WifiTransport::Beacon));  // ... same MPDU decoded
    if (d.size() == 1) EXPECT_TRUE(d[0].phy.mpdu == mpdu);
  }
}

TEST(WifiPhy, FcsFailureIsNotReported) {
  for (const bool ofdm : {true, false}) {
    auto mpdu = build_beacon_mpdu(kMac, "RID", 0, sample_pack());
    mpdu[60] ^= 0x10;  // corrupt after the FCS was computed
    const auto iq = frame_iq(ofdm ? synth_ofdm_ppdu(mpdu, 6) : synth_dsss_ppdu(mpdu, 1, false), 20e6, 30);
    WifiDecodeOptions opt;
    WifiStats st;
    const auto phy = decode_wifi_phy(iq, opt, &st);
    EXPECT_EQ(phy.size(), std::size_t(1));
    if (phy.size() == 1) {
      EXPECT_FALSE(phy[0].fcs_ok);
      EXPECT_TRUE(phy[0].mpdu == mpdu);  // demodulated exactly; the frame itself is bad
    }
    EXPECT_EQ(st.fcs_fail, 1u);
    WifiStats st2;
    EXPECT_EQ(decode_odid_wifi(iq, opt, &st2).size(), std::size_t(0));
    EXPECT_EQ(st2.odid_frames, 0u);
  }
}

TEST(WifiPhy, NonOdidAndMalformedFramesAreCounted) {
  // A plain beacon (no ODID element) and an ODID beacon whose pack header is malformed.
  std::vector<uint8_t> plain = {0x80, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  plain.insert(plain.end(), kMac.begin(), kMac.end());
  plain.insert(plain.end(), kMac.begin(), kMac.end());
  plain.insert(plain.end(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0, 0x01, 0x04, 0, 4, 'h', 'o', 'm', 'e'});
  plain = append_fcs(plain);
  auto bad_pack = sample_pack();
  bad_pack[2] = 12;  // claims 12 messages
  const auto malformed = build_beacon_mpdu(kMac, "RID", 0, bad_pack);
  std::vector<cf32> iq(2000);
  add_at(iq, 1000, synth_ofdm_ppdu(plain, 6));
  add_at(iq, iq.size() + 1000, synth_ofdm_ppdu(malformed, 6));
  iq.resize(iq.size() + 2000);
  add_noise(iq, 30);
  WifiDecodeOptions opt;
  WifiStats st;
  EXPECT_EQ(decode_odid_wifi(iq, opt, &st).size(), std::size_t(0));
  EXPECT_EQ(st.fcs_ok, 2u);
  EXPECT_EQ(st.non_odid_frames, 1u);
  EXPECT_EQ(st.odid_rejected, 1u);
}

TEST(WifiPhy, StreamChunkBoundariesReportEachFrameOnce) {
  const std::size_t window = 1 << 15;
  // Frames deliberately straddle window edges (k * window) and start exactly on one.
  struct Placed { std::size_t at; bool ofdm; };
  const std::vector<Placed> placed = {{window - 150, true},     {2 * window - 1, false}, {3 * window, true},
                                      {4 * window + 40000, true}, {6 * window - 9000, false}};
  std::vector<cf32> iq;
  for (std::size_t k = 0; k < placed.size(); ++k) {
    const auto mpdu = build_beacon_mpdu(kMac, "RID", static_cast<uint8_t>(k), sample_pack(static_cast<int>(k)));
    add_at(iq, placed[k].at, placed[k].ofdm ? synth_ofdm_ppdu(mpdu, 6) : synth_dsss_ppdu(mpdu, 2, true));
  }
  iq.resize(iq.size() + 5000);
  add_noise(iq, 25);
  WifiDecodeOptions opt;
  for (const std::size_t chunk : {iq.size(), std::size_t(4096), std::size_t(777), std::size_t(65536)}) {
    OdidWifiStream stream(opt, window, 0.003);
    std::vector<OdidDetection> got;
    for (std::size_t at = 0; at < iq.size(); at += chunk) {
      const auto n = std::min(chunk, iq.size() - at);
      for (auto& d : stream.feed(iq.data() + at, n)) got.push_back(std::move(d));
    }
    for (auto& d : stream.feed(nullptr, 0, true)) got.push_back(std::move(d));
    EXPECT_EQ(got.size(), placed.size());
    for (std::size_t k = 0; k < std::min(got.size(), placed.size()); ++k) {
      EXPECT_EQ(got[k].frame.message_counter, static_cast<uint8_t>(k));
      EXPECT_EQ(got[k].frame.uas.primary_id(), "TEST-UAS-" + std::to_string(k));
      // Preamble start estimate within 2 samples of where the PPDU was placed (DSSS adds the
      // synthesizer's 32-chip lead-in at 22 MSPS: 29 samples at 20 MSPS).
      const double expect = static_cast<double>(placed[k].at) + (placed[k].ofdm ? 0.0 : 32 * 20.0 / 22.0);
      EXPECT_NEAR(static_cast<double>(got[k].phy.sample_index), expect, 3.0);
      if (std::abs(static_cast<double>(got[k].phy.sample_index) - expect) > 3) std::printf("    k=%zu got %lld want %.1f\n", k, static_cast<long long>(got[k].phy.sample_index), expect);
    }
    EXPECT_EQ(stream.stats().odid_frames, placed.size());
    EXPECT_EQ(stream.samples_consumed(), iq.size());
  }
}

TEST(WifiPhy, BackToBackOfdmFrames) {
  // Two PPDUs 0.8 us apart (shorter than SIFS): the detector must re-arm right after the first
  // frame instead of waiting for the autocorrelation metric to drop.
  std::vector<cf32> iq(1000);
  for (int k = 0; k < 2; ++k) {
    add_at(iq, iq.size(), synth_ofdm_ppdu(build_nan_action_mpdu(kMac, static_cast<uint8_t>(k), sample_pack(k)), 12));
    iq.resize(iq.size() + 16);
  }
  iq.resize(iq.size() + 1000);
  add_noise(iq, 30);
  WifiDecodeOptions opt;
  const auto d = decode_odid_wifi(iq, opt);
  EXPECT_EQ(d.size(), std::size_t(2));
  if (d.size() == 2) EXPECT_EQ(d[1].frame.message_counter, 1);
}

TEST(WifiPhy, TruncatedFrameAtStreamEnd) {
  const auto ppdu = synth_ofdm_ppdu(build_beacon_mpdu(kMac, "RID", 0, sample_pack()), 6);
  std::vector<cf32> iq(1000);
  add_at(iq, 1000, ppdu);
  iq.resize(1000 + ppdu.size() / 2);
  add_noise(iq, 30);
  WifiDecodeOptions opt;
  OdidWifiStream stream(opt);
  EXPECT_EQ(stream.feed(iq.data(), iq.size(), true).size(), std::size_t(0));
  EXPECT_EQ(stream.stats().ofdm_sync, 1u);
}

TEST(WifiPhy, NoiseToneAndSilenceProduceNothing) {
  WifiDecodeOptions opt;
  std::vector<cf32> noise(2'000'000);
  add_noise(noise, 0, 99);
  WifiStats st;
  EXPECT_EQ(decode_wifi_phy(noise, opt, &st).size(), std::size_t(0));
  EXPECT_EQ(st.fcs_ok, 0u);
  // A strong CW tone plus DC must not trigger repeated decode attempts.
  std::vector<cf32> tone(2'000'000, cf32(0.05f, 0));
  for (std::size_t n = 0; n < tone.size(); ++n) tone[n] += std::polar(0.5f, static_cast<float>(0.3 * n));
  add_noise(tone, 30, 5);
  WifiStats tone_stats;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(decode_wifi_phy(tone, opt, &tone_stats).size(), std::size_t(0));
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_TRUE(tone_stats.ofdm_sync <= 2);
  EXPECT_TRUE(secs < 20.0);
  std::vector<cf32> zeros(500000);
  EXPECT_EQ(decode_wifi_phy(zeros, opt).size(), std::size_t(0));
  EXPECT_EQ(decode_wifi_phy({}, opt).size(), std::size_t(0));
}

MICROTEST_MAIN
