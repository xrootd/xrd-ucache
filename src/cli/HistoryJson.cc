#include "HistoryJson.h"

#include "Publish.h" // dateOnlyUtc, registrableDomain, jsonEscape

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>

namespace ucache {

const char* runKind(const Run& r) {
  if (r.disabled)
    return "baseline"; // cache out of the loop — the measured reference
  if (r.originBytes && r.cacheBytes() == 0)
    return "fill";
  if (r.originBytes == 0 && r.cacheBytes())
    return "warm";
  if (r.originBytes && r.cacheBytes())
    return "mixed";
  return "idle";
}

namespace {

void putf(std::string& s, const char* f, ...) __attribute__((format(printf, 2, 3)));
void putf(std::string& s, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  char buf[4096];
  const int n = std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (static_cast<size_t>(n) < sizeof buf) {
    s.append(buf, static_cast<size_t>(n));
    return;
  }
  std::string big(static_cast<size_t>(n) + 1, '\0');
  va_start(ap, f);
  std::vsnprintf(big.data(), big.size(), f, ap);
  va_end(ap);
  big.resize(static_cast<size_t>(n));
  s += big;
}

void putArray(std::string& s, const std::vector<uint64_t>& v) {
  s += '[';
  for (size_t i = 0; i < v.size(); ++i)
    putf(s, i ? ",%llu" : "%llu", (unsigned long long)v[i]);
  s += ']';
}

void addInto(std::vector<uint64_t>& acc, const std::vector<uint64_t>& h) {
  if (h.size() > acc.size())
    acc.resize(h.size(), 0);
  for (size_t i = 0; i < h.size(); ++i)
    acc[i] += h[i];
}

// The raw measurements the per-file and counter records carry for the report
// service to interpret: nothing here is judged, only summed. Each key is left
// out when no record of the run carried what it is made from, so a record
// written before a field existed reads as unknown rather than as zero.
void putRawFields(std::string& s, const Run& r, bool redacted) {
  if (r.haveDurationMs())
    putf(s, ",\"duration_ms\":%llu", (unsigned long long)r.durationMs());
  // Each sum says how many files it is over: a file whose counts are unknown
  // is left out of it, so the three can cover different sets of files.
  uint64_t origRead = 0, origFiles = 0, unique = 0, uniqueFiles = 0, filesBytes = 0;
  for (const auto& [k, f] : r.files) {
    (void)k;
    filesBytes += f.originSize;
    if (f.haveOrigBytes) {
      origRead += f.origBytes;
      ++origFiles;
    }
    if (f.haveUniqueBytes) {
      unique += f.uniqueBytes;
      ++uniqueFiles;
    }
  }
  if (origFiles)
    putf(s,
         ",\"orig_read_bytes\":%llu,\"orig_files\":%llu,\"unique_bytes\":%llu,"
         "\"unique_files\":%llu",
         (unsigned long long)origRead, (unsigned long long)origFiles, (unsigned long long)unique,
         (unsigned long long)uniqueFiles);
  putf(s, ",\"files_bytes\":%llu", (unsigned long long)filesBytes);
  if (!r.counterSource.empty())
    putf(s, ",\"counter_source\":\"%s\"", r.counterSource.c_str());
  if (r.pmuDuty >= 0.0)
    putf(s, ",\"pmu_duty\":%.4f", r.pmuDuty);
  if (r.haveRelayRt) {
    s += ",\"relay_rt_us\":";
    putArray(s, r.histRelayRt);
  }
  // By origin: the host in the local form; in the published one its
  // registrable domain, exactly as `origins`, with the hosts of one domain
  // summed into it.
  if (!r.originRtByHost.empty()) {
    std::map<std::string, std::vector<uint64_t>> by;
    for (const auto& [host, h] : r.originRtByHost)
      addInto(by["root://" + (redacted ? registrableDomain(host) : host)], h);
    s += ",\"origins_rt\":[";
    bool first = true;
    for (const auto& [origin, h] : by) {
      if (!first)
        s += ',';
      first = false;
      s += "{\"origin\":\"" + jsonEscape(origin) + "\",\"hist\":";
      putArray(s, h);
      s += '}';
    }
    s += ']';
  }
}

} // namespace

std::string historyJson(const std::vector<Run>& runs, size_t shown, bool redacted,
                        bool oldestFirst) {
  std::string s;
  const Totals t = summarize(runs);
  putf(s,
       "{\"schema\":1,\"totals\":{\"runs\":%zu,\"runs_estimated\":%zu,\"distinct_files\":%zu,"
       "\"duration_s\":%llu,\"cache_bytes\":%llu,\"origin_bytes\":%llu,"
       "\"relay_bytes\":%llu,\"faults\":%llu,\"saved_s\":%.1f,\"gain\":",
       t.runs, t.runsEstimated, t.distinctFiles, (unsigned long long)t.durationS,
       (unsigned long long)t.cacheBytes(), (unsigned long long)t.originBytes,
       (unsigned long long)t.relayBytes, (unsigned long long)t.faults, t.savedS);
  if (t.haveGain)
    putf(s, "%.3f},\"runs\":[", t.gain);
  else
    s += "null},\"runs\":[";
  shown = std::min(shown, runs.size());
  for (size_t n = 0; n < shown; ++n) {
    const Run& r = runs[oldestFirst ? shown - 1 - n : n];
    const GainEstimate g = estimateGain(r, runs);
    if (n)
      s += ',';
    if (redacted)
      putf(s, "{\"start\":\"%s\",\"duration_s\":%llu,\"host\":\"\",", dateOnlyUtc(r.startS).c_str(),
           (unsigned long long)r.durationS());
    else
      putf(s, "{\"start\":%llu,\"duration_s\":%llu,\"host\":\"%s\",\"pid\":%llu,",
           (unsigned long long)r.startS, (unsigned long long)r.durationS(), r.host.c_str(),
           (unsigned long long)r.pid);
    putf(s,
         "\"kind\":\"%s\",\"files\":%llu,\"served_bytes\":%llu,"
         "\"origin_bytes\":%llu,\"hit_bytes\":%llu,\"replica_bytes\":%llu,"
         "\"relay_bytes\":%llu,\"faults\":%llu,\"sig\":\"%s\","
         "\"threads\":%llu,\"cpu_us\":%llu,\"instructions\":%llu,"
         "\"cycles\":%llu,\"read_sig\":\"%s\","
         // The two columns the table shows and this did not: a scripted
         // reader was told it gets the same figures.
         "\"peak_cores\":%llu,\"origin_reads_in_flight_high_water\":%llu,"
         // `overhead` is the live measure a fill is judged by; `fill_cost`
         // beside it is superseded and kept only so a consumer that reads it
         // does not lose the key.
         "\"origin_share\":%.3f,\"fill_cost\":%.4f,"
         "\"overhead\":%.4f,\"overhead_known\":%s,\"gain\":",
         runKind(r), (unsigned long long)r.files.size(), (unsigned long long)r.servedBytes,
         (unsigned long long)r.originBytes, (unsigned long long)r.hitBytes,
         (unsigned long long)r.replicaBytesServed, (unsigned long long)r.relayBytes,
         (unsigned long long)r.faults(), r.sig.c_str(), (unsigned long long)r.threadsHighWater,
         (unsigned long long)r.cpuUs, (unsigned long long)r.instructions,
         (unsigned long long)r.cycles, r.readSig.c_str(), (unsigned long long)r.peakCores,
         (unsigned long long)r.originReadsInFlight, r.originShare(), r.fillCost(), r.overhead(),
         r.overheadKnown() ? "true" : "false");
    if (g.valid)
      // Not a literal: "baseline" told a scripted reader every reference was
      // an explicit no-cache run, when the whole point of the inference is
      // that most will be qualifying fills. compared_files is the denominator
      // the coverage rule uses; matched_files is the plain intersection.
      putf(s,
           "%.3f,\"gain_source\":\"%s\",\"work_verified\":%s,"
           "\"checked_files\":%llu,\"compared_files\":%llu,\"matched_files\":%llu",
           g.gain, g.referenceDisabled ? "disabled" : "fill", g.workVerified ? "true" : "false",
           (unsigned long long)g.sigPairs, (unsigned long long)g.comparedFiles,
           (unsigned long long)g.matchedFiles);
    else
      s += "null,\"gain_source\":null";
    putRawFields(s, r, redacted);
    if (redacted) {
      std::set<std::string> domains;
      for (const auto& [host, count] : r.originHosts) {
        (void)count;
        domains.insert("root://" + registrableDomain(host));
      }
      s += ",\"origins\":[";
      bool first = true;
      for (const auto& d : domains) {
        if (!first)
          s += ',';
        first = false;
        s += '"' + jsonEscape(d) + '"';
      }
      s += ']';
    }
    s += '}';
  }
  s += "]}";
  return s;
}

} // namespace ucache
