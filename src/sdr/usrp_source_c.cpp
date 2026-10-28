// Receive-only UHD C API adapter. Dynamically loaded to keep replay driver-independent.
// GPL-3.0-or-later. C declarations come from UHD 4.1; see third_party/uhd_c/README.md.
#include "droneid/usrp_source.h"
#define UHD_STATIC_LIB
extern "C" {
#include <uhd/usrp/usrp.h>
#include <uhd/version.h>
}
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// A single list keeps declaration and required-symbol validation in sync.
#define UHD_FUNCTIONS(X) \
 X(uhd_get_last_error) X(uhd_get_version_string) \
 X(uhd_usrp_make) X(uhd_usrp_free) X(uhd_usrp_last_error) \
 X(uhd_rx_streamer_make) X(uhd_rx_streamer_free) X(uhd_rx_streamer_recv) \
 X(uhd_rx_streamer_issue_stream_cmd) X(uhd_rx_streamer_last_error) \
 X(uhd_rx_metadata_make) X(uhd_rx_metadata_free) X(uhd_rx_metadata_error_code) \
 X(uhd_rx_metadata_has_time_spec) X(uhd_rx_metadata_time_spec) X(uhd_rx_metadata_strerror) \
 X(uhd_usrp_get_rx_stream) X(uhd_usrp_set_clock_source) X(uhd_usrp_set_time_source) \
 X(uhd_usrp_get_clock_source) X(uhd_usrp_get_time_source) X(uhd_usrp_get_master_clock_rate) \
 X(uhd_usrp_set_rx_rate) X(uhd_usrp_get_rx_rate) X(uhd_usrp_set_rx_freq) X(uhd_usrp_get_rx_freq) \
 X(uhd_usrp_set_rx_gain) X(uhd_usrp_get_rx_gain) X(uhd_usrp_set_rx_antenna) X(uhd_usrp_get_rx_antenna) \
 X(uhd_usrp_set_rx_bandwidth) X(uhd_usrp_get_rx_bandwidth) X(uhd_usrp_get_rx_bandwidth_range) \
 X(uhd_meta_range_make) X(uhd_meta_range_free) X(uhd_meta_range_clip) \
 X(uhd_usrp_get_rx_info) X(uhd_usrp_rx_info_free) X(uhd_usrp_get_mboard_sensor) X(uhd_usrp_get_rx_sensor) \
 X(uhd_sensor_value_make) X(uhd_sensor_value_free) X(uhd_sensor_value_to_bool) \
 X(uhd_usrp_get_time_now) X(uhd_usrp_set_command_time) X(uhd_usrp_clear_command_time)

