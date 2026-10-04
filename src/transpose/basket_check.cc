// ucache-basket-check: run the integrity checks uCache applies to origin bytes
// over every basket of one tree of a local ROOT file, and print what they say
// as JSON — the codec check of each compressed basket and the comparison of
// each key header with the tree's own record. A development tool: it shows on
// real files that the checks pass on undamaged data (no false alarms) and
// what share of a file they cover.
#include "TreeMeta.h"

#include <cstdio>
#include <fcntl.h>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ucache::transpose;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE.root [tree]\n", argv[0]);
    return 2;
  }
  const std::string tree = argc > 2 ? argv[2] : "Events";
  FileMeta fm = parseFile(argv[1], tree);
  if (!fm.error.empty()) {
    std::fprintf(stderr, "parse failed: %s\n", fm.error.c_str());
    return 1;
  }
  const int fd = ::open(argv[1], O_RDONLY);
  if (fd < 0) {
    std::perror("open");
    return 1;
  }
  uint64_t baskets = 0, passed = 0, failed = 0, notCheckable = 0, raw = 0, tolerated = 0;
  uint64_t bytesPassed = 0, bytesUnchecked = 0, keyBytes = 0, keyFail = 0, keyAnomaly = 0;
  uint64_t unreadable = 0;
  std::map<std::string, uint64_t> reasons;
  std::vector<uint8_t> buf;
  for (const auto& br : fm.branches) {
    for (size_t i = 0; i < br.basketSeek.size() && i < br.basketBytes.size(); ++i) {
      ++baskets;
      const int32_t nb = br.basketBytes[i];
      buf.resize(nb > 0 ? static_cast<size_t>(nb) : 0);
      if (nb <= 0 || ::pread(fd, buf.data(), buf.size(), br.basketSeek[i]) !=
                         static_cast<ssize_t>(buf.size())) {
        ++unreadable;
        continue;
      }
      const auto k = parseKey(buf.data(), buf.size(), 0);
      const auto b = k ? parseBasketHeader(buf.data(), buf.size(), 0, *k) : std::nullopt;
      if (!k || !b || k->keylen > buf.size()) {
        ++unreadable;
        ++reasons["key header unreadable"];
        continue;
      }
      keyBytes += k->keylen;
      const BasketKeyCheck kc = checkBasketKey(*k, *b, br, i, tree);
      if (kc.fail) {
        ++keyFail;
        for (const auto& p : kc.problems)
          ++reasons["key: " + p];
      }
      if (!kc.anomalies.empty()) {
        ++keyAnomaly;
        for (const auto& a : kc.anomalies)
          ++reasons["key anomaly: " + a];
      }
      const size_t payload = buf.size() - k->keylen;
      if (k->objlen == static_cast<int32_t>(payload)) { // stored uncompressed
        ++raw;
        bytesUnchecked += payload;
        continue;
      }
      const DecodedFrames d = decodeFrames(buf.data() + k->keylen, payload,
                                           static_cast<uint64_t>(k->objlen));
      if (d.headerTolerated)
        ++tolerated;
      switch (d.check) {
      case FrameCheck::kPassed:
        ++passed;
        bytesPassed += payload;
        break;
      case FrameCheck::kFailed:
        ++failed;
        ++reasons[std::string("codec: ") + d.why];
        break;
      case FrameCheck::kNotCheckable:
        ++notCheckable;
        bytesUnchecked += payload;
        ++reasons[std::string("not checkable: ") + d.why];
        break;
      }
    }
  }
  ::close(fd);
  std::printf("{\"file\":\"%s\",\"tree\":\"%s\",\"branches\":%zu,\"baskets\":%llu,"
              "\"codec_passed\":%llu,\"codec_failed\":%llu,\"not_checkable\":%llu,"
              "\"stored_raw\":%llu,\"header_tolerated\":%llu,\"unreadable\":%llu,"
              "\"key_fail\":%llu,\"key_anomaly\":%llu,\"bytes_codec_checked\":%llu,"
              "\"bytes_unchecked_payload\":%llu,\"bytes_key_headers\":%llu,\"reasons\":{",
              argv[1], tree.c_str(), fm.branches.size(), (unsigned long long)baskets,
              (unsigned long long)passed, (unsigned long long)failed,
              (unsigned long long)notCheckable, (unsigned long long)raw,
              (unsigned long long)tolerated, (unsigned long long)unreadable,
              (unsigned long long)keyFail, (unsigned long long)keyAnomaly,
              (unsigned long long)bytesPassed, (unsigned long long)bytesUnchecked,
              (unsigned long long)keyBytes);
  bool first = true;
  for (const auto& [r, n] : reasons) {
    std::printf("%s\"%s\":%llu", first ? "" : ",", r.c_str(), (unsigned long long)n);
    first = false;
  }
  std::printf("}}\n");
  return failed || keyFail ? 3 : 0;
}
