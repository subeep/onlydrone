// droneid/odid_wifi.h — Open Drone ID over Wi-Fi (ASTM F3411 Wi-Fi Beacon and Wi-Fi NAN).
//
// Receive chain: IQ -> 802.11 PHY (legacy OFDM 6-54 Mb/s, DSSS 1/2 Mb/s) -> MPDU with FCS ->
// beacon vendor IE (OUI FA:0B:BC, type 0x0D) or NAN Service Discovery frame (service id
// "org.opendroneid.remoteid") -> F3411 Message Pack (droneid/odid.h).
// Not covered: CCK 5.5/11 Mb/s, HT/VHT/HE PPDUs, BLE transports. Synthesis functions exist only
// to generate test fixtures; the application is receive-only.
// GPL-3.0-or-later.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "droneid/odid.h"
#include "droneid/types.h"

namespace droneid::odid {

using Mac = std::array<uint8_t, 6>;
std::string mac_string(const Mac& mac);

// ---------------------------------------------------------------------------------------------
// MAC layer
// ---------------------------------------------------------------------------------------------

enum class WifiTransport { Beacon, Nan };
const char* to_string(WifiTransport t);

struct WifiOdidFrame {
  WifiTransport transport = WifiTransport::Beacon;
  Mac src{}, bssid{};
  std::string ssid;              // beacon only
  uint8_t message_counter = 0;
  std::vector<uint8_t> payload;  // F3411 bytes after the counter (normally a Message Pack)
  UasData uas;
  DecodeResult result;
};

uint32_t crc32_ieee(const uint8_t* data, std::size_t size);
// True if the last four bytes are the little-endian CRC-32 of the rest.
bool fcs_valid(const std::vector<uint8_t>& mpdu_with_fcs);

// Parses an MPDU *without* FCS. Returns true if it carries an F3411 payload (beacon vendor IE or
// NAN SDF). `why` (optional) explains a rejection: not management, no ODID element, ...
bool parse_odid_mpdu(const std::vector<uint8_t>& mpdu, WifiOdidFrame& out, std::string* why = nullptr);

// Fixture builders (return MPDU *with* FCS), mirroring opendroneid-core-c wifi.c layouts.
std::vector<uint8_t> build_beacon_mpdu(const Mac& src, const std::string& ssid, uint8_t counter,
                                       const std::vector<uint8_t>& pack);
std::vector<uint8_t> build_nan_action_mpdu(const Mac& src, uint8_t counter,
                                           const std::vector<uint8_t>& pack);
std::vector<uint8_t> append_fcs(std::vector<uint8_t> mpdu);

// ---------------------------------------------------------------------------------------------
// PHY layer
// ---------------------------------------------------------------------------------------------

enum class WifiPhy { Ofdm, Dsss };
const char* to_string(WifiPhy p);

struct PhyFrame {
  WifiPhy phy = WifiPhy::Ofdm;
  double rate_mbps = 0;
  bool short_preamble = false;     // DSSS only
  int64_t sample_index = 0;        // start of preamble, input-sample units, relative to the block
  double cfo_hz = 0;               // estimated carrier offset (after any --freq-offset shift)
  float quality = 0;               // OFDM: LTS correlation; DSSS: Barker sync metric (0..1)
  float snr_db = 0;                // rough in-band SNR estimate
  bool fcs_ok = false;
  std::vector<uint8_t> mpdu;       // PSDU incl. FCS
};

struct WifiDecodeOptions {
  double sample_rate = 20e6;       // input rate; >= 20 MSPS recommended (OFDM needs 20 MHz)
  double freq_offset = 0;          // Hz added to the input to center the Wi-Fi channel
  bool ofdm = true;
  bool dsss = true;
  float ofdm_sync_threshold = 0.70f;   // STS autocorrelation plateau
  float ofdm_lts_threshold = 0.60f;    // normalized LTS cross-correlation
  float dsss_sync_threshold = 0.45f;   // Barker periodicity metric
  std::size_t max_psdu_bytes = 2400;
};

struct WifiStats {
  uint64_t ofdm_sync = 0, ofdm_signal_ok = 0, ofdm_signal_bad = 0;
  uint64_t dsss_sync = 0, dsss_header_ok = 0, dsss_header_bad = 0, dsss_unsupported_rate = 0;
  uint64_t fcs_ok = 0, fcs_fail = 0;
  uint64_t odid_frames = 0, non_odid_frames = 0, odid_rejected = 0;
  void add(const WifiStats& o);
};

// Detects and demodulates every 802.11 PPDU in `iq` (one block). Only FCS-valid frames have
// fcs_ok set; frames failing FCS are still returned (for diagnostics) with fcs_ok = false.
std::vector<PhyFrame> decode_wifi_phy(const std::vector<cf32>& iq, const WifiDecodeOptions& opt,
                                      WifiStats* stats = nullptr);

struct OdidDetection {
  PhyFrame phy;
  WifiOdidFrame frame;
};

// PHY + MAC + F3411 for one block.
std::vector<OdidDetection> decode_odid_wifi(const std::vector<cf32>& iq, const WifiDecodeOptions& opt,
                                            WifiStats* stats = nullptr);

// Streaming wrapper: feeds arbitrary chunks, decodes overlapping windows and reports each frame
// once, with sample_index made absolute (samples since the first feed). A frame must fit in the
// overlap to be decoded across a window edge; the default 16 ms covers a 2000-byte 1 Mb/s PSDU.
class OdidWifiStream {
 public:
  explicit OdidWifiStream(const WifiDecodeOptions& opt, std::size_t window = std::size_t(1) << 21,
                          double overlap_seconds = 0.016);
  std::vector<OdidDetection> feed(const cf32* samples, std::size_t count, bool final = false);
  const WifiStats& stats() const { return stats_; }
  uint64_t samples_consumed() const { return base_ + buffer_.size(); }

 private:
  WifiDecodeOptions opt_;
  std::size_t window_, overlap_;
  uint64_t base_ = 0;
  std::vector<cf32> buffer_;
  WifiStats stats_;
  std::vector<std::pair<std::vector<uint8_t>, int64_t>> recent_;  // duplicate guard
};

// ---------------------------------------------------------------------------------------------
// DSP helpers shared by the PHY (exposed for tests and the fixture generator)
// ---------------------------------------------------------------------------------------------

// Band-limited arbitrary-ratio resampler (Kaiser-windowed sinc). Output sample m sits at input
// position m * fs_in / fs_out. `shift_hz` mixes the input up by that frequency first.
std::vector<cf32> resample(const std::vector<cf32>& in, double fs_in, double fs_out, double shift_hz = 0);

// Fixture synthesis (unit-amplitude baseband PPDU at `fs_out`).
// OFDM: rate in {6,9,12,18,24,36,48,54}. DSSS: rate in {1,2}.
std::vector<cf32> synth_ofdm_ppdu(const std::vector<uint8_t>& psdu, int rate_mbps,
                                  double fs_out = 20e6, uint8_t scrambler_seed = 0x5D);
std::vector<cf32> synth_dsss_ppdu(const std::vector<uint8_t>& psdu, int rate_mbps, bool short_preamble,
                                  double fs_out = 20e6);

}  // namespace droneid::odid
