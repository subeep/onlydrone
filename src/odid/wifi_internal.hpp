// src/odid/wifi_internal.hpp — internals shared by the ODID Wi-Fi PHY sources.
// GPL-3.0-or-later.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "droneid/odid_wifi.h"

namespace droneid::odid::detail {

// 64-point FFT (forward: e^{-j}, inverse: e^{+j}, both unscaled).
void fft64(const cf32* in, cf32* out, bool inverse = false);

// Frames found in one block, reported only when their estimated preamble start (input samples)
// lies in [accept_from, accept_before).
struct BlockLimits {
  int64_t accept_from = std::numeric_limits<int64_t>::min();
  int64_t accept_before = std::numeric_limits<int64_t>::max();
};

std::vector<PhyFrame> ofdm_receive(const std::vector<cf32>& x20, double input_rate,
                                   const WifiDecodeOptions& opt, const BlockLimits& lim, WifiStats& st);
std::vector<PhyFrame> dsss_receive(const std::vector<cf32>& x22, double input_rate,
                                   const WifiDecodeOptions& opt, const BlockLimits& lim, WifiStats& st);

std::vector<PhyFrame> decode_block(const std::vector<cf32>& iq, const WifiDecodeOptions& opt,
                                   const BlockLimits& lim, WifiStats& st);

// 802.11a/g scrambler output from the all-ones state (127-bit period; also pilot polarity).
const std::array<uint8_t, 127>& scrambler_ones();
// 802.11a/g rate-1/2 K=7 convolutional encoder (g0 = 133, g1 = 171 octal), output A0 B0 A1 B1 ...
std::vector<uint8_t> conv_encode(const std::vector<uint8_t>& bits);

// 802.11b PLCP header CRC-16 (CCITT, preset ones, ones-complemented), bits in transmit order.
// Returns the 16 CRC bits in transmit order.
std::vector<uint8_t> plcp_crc16_bits(const std::vector<uint8_t>& bits32);

}  // namespace droneid::odid::detail
