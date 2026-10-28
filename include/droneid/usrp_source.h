// droneid/usrp_source.h — IqSource backed by a live USRP (X310 + UBX-160) via UHD.
// pimpl keeps all UHD/Boost headers out of this public header, so code that doesn't
// touch the radio builds without UHD.
// GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "droneid/iq_source.h"

namespace droneid {

struct UsrpParams {
  std::string device_args      = "addr=192.168.10.2";
  double      master_clock_rate = 184.32e6;  // 12 x 15.36 MSPS -> integer DDC decim
  double      sample_rate       = 15.36e6;
  double      center_freq       = 2.4595e9;
  double      gain_db           = 20.0;
  double      bandwidth         = 10e6;       // analog BW; <=0 leaves UHD default
  std::string antenna           = "RX2";
  std::string wire_format       = "sc16";     // over-the-wire: sc16 | sc8 (host side is fc32)
  std::size_t channel           = 0;          // Radio#0 = single antenna
  std::string clock_source      = "internal"; // internal | external | gpsdo
  std::string time_source       = "internal";
  std::vector<std::size_t> channels;
  bool auto_start = true;
};

struct RxChannelState {
  std::size_t channel = 0;
  std::string daughterboard_id;
  std::string daughterboard_serial;
  std::string antenna;
  double sample_rate = 0;
  double center_hz = 0;
  double rf_hz = 0;
  double dsp_hz = 0;
  double gain_db = 0;
  double bandwidth_hz = 0;
  bool lo_locked = false;
};

struct RxHardwareState {
  std::string uhd_version;
  std::string fpga_version;
  std::string clock_source;
  std::string time_source;
  std::string wire_format;
  double master_clock_rate = 0;
  bool ref_locked = false;
  std::vector<RxChannelState> channels;
};

class UsrpSource : public IqSource {
 public:
  explicit UsrpSource(const UsrpParams& p);
  ~UsrpSource() override;

  // Begin continuous streaming. Called by the ctor already; exposed for restart.
  void start();
  void stop();
  void start_at(double device_seconds);
  double device_time() const;
  void tune_at(double center_hz, double device_seconds);
  std::size_t recv_channels(const std::vector<cf32*>& buffers, std::size_t count, int64_t& t_ns);

  // IqSource. Blocks up to ~a few hundred ms; returns 0 only on a hard error/stop.
  std::size_t recv(cf32* buf, std::size_t n, int64_t& t_ns) override;
  double sample_rate() const override;
  double center_freq() const override;

  // Status for the UI / logging.
  bool          ref_locked() const;
  std::uint64_t overflow_count() const;
  std::string   serial() const;
  std::string   summary() const;
  RxHardwareState hardware_state() const;

  // Retune (dwell/hop, Phase 4). Returns the actual center set.
  double tune(double center_hz);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace droneid
