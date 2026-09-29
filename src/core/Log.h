// Minimal leveled logger to stderr or a file (UCACHE_LOG).
// The cache must never break a job — logging failures are swallowed.
//
// In the plugin, messages also follow the XRootD client's log: its level
// (XRD_LOGLEVEL) can raise uCache's, never lower it, and a message the
// client's level admits is written through the client's log, so it lands
// where the client's own lines go (XRD_LOGFILE) under the topic "UCache".
// A file named in UCACHE_LOG takes every message instead.
//
// Repeated warnings are held back at the default levels: after 20 of one kind
// (one call site) a process prints a summary at most once a minute. At info
// and debug every message is printed.
//
// Thread-safety: fully thread-safe; each message is one write() call.
#pragma once

#include <cstdarg>
#include <string>

namespace ucache {

enum class LogLevel { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

class Log {
 public:
  // "error"/"warn"/"info"/"debug", optionally "level:/path/to/file".
  static void configure(const std::string& spec);
  // The level asked for, as configure() was given it ("warn" by default).
  static std::string spec();
  static bool enabled(LogLevel lvl);
  static void write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // A second source of verbosity (the XRootD client's level): the effective
  // level is the higher of the two, whatever configure() is given later.
  static void raiseLevel(LogLevel lvl);
  // Where a message goes when no file was named. Returns true if it took the
  // message, which carries no prefix; false and the message goes to stderr.
  using Sink = bool (*)(LogLevel lvl, const char* msg);
  static void setSink(Sink s);
  // One line saying what is running, printed before the process's first
  // warning or error. Must not log.
  using Context = std::string (*)();
  static void setContext(Context c);
  // Print what the repeated-warning limit is holding back (at exit).
  static void flushHeldBack();
};

#define UCACHE_LOG(lvl, ...)                                                                       \
  do {                                                                                             \
    if (::ucache::Log::enabled(lvl))                                                               \
      ::ucache::Log::write(lvl, __VA_ARGS__);                                                      \
  } while (0)
#define UCACHE_ERROR(...) UCACHE_LOG(::ucache::LogLevel::kError, __VA_ARGS__)
#define UCACHE_WARN(...) UCACHE_LOG(::ucache::LogLevel::kWarn, __VA_ARGS__)
#define UCACHE_INFO(...) UCACHE_LOG(::ucache::LogLevel::kInfo, __VA_ARGS__)
#define UCACHE_DEBUG(...) UCACHE_LOG(::ucache::LogLevel::kDebug, __VA_ARGS__)

} // namespace ucache
