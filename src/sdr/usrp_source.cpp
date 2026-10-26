// droneid/src/sdr/usrp_source.cpp — UHD implementation of UsrpSource.
// Receive-only. Streams fc32 host samples (== cf32), with sc16/sc8 on the wire.
// GPL-3.0-or-later.
#include "droneid/usrp_source.h"

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/metadata.hpp>

#include <atomic>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include "droneid/logging.h"

namespace droneid {

struct UsrpSource::Impl {
  UsrpParams p;
  uhd::usrp::multi_usrp::sptr usrp;
  uhd::rx_streamer::sptr rx;
  uhd::rx_metadata_t md;
  std::atomic<std::uint64_t> overflows{0};
  bool streaming = false;
  double actual_rate = 0;
  double actual_freq = 0;
};

UsrpSource::UsrpSource(const UsrpParams& params) : impl_(std::make_unique<Impl>()) {
  impl_->p = params;
  auto& p = impl_->p;

  // On X300-series the master clock rate is fixed at init: it MUST come through the
  // device args passed to make(), not a later set_master_clock_rate() (which the device
  // silently ignores, leaving it at 200 MHz and forcing an odd DDC decimation).
  // 184.32 MHz / 12 = 15.36 MSPS exactly, with an even (halfband-friendly) decimation.
  std::string args = p.device_args;
  if (args.find("master_clock_rate") == std::string::npos) {
    std::ostringstream mcr;
    mcr << args << (args.empty() ? "" : ",") << "master_clock_rate=" << std::fixed
        << static_cast<long long>(p.master_clock_rate);
    args = mcr.str();
  }
  LOG_INFO("UsrpSource: opening %s", args.c_str());
  impl_->usrp = uhd::usrp::multi_usrp::make(args);

  // Clock/time source (GPSDO/external enable disciplined capture).
  impl_->usrp->set_clock_source(p.clock_source);
  impl_->usrp->set_time_source(p.time_source);

  const double got_mcr = impl_->usrp->get_master_clock_rate(p.channel);
  if (std::fabs(got_mcr - p.master_clock_rate) > 1.0)
    LOG_WARN("UsrpSource: master clock is %.0f Hz, not the requested %.0f — "
             "sample rate will not be exact", got_mcr, p.master_clock_rate);

  impl_->usrp->set_rx_rate(p.sample_rate, p.channel);
  impl_->actual_rate = impl_->usrp->get_rx_rate(p.channel);
  if (std::fabs(impl_->actual_rate - p.sample_rate) > 1.0)
    LOG_WARN("UsrpSource: requested rate %.0f, got %.0f", p.sample_rate, impl_->actual_rate);

  uhd::tune_request_t tr(p.center_freq);
  impl_->usrp->set_rx_freq(tr, p.channel);
  impl_->actual_freq = impl_->usrp->get_rx_freq(p.channel);

  impl_->usrp->set_rx_gain(p.gain_db, p.channel);
  if (p.bandwidth > 0) impl_->usrp->set_rx_bandwidth(p.bandwidth, p.channel);
  if (!p.antenna.empty()) impl_->usrp->set_rx_antenna(p.antenna, p.channel);

  // fc32 host samples arrive as std::complex<float> == cf32; wire format sc16/sc8.
  uhd::stream_args_t sa("fc32", p.wire_format);
  sa.channels = {p.channel};
  impl_->rx = impl_->usrp->get_rx_stream(sa);

  LOG_INFO("UsrpSource: %s", summary().c_str());
  start();
}

UsrpSource::~UsrpSource() {
  try { stop(); } catch (...) {}
}

void UsrpSource::start() {
  if (impl_->streaming) return;
  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  cmd.stream_now = true;
  impl_->rx->issue_stream_cmd(cmd);
  impl_->streaming = true;
}

void UsrpSource::stop() {
  if (!impl_ || !impl_->streaming) return;
  impl_->rx->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
  // Flush whatever is in flight.
  std::complex<float> drain[4096];
  uhd::rx_metadata_t md;
  while (impl_->rx->recv(drain, 4096, md, 0.1) > 0) {}
  impl_->streaming = false;
}

std::size_t UsrpSource::recv(cf32* buf, std::size_t n, int64_t& t_ns) {
  t_ns = 0;
  const std::size_t got = impl_->rx->recv(buf, n, impl_->md, 1.0);

  switch (impl_->md.error_code) {
    case uhd::rx_metadata_t::ERROR_CODE_NONE:
      break;
    case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
      impl_->overflows.fetch_add(1, std::memory_order_relaxed);
      LOG_WARN("UsrpSource: overflow (#%llu)",
               static_cast<unsigned long long>(impl_->overflows.load()));
      break;
    case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
      LOG_WARN("UsrpSource: recv timeout");
      return 0;
    default:
      LOG_ERROR("UsrpSource: recv error %s", impl_->md.strerror().c_str());
      break;
  }
  if (impl_->md.has_time_spec)
    t_ns = static_cast<int64_t>(impl_->md.time_spec.get_real_secs() * 1e9);
  return got;
}

double UsrpSource::sample_rate() const { return impl_->actual_rate; }
double UsrpSource::center_freq() const { return impl_->actual_freq; }

bool UsrpSource::ref_locked() const {
  try {
    auto names = impl_->usrp->get_mboard_sensor_names(0);
    for (const auto& nm : names)
      if (nm == "ref_locked") return impl_->usrp->get_mboard_sensor("ref_locked", 0).to_bool();
  } catch (...) {}
  return impl_->p.clock_source == "internal";  // internal ref: always "locked"
}

std::uint64_t UsrpSource::overflow_count() const {
  return impl_->overflows.load(std::memory_order_relaxed);
}

std::string UsrpSource::serial() const {
  try {
    auto info = impl_->usrp->get_usrp_rx_info(impl_->p.channel);
    return info.get("mboard_serial", "?");
  } catch (...) { return "?"; }
}

std::string UsrpSource::summary() const {
  std::ostringstream os;
  os << "X310 " << serial() << " ch" << impl_->p.channel
     << " rate=" << impl_->actual_rate / 1e6 << " MSPS"
     << " center=" << impl_->actual_freq / 1e6 << " MHz"
     << " gain=" << impl_->p.gain_db << " dB"
     << " ant=" << impl_->p.antenna
     << " wire=" << impl_->p.wire_format
     << " ref=" << (ref_locked() ? "locked" : "UNLOCKED");
  return os.str();
}

double UsrpSource::tune(double center_hz) {
  uhd::tune_request_t tr(center_hz);
  impl_->usrp->set_rx_freq(tr, impl_->p.channel);
  impl_->actual_freq = impl_->usrp->get_rx_freq(impl_->p.channel);
  return impl_->actual_freq;
}

}  // namespace droneid
