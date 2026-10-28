#include "receiver_args.h"
#include "droneid/file_source.h"
#include "droneid/o4_detector.h"
#include <algorithm>
#include <cstdio>

int main(int argc, char** argv) {
  try {
    ReceiverArgs args(argc, argv, {"--format", "--rate", "--seconds"}, {"--help"});
    if (args.has("--help")) {
      std::puts("o4_detect FILE [--format sc16|cf32|sc8] [--rate HZ] [--seconds 0]\n"
                "Experimental Air 3S structure evidence in non-overlapping 16 ms windows.\n"
                "o4_candidate is not confirmed O4 identity or a unique-drone count.\n"
                "Partial final windows are not classified; sidecars override rate/format.");
      return 0;
    }
    if (args.files.size() != 1) throw std::invalid_argument("provide one IQ file");
    droneid::SampleFormat format;
    if (!droneid::parse_format(args.get("--format", "sc16"), format))
      throw std::invalid_argument("invalid sample format");
    droneid::FileSource source(args.files[0], format, args.number("--rate", 15.36e6));
    droneid::O4CandidateDetector detector(source.sample_rate());
    const double seconds = args.number("--seconds", 0);
    if (seconds < 0 || seconds > 86400) throw std::invalid_argument("invalid duration");
    const uint64_t limit = seconds > 0 ? static_cast<uint64_t>(seconds * source.sample_rate()) : UINT64_MAX;
    uint64_t received=0, windows=0, matches=0, candidates=0;
    int64_t timestamp=0;
    std::vector<droneid::cf32> block(65536);
    while (received < limit) {
      const auto got = source.recv(block.data(), std::min<uint64_t>(block.size(), limit-received), timestamp);
      if (!got) break;
      for (const auto& e : detector.feed(block.data(), got, received)) {
        std::puts(e.to_jsonl().c_str());
        ++windows;
        matches += e.structure_match;
        candidates += e.candidate;
      }
      received += got;
    }
    std::printf("{\"event\":\"summary\",\"profile\":\"%s\",\"samples\":%llu,"
                "\"windows\":%llu,\"structure_windows\":%llu,\"candidate_windows\":%llu,"
                "\"unclassified_tail_samples\":%llu,\"identified_drones\":null,\"o4_identity_accuracy\":null}\n",
                droneid::kO4Profile, static_cast<unsigned long long>(received), static_cast<unsigned long long>(windows),
                static_cast<unsigned long long>(matches), static_cast<unsigned long long>(candidates),
                static_cast<unsigned long long>(received % detector.window_samples()));
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
