#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace ucache {
namespace {
std::atomic<int> gLevel{static_cast<int>(LogLevel::kWarn)}; // as configured
std::atomic<int> gRaised{static_cast<int>(LogLevel::kError)}; // from the XRootD client
std::atomic<int> gFd{STDERR_FILENO};
std::atomic<bool> gFileNamed{false};
std::atomic<Log::Sink> gSink{nullptr};
std::atomic<Log::Context> gContext{nullptr};
std::atomic<bool> gContextShown{false};

std::mutex gSpecMu;
std::string& specStore() {
  static auto* s = new std::string("warn"); // leaked: logging outlives static teardown
  return *s;
}

int effective() { return std::max(gLevel.load(), gRaised.load()); }

// Repeated warnings, by call site: a format string is a literal, so its
// address names the kind of message.
constexpr uint64_t kShowPerKind = 20;
constexpr time_t kSummaryEvery = 60;
struct Kind {
  const char* fmt = nullptr;
  uint64_t shown = 0, held = 0;
  time_t since = 0;
  char last[240] = {0}; // the latest one held back, as an example
};
constexpr size_t kKinds = 64; // more kinds than this are not limited
std::mutex gLimitMu;
Kind* kinds() {
  static auto* k = new Kind[kKinds];
  return k;
}

void emit(LogLevel lvl, const char* body) {
  if (!gFileNamed.load())
    if (Log::Sink s = gSink.load(); s && s(lvl, body))
      return;
  static const char* names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
  char buf[1100];
  time_t now = ::time(nullptr);
  struct tm tmv;
  ::localtime_r(&now, &tmv);
  int n = ::snprintf(buf, sizeof buf, "[%02d:%02d:%02d][ucache %s] %s", tmv.tm_hour, tmv.tm_min,
                     tmv.tm_sec, names[static_cast<int>(lvl)], body);
  if (n > static_cast<int>(sizeof buf) - 2)
    n = sizeof buf - 2;
  buf[n++] = '\n';
  // Single write per message; failure is deliberately ignored (fail-open).
  [[maybe_unused]] ssize_t r = ::write(gFd.load(), buf, n);
}

// false: hold this one back. `summary` gets a line to print in its place when
// one is due.
bool admit(const char* fmt, const char* body, std::string& summary) {
  std::lock_guard<std::mutex> g(gLimitMu);
  Kind* ks = kinds();
  Kind* k = nullptr;
  for (size_t i = 0; i < kKinds; ++i)
    if (ks[i].fmt == fmt || !ks[i].fmt) {
      k = &ks[i];
      break;
    }
  if (!k)
    return true;
  if (!k->fmt) {
    k->fmt = fmt;
    k->since = ::time(nullptr);
  }
  if (k->shown < kShowPerKind) {
    ++k->shown;
    return true;
  }
  ++k->held;
  const size_t bl = std::min(std::strlen(body), sizeof k->last - 1);
  std::memcpy(k->last, body, bl);
  k->last[bl] = 0;
  const time_t now = ::time(nullptr);
  if (now - k->since < kSummaryEvery)
    return false;
  char line[400];
  ::snprintf(line, sizeof line,
             "%llu more like this in the last %llds (log level info shows each), the latest: %.239s",
             static_cast<unsigned long long>(k->held), static_cast<long long>(now - k->since),
             k->last);
  summary = line;
  k->held = 0;
  k->since = now;
  return false;
}
} // namespace

void Log::configure(const std::string& spec) {
  std::string level = spec, path;
  auto colon = spec.find(':');
  if (colon != std::string::npos) {
    level = spec.substr(0, colon);
    path = spec.substr(colon + 1);
  }
  if (level == "error")
    gLevel = 0;
  else if (level == "warn")
    gLevel = 1;
  else if (level == "info")
    gLevel = 2;
  else if (level == "debug")
    gLevel = 3;
  else
    return; // not a level: nothing changes, and settings keeps showing the last one
  // The last spec says where as well as how much: one without a file sends
  // messages back to stderr (or to the client's log, in the plugin).
  if (!path.empty()) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
      gFd = fd; // the old fd is not closed; leaked fds on reconfigure are fine
      gFileNamed = true;
    }
  } else {
    gFd = STDERR_FILENO;
    gFileNamed = false;
  }
  std::lock_guard<std::mutex> g(gSpecMu);
  specStore() = spec;
}

std::string Log::spec() {
  std::lock_guard<std::mutex> g(gSpecMu);
  return specStore();
}

bool Log::enabled(LogLevel lvl) { return static_cast<int>(lvl) <= effective(); }

void Log::raiseLevel(LogLevel lvl) {
  int want = static_cast<int>(lvl), cur = gRaised.load();
  while (want > cur && !gRaised.compare_exchange_weak(cur, want)) {
  }
}

void Log::setSink(Sink s) { gSink = s; }

void Log::setContext(Context c) { gContext = c; }

void Log::write(LogLevel lvl, const char* fmt, ...) {
  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  ::vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  if (lvl <= LogLevel::kWarn && effective() < static_cast<int>(LogLevel::kInfo)) {
    std::string summary;
    if (!admit(fmt, body, summary)) {
      if (!summary.empty())
        emit(lvl, summary.c_str());
      return;
    }
  }
  if (lvl <= LogLevel::kWarn && !gContextShown.load())
    if (Context c = gContext.load(); c && !gContextShown.exchange(true))
      emit(lvl, c().c_str());
  emit(lvl, body);
}

void Log::flushHeldBack() {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> g(gLimitMu);
    Kind* ks = kinds();
    for (size_t i = 0; i < kKinds && ks[i].fmt; ++i) {
      if (!ks[i].held)
        continue;
      char line[400];
      ::snprintf(line, sizeof line, "%llu more like this were held back, the latest: %.239s",
                 static_cast<unsigned long long>(ks[i].held), ks[i].last);
      lines.emplace_back(line);
      ks[i].held = 0;
    }
  }
  for (const auto& l : lines)
    emit(LogLevel::kWarn, l.c_str());
}

} // namespace ucache
