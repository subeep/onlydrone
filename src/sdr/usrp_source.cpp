// droneid/src/sdr/usrp_source.cpp — UHD implementation of UsrpSource.
// Receive-only. Streams fc32 host samples (== cf32), with sc16/sc8 on the wire.
// GPL-3.0-or-later.
#include "droneid/usrp_source.h"

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/property_tree.hpp>
#include <uhd/version.hpp>

#include <atomic>
#include <cmath>
#include <map>
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
  std::map<std::size_t, uhd::tune_result_t> tunes;
};

UsrpSource::UsrpSource(const UsrpParams& params) : impl_(std::make_unique<Impl>()) {
  impl_->p = params;
  auto& p = impl_->p;
  if (p.channels.empty()) p.channels = {p.channel};
  if (p.channels.size() > 2 || (p.channels.size() == 2 && p.channels[0] == p.channels[1]))
    throw std::invalid_argument("choose one or two distinct RX channels");
  if (p.wire_format != "sc8" && p.wire_format != "sc16")
    throw std::invalid_argument("wire format must be sc8 or sc16");
  p.channel = p.channels.front();

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

  const double got_mcr = impl_->usrp->get_master_clock_rate(0);
  if (std::fabs(got_mcr - p.master_clock_rate) > 1.0)
    LOG_WARN("UsrpSource: master clock is %.0f Hz, not the requested %.0f — "
             "sample rate will not be exact", got_mcr, p.master_clock_rate);

  impl_->usrp->set_rx_rate(p.sample_rate, p.channel);
  impl_->actual_rate = impl_->usrp->get_rx_rate(p.channel);
  if (std::fabs(impl_->actual_rate - p.sample_rate) > 1.0)
    LOG_WARN("UsrpSource: requested rate %.0f, got %.0f", p.sample_rate, impl_->actual_rate);

  uhd::tune_request_t tr(p.center_freq);
  impl_->tunes[p.channel] = impl_->usrp->set_rx_freq(tr, p.channel);
  impl_->actual_freq = impl_->usrp->get_rx_freq(p.channel);

  const auto set_bandwidth = [&](std::size_t channel) {
    if (p.bandwidth <= 0) return;
    const double supported = impl_->usrp->get_rx_bandwidth_range(channel).clip(p.bandwidth, true);
    if (std::abs(supported - p.bandwidth) > 1)
      LOG_WARN("UsrpSource: ch%zu bandwidth %.0f Hz outside supported range; using %.0f Hz",
               channel, p.bandwidth, supported);
    impl_->usrp->set_rx_bandwidth(supported, channel);
  };
  impl_->usrp->set_rx_gain(p.gain_db, p.channel);
  set_bandwidth(p.channel);
  if (!p.antenna.empty()) impl_->usrp->set_rx_antenna(p.antenna, p.channel);

  for (std::size_t index = 1; index < p.channels.size(); ++index) {
    const auto channel = p.channels[index];
    impl_->usrp->set_rx_rate(p.sample_rate, channel);
    if (std::abs(impl_->usrp->get_rx_rate(channel) - impl_->actual_rate) > 1)
      throw std::runtime_error("dual RX rates do not match");
    impl_->tunes[channel] = impl_->usrp->set_rx_freq(tr, channel);
    impl_->usrp->set_rx_gain(p.gain_db, channel);
    set_bandwidth(channel);
    if (!p.antenna.empty()) impl_->usrp->set_rx_antenna(p.antenna, channel);
  }
  if (p.clock_source != "internal" && !ref_locked())
    throw std::runtime_error("external reference is not locked");

  // fc32 host samples arrive as std::complex<float> == cf32; wire format sc16/sc8.
  uhd::stream_args_t sa("fc32", p.wire_format);
  sa.channels = p.channels;
  impl_->rx = impl_->usrp->get_rx_stream(sa);

  LOG_INFO("UsrpSource: %s", summary().c_str());
  if (p.auto_start) start();
}

UsrpSource::~UsrpSource() {
  try { stop(); } catch (...) {}
}

void UsrpSource::start() {
  if (impl_->streaming) return;
  if (impl_->p.channels.size() > 1) { start_at(device_time() + 0.1); return; }
  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  cmd.stream_now = true;
  impl_->rx->issue_stream_cmd(cmd);
  impl_->streaming = true;
}

void UsrpSource::stop() {
  if (!impl_ || !impl_->streaming) return;
  impl_->rx->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
  // Flush whatever is in flight.
  std::vector<std::vector<cf32>> drain(impl_->p.channels.size(), std::vector<cf32>(4096));
  std::vector<void*> buffers;
  for (auto& channel : drain) buffers.push_back(channel.data());
  uhd::rx_metadata_t md;
  for (int attempt = 0; attempt < 1024; ++attempt)
    if (impl_->rx->recv(buffers, 4096, md, 0.1) == 0) break;
  impl_->streaming = false;
}

