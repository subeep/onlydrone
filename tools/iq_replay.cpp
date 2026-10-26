// tools/iq_replay.cpp — stream a raw IQ file through source -> ring buffer -> pipeline.
// Phase 0: the pipeline is a no-op that reports sample count and mean power, proving
// the end-to-end wiring without a radio and without any DSP.
// GPL-3.0-or-later.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "droneid/file_source.h"
#include "droneid/logging.h"
#include "droneid/pipeline.h"
#include "droneid/ring_buffer.h"

using namespace droneid;

static void usage(const char* p) {
  std::fprintf(stderr,
               "usage: %s FILE [--format cf32|sc16|sc8] [--rate HZ] [--center HZ] [--loop]\n"
               "  (a FILE.meta sidecar overrides --format/--rate/--center)\n",
               p);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  const std::string path = argv[1];
  SampleFormat fmt = SampleFormat::cf32;
  double rate = sig::kChannelRate, center = 0.0;
  bool loop = false;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--format") { if (!parse_format(next(), fmt)) { usage(argv[0]); return 2; } }
    else if (a == "--rate")   rate = std::strtod(next().c_str(), nullptr);
    else if (a == "--center") center = std::strtod(next().c_str(), nullptr);
    else if (a == "--loop")   loop = true;
    else { usage(argv[0]); return 2; }
  }

  try {
    FileSource src(path, fmt, rate, center, loop);
    SpscRingBuffer<cf32> ring(1u << 16);
    NoopPipeline pipe;

    std::vector<cf32> in(8192), out(8192);
    int64_t t_ns = 0;
    for (;;) {
      const std::size_t got = src.recv(in.data(), in.size(), t_ns);
      if (got == 0) break;  // EOF (loop handled inside FileSource)
      std::size_t off = 0;
      while (off < got) {
        off += ring.write(in.data() + off, got - off);
        std::size_t r;
        while ((r = ring.read(out.data(), out.size())) > 0) pipe.process(out.data(), r);
      }
      if (loop && pipe.count() > static_cast<std::uint64_t>(rate)) break;  // 1s cap when looping
    }
    // Final drain.
    std::size_t r;
    while ((r = ring.read(out.data(), out.size())) > 0) pipe.process(out.data(), r);

    const double secs = src.sample_rate() > 0 ? pipe.count() / src.sample_rate() : 0.0;
    std::printf("replayed %llu samples (%.4f s @ %.0f Hz), mean power %.2f dBFS\n",
                static_cast<unsigned long long>(pipe.count()), secs, src.sample_rate(),
                pipe.mean_power_dbfs());
    return 0;
  } catch (const std::exception& e) {
    LOG_ERROR("iq_replay: %s", e.what());
    return 1;
  }
}