namespace droneid {
namespace {
struct Library {
#ifdef _WIN32
  HMODULE handle = nullptr;
#else
  void* handle = nullptr;
#endif
  Library() {
    const char* override_path = std::getenv("DRONEID_UHD_LIBRARY");
#ifdef _WIN32
    std::filesystem::path path;
    if (override_path && *override_path) path = std::filesystem::u8path(override_path);
    else {
      const wchar_t* root = _wgetenv(L"ProgramFiles");
      path = std::filesystem::path(root ? root : L"C:\\Program Files") / L"UHD/bin/uhd.dll";
    }
    handle = LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!handle) throw std::runtime_error("Cannot load 64-bit UHD runtime (Windows error " +
        std::to_string(GetLastError()) + "). Install UHD in C:\\Program Files\\UHD, or set "
        "DRONEID_UHD_LIBRARY to the absolute uhd.dll path. IQ replay needs no UHD.");
#else
    handle = dlopen(override_path && *override_path ? override_path : "libuhd.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(std::string("Cannot load UHD: ") + dlerror());
#endif
  }
  ~Library() {
#ifdef _WIN32
    if (handle) FreeLibrary(handle);
#else
    if (handle) dlclose(handle);
#endif
  }
  template<class T> T symbol(const char* name) {
#ifdef _WIN32
    auto value = GetProcAddress(handle, name);
#else
    auto value = dlsym(handle, name);
#endif
    if (!value) throw std::runtime_error(std::string("UHD runtime lacks C API symbol: ") + name);
    return reinterpret_cast<T>(value);
  }
};
struct Api {
  Library library;
#define DECLARE(name) decltype(&::name) name = library.symbol<decltype(&::name)>(#name);
  UHD_FUNCTIONS(DECLARE)
#undef DECLARE
};
std::string safe(const char* s) { return s ? s : "unknown"; }
}

struct UsrpSource::Impl {
  Api api;
  UsrpParams p;
  uhd_usrp_handle usrp = nullptr;
  uhd_rx_streamer_handle rx = nullptr;
  uhd_rx_metadata_handle md = nullptr;
  std::atomic<std::uint64_t> overflows{0};
  bool streaming = false;
  double rate = 0, freq = 0;
  std::map<size_t, uhd_tune_result_t> tunes;
  ~Impl() {
    if (rx) api.uhd_rx_streamer_free(&rx);
    if (md) api.uhd_rx_metadata_free(&md);
    if (usrp) api.uhd_usrp_free(&usrp);
  }
  void check(uhd_error error, const char* operation) const {
    if (error == UHD_ERROR_NONE) return;
    char detail[2048]{};
    api.uhd_get_last_error(detail, sizeof detail);
    throw std::runtime_error(std::string(operation) + ": " + detail + " (UHD " + std::to_string(error) + ")");
  }
  bool sensor(const char* name, size_t channel, bool motherboard) const {
    uhd_sensor_value_handle sensor = nullptr;
    check(api.uhd_sensor_value_make(&sensor), "sensor allocation");
    bool value = false;
    auto error = motherboard ? api.uhd_usrp_get_mboard_sensor(usrp, name, channel, &sensor)
                             : api.uhd_usrp_get_rx_sensor(usrp, name, channel, &sensor);
    if (error == UHD_ERROR_NONE) error = api.uhd_sensor_value_to_bool(sensor, &value);
    api.uhd_sensor_value_free(&sensor);
    check(error, name);
    return value;
  }
};
#define CALL(name, ...) impl_->check(impl_->api.name(__VA_ARGS__), #name)

UsrpSource::UsrpSource(const UsrpParams& params) : impl_(std::make_unique<Impl>()) {
  impl_->p = params;
  auto& p = impl_->p;
  if (p.channels.empty()) p.channels = {p.channel};
  if (p.channels.size() > 2 || (p.channels.size() == 2 && p.channels[0] == p.channels[1]))
    throw std::invalid_argument("choose one or two distinct RX channels");
  if (p.wire_format != "sc16" && p.wire_format != "sc8") throw std::invalid_argument("wire format must be sc16 or sc8");
  p.channel = p.channels.front();
  char version[256]{};
  CALL(uhd_get_version_string, version, sizeof version);
  if (std::string(version).rfind("4.", 0) != 0) throw std::runtime_error("UHD 4.x runtime required");
  std::string args = p.device_args;
  if (args.find("master_clock_rate") == std::string::npos)
    args += (args.empty() ? "" : ",") + std::string("master_clock_rate=") + std::to_string(p.master_clock_rate);
  CALL(uhd_usrp_make, &impl_->usrp, args.c_str());
  CALL(uhd_usrp_set_clock_source, impl_->usrp, p.clock_source.c_str(), 0);
  CALL(uhd_usrp_set_time_source, impl_->usrp, p.time_source.c_str(), 0);
  for (auto channel : p.channels) {
    CALL(uhd_usrp_set_rx_rate, impl_->usrp, p.sample_rate, channel);
    double rate = 0;
    CALL(uhd_usrp_get_rx_rate, impl_->usrp, channel, &rate);
    if (channel == p.channel) impl_->rate = rate;
    else if (std::abs(rate - impl_->rate) > 1) throw std::runtime_error("dual RX rates do not match");
    CALL(uhd_usrp_set_rx_gain, impl_->usrp, p.gain_db, channel, "");
    if (!p.antenna.empty()) CALL(uhd_usrp_set_rx_antenna, impl_->usrp, p.antenna.c_str(), channel);
    if (p.bandwidth > 0) {
      uhd_meta_range_handle range = nullptr;
      CALL(uhd_meta_range_make, &range);
      double bandwidth = 0;
      auto error = impl_->api.uhd_usrp_get_rx_bandwidth_range(impl_->usrp, channel, range);
      if (error == UHD_ERROR_NONE) error = impl_->api.uhd_meta_range_clip(range, p.bandwidth, true, &bandwidth);
      impl_->api.uhd_meta_range_free(&range);
      impl_->check(error, "bandwidth range");
      CALL(uhd_usrp_set_rx_bandwidth, impl_->usrp, bandwidth, channel);
    }
  }
  tune(p.center_freq);
  if (p.clock_source != "internal" && !ref_locked()) throw std::runtime_error("external reference is not locked");
  CALL(uhd_rx_streamer_make, &impl_->rx);
  CALL(uhd_rx_metadata_make, &impl_->md);
  char cpu[] = "fc32", empty[] = "";
  uhd_stream_args_t stream{cpu, p.wire_format.data(), empty, p.channels.data(), static_cast<int>(p.channels.size())};
  CALL(uhd_usrp_get_rx_stream, impl_->usrp, &stream, impl_->rx);
  if (p.auto_start) start();
}
UsrpSource::~UsrpSource() { try { stop(); } catch (...) {} }
void UsrpSource::start() {
  if (impl_->streaming) return;
  if (impl_->p.channels.size() > 1) { start_at(device_time() + .1); return; }
  uhd_stream_cmd_t cmd{UHD_STREAM_MODE_START_CONTINUOUS, 0, true, 0, 0};
  CALL(uhd_rx_streamer_issue_stream_cmd, impl_->rx, &cmd);
  impl_->streaming = true;
}
void UsrpSource::stop() {
  if (!impl_->streaming) return;
  uhd_stream_cmd_t cmd{UHD_STREAM_MODE_STOP_CONTINUOUS, 0, true, 0, 0};
  CALL(uhd_rx_streamer_issue_stream_cmd, impl_->rx, &cmd);
  impl_->streaming = false;
  std::vector<std::vector<cf32>> samples(impl_->p.channels.size(), std::vector<cf32>(4096));
  std::vector<void*> buffers;
  for (auto& channel : samples) buffers.push_back(channel.data());
  for (int attempt = 0; attempt < 1024; ++attempt) {
    size_t got = 0;
    CALL(uhd_rx_streamer_recv, impl_->rx, buffers.data(), 4096, &impl_->md, .01, false, &got);
    if (!got) break;
  }
}
size_t UsrpSource::recv(cf32* buffer, size_t n, int64_t& timestamp) {
  if (impl_->p.channels.size() != 1) throw std::logic_error("dual RX requires recv_channels");
  return recv_channels({buffer}, n, timestamp);
}
size_t UsrpSource::recv_channels(const std::vector<cf32*>& buffers, size_t n, int64_t& timestamp) {
  if (buffers.size() != impl_->p.channels.size()) throw std::invalid_argument("RX buffer count mismatch");
  for (auto* buffer : buffers) if (!buffer) throw std::invalid_argument("null RX buffer");
  timestamp = 0;
  if (!impl_->streaming || !n) return 0;
  std::vector<void*> pointers(buffers.begin(), buffers.end());
  size_t got = 0;
  CALL(uhd_rx_streamer_recv, impl_->rx, pointers.data(), n, &impl_->md, 1., false, &got);
  uhd_rx_metadata_error_code_t error;
  CALL(uhd_rx_metadata_error_code, impl_->md, &error);
  if (error == UHD_RX_METADATA_ERROR_CODE_TIMEOUT) return 0;
  if (error == UHD_RX_METADATA_ERROR_CODE_OVERFLOW) ++impl_->overflows;
  else if (error != UHD_RX_METADATA_ERROR_CODE_NONE) {
    char detail[1024]{};
    CALL(uhd_rx_metadata_strerror, impl_->md, detail, sizeof detail);
    throw std::runtime_error(std::string("RX: ") + detail);
  }
  bool timed = false;
  CALL(uhd_rx_metadata_has_time_spec, impl_->md, &timed);
  if (timed) {
    int64_t seconds = 0; double fraction = 0;
    CALL(uhd_rx_metadata_time_spec, impl_->md, &seconds, &fraction);
    timestamp = seconds * 1000000000LL + static_cast<int64_t>(std::llround(fraction * 1e9));
  }
  return got;
}
double UsrpSource::sample_rate() const { return impl_->rate; }
double UsrpSource::center_freq() const { return impl_->freq; }
uint64_t UsrpSource::overflow_count() const { return impl_->overflows.load(); }
bool UsrpSource::ref_locked() const {
  try { return impl_->sensor("ref_locked", 0, true); }
  catch (...) { return impl_->p.clock_source == "internal"; }
}
std::string UsrpSource::serial() const {
  uhd_usrp_rx_info_t info{};
  CALL(uhd_usrp_get_rx_info, impl_->usrp, impl_->p.channel, &info);
  const auto result = safe(info.mboard_serial);
  impl_->api.uhd_usrp_rx_info_free(&info);
  return result;
}
std::string UsrpSource::summary() const {
  std::ostringstream out;
  out << "X310 " << serial() << " rate=" << sample_rate()/1e6 << " MSPS center=" << center_freq()/1e6 << " MHz (UHD C API)";
  return out.str();
}
RxHardwareState UsrpSource::hardware_state() const {
  RxHardwareState state;
  char text[512]{};
  CALL(uhd_get_version_string, text, sizeof text); state.uhd_version = text;
  state.fpga_version = "unknown (not exposed by UHD C API)";
  CALL(uhd_usrp_get_clock_source, impl_->usrp, 0, text, sizeof text); state.clock_source = text;
  CALL(uhd_usrp_get_time_source, impl_->usrp, 0, text, sizeof text); state.time_source = text;
  CALL(uhd_usrp_get_master_clock_rate, impl_->usrp, 0, &state.master_clock_rate);
  state.wire_format = impl_->p.wire_format;
  state.ref_locked = impl_->sensor("ref_locked", 0, true);
  for (auto channel : impl_->p.channels) {
    RxChannelState receiver; receiver.channel = channel;
    uhd_usrp_rx_info_t info{};
    CALL(uhd_usrp_get_rx_info, impl_->usrp, channel, &info);
    receiver.daughterboard_id = safe(info.rx_id); receiver.daughterboard_serial = safe(info.rx_serial);
    impl_->api.uhd_usrp_rx_info_free(&info);
    CALL(uhd_usrp_get_rx_antenna, impl_->usrp, channel, text, sizeof text); receiver.antenna = text;
    CALL(uhd_usrp_get_rx_rate, impl_->usrp, channel, &receiver.sample_rate);
    CALL(uhd_usrp_get_rx_freq, impl_->usrp, channel, &receiver.center_hz);
    CALL(uhd_usrp_get_rx_gain, impl_->usrp, channel, "", &receiver.gain_db);
    CALL(uhd_usrp_get_rx_bandwidth, impl_->usrp, channel, &receiver.bandwidth_hz);
    receiver.lo_locked = impl_->sensor("lo_locked", channel, false);
    receiver.rf_hz = impl_->tunes.at(channel).actual_rf_freq;
    receiver.dsp_hz = impl_->tunes.at(channel).actual_dsp_freq;
    state.channels.push_back(receiver);
  }
  return state;
}
double UsrpSource::tune(double center) {
  char empty[] = "";
  uhd_tune_request_t request{center, UHD_TUNE_REQUEST_POLICY_AUTO, 0, UHD_TUNE_REQUEST_POLICY_AUTO, 0, empty};
  for (auto channel : impl_->p.channels)
    CALL(uhd_usrp_set_rx_freq, impl_->usrp, &request, channel, &impl_->tunes[channel]);
  CALL(uhd_usrp_get_rx_freq, impl_->usrp, impl_->p.channel, &impl_->freq);
  return impl_->freq;
}
double UsrpSource::device_time() const {
  int64_t seconds = 0; double fraction = 0;
  CALL(uhd_usrp_get_time_now, impl_->usrp, 0, &seconds, &fraction);
  return static_cast<double>(seconds) + fraction;
}
void UsrpSource::start_at(double seconds) {
  if (impl_->streaming) throw std::logic_error("stop RX before timed restart");
  if (!std::isfinite(seconds) || seconds <= device_time() + .01) throw std::invalid_argument("RX start must be at least 10 ms in the future");
  auto whole = static_cast<int64_t>(std::floor(seconds));
  uhd_stream_cmd_t cmd{UHD_STREAM_MODE_START_CONTINUOUS, 0, false, whole, seconds-whole};
  CALL(uhd_rx_streamer_issue_stream_cmd, impl_->rx, &cmd);
  impl_->streaming = true;
}
void UsrpSource::tune_at(double center, double seconds) {
  if (!std::isfinite(center) || center < 1e6 || center > 6e9 || !std::isfinite(seconds) || seconds <= device_time()+.01)
    throw std::invalid_argument("invalid center or late timed tune");
  auto whole = static_cast<int64_t>(std::floor(seconds));
  CALL(uhd_usrp_set_command_time, impl_->usrp, whole, seconds-whole, 0);
  try { tune(center); }
  catch (...) { impl_->api.uhd_usrp_clear_command_time(impl_->usrp, 0); throw; }
  CALL(uhd_usrp_clear_command_time, impl_->usrp, 0);
}
#undef CALL
}  // namespace droneid