std::size_t UsrpSource::recv(cf32* buf, std::size_t n, int64_t& t_ns) {
  if (impl_->p.channels.size() != 1) throw std::logic_error("dual RX requires recv_channels");
  return recv_channels({buf}, n, t_ns);
}

std::size_t UsrpSource::recv_channels(const std::vector<cf32*>& buffers, std::size_t n, int64_t& t_ns) {
  if (buffers.size() != impl_->p.channels.size()) throw std::invalid_argument("RX buffer count mismatch");
  for (const auto buffer : buffers)
    if (!buffer) throw std::invalid_argument("null RX buffer");
  t_ns = 0;
  if (!impl_->streaming) return 0;
  std::vector<void*> pointers(buffers.begin(), buffers.end());
  const std::size_t got = impl_->rx->recv(pointers, n, impl_->md, 1.0);

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
      throw std::runtime_error("UsrpSource: " + impl_->md.strerror());
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

RxHardwareState UsrpSource::hardware_state() const {
  RxHardwareState state;
  state.uhd_version = uhd::get_version_string();
  state.fpga_version = "unknown";
  const auto tree = impl_->usrp->get_tree();
  if (tree->exists("/mboards/0/fpga_version"))
    state.fpga_version = tree->access<std::string>("/mboards/0/fpga_version").get();
  state.clock_source = impl_->usrp->get_clock_source(0);
  state.time_source = impl_->usrp->get_time_source(0);
  state.master_clock_rate = impl_->usrp->get_master_clock_rate(0);
  state.wire_format = impl_->p.wire_format;
  state.ref_locked = impl_->usrp->get_mboard_sensor("ref_locked", 0).to_bool();
  for (const auto channel : impl_->p.channels) {
    RxChannelState receiver;
    receiver.channel = channel;
    const auto info = impl_->usrp->get_usrp_rx_info(channel);
    receiver.daughterboard_id = info.get("rx_id", "unknown");
    receiver.daughterboard_serial = info.get("rx_serial", "unknown");
    receiver.antenna = impl_->usrp->get_rx_antenna(channel);
    receiver.sample_rate = impl_->usrp->get_rx_rate(channel);
    receiver.center_hz = impl_->usrp->get_rx_freq(channel);
    receiver.gain_db = impl_->usrp->get_rx_gain(channel);
    receiver.bandwidth_hz = impl_->usrp->get_rx_bandwidth(channel);
    receiver.lo_locked = impl_->usrp->get_rx_sensor("lo_locked", channel).to_bool();
    receiver.rf_hz = impl_->tunes.at(channel).actual_rf_freq;
    receiver.dsp_hz = impl_->tunes.at(channel).actual_dsp_freq;
    state.channels.push_back(receiver);
  }
  return state;
}

double UsrpSource::tune(double center_hz) {
  uhd::tune_request_t tr(center_hz);
  for (auto channel : impl_->p.channels)
    impl_->tunes[channel] = impl_->usrp->set_rx_freq(tr, channel);
  impl_->actual_freq = impl_->usrp->get_rx_freq(impl_->p.channel);
  return impl_->actual_freq;
}

double UsrpSource::device_time() const { return impl_->usrp->get_time_now().get_real_secs(); }

void UsrpSource::start_at(double seconds) {
  if (impl_->streaming) throw std::logic_error("stop RX before a timed restart");
  if (!std::isfinite(seconds) || seconds <= device_time() + 0.01)
    throw std::invalid_argument("RX start must be at least 10 ms in the future");
  uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  command.stream_now = false;
  command.time_spec = uhd::time_spec_t(seconds);
  impl_->rx->issue_stream_cmd(command);
  impl_->streaming = true;
}

void UsrpSource::tune_at(double center_hz, double seconds) {
  if (!std::isfinite(center_hz) || center_hz < 1e6 || center_hz > 6e9 ||
      !std::isfinite(seconds) || seconds <= device_time() + 0.01)
    throw std::invalid_argument("invalid center or late timed tune");
  impl_->usrp->set_command_time(uhd::time_spec_t(seconds));
  try {
    for (auto channel : impl_->p.channels)
      impl_->tunes[channel] = impl_->usrp->set_rx_freq(uhd::tune_request_t(center_hz), channel);
  } catch (...) {
    impl_->usrp->clear_command_time();
    throw;
  }
  impl_->usrp->clear_command_time();
  impl_->actual_freq = center_hz;
}

}  // namespace droneid
