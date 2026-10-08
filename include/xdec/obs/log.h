// Category-based structured logging.
//
// Each subsystem declares a category; verbosity is set per category, because
// debugging a decompiler usually means turning one subsystem up to Trace while
// keeping everything else quiet. Configure with the XDEC_LOG environment
// variable or the CLI's `--log` (same grammar): `driver=info,pass=debug`, a
// bare level for every category, `*=level` likewise.
//
// A record is more than its message: it carries the time, the thread, the job
// it belongs to, the scoped context active when it was emitted (entry VA,
// round, pass -- see context.h) and optional typed fields. Where it goes is
// decided by sinks, not by the call site: the CLI installs a stderr text sink,
// a JSONL file, a ring buffer for crash dumps; a GUI installs a callback sink
// per job. The call site stays `XDEC_LOG_INFO(driverLog(), "...", ...)`.
//
// The disabled path is one relaxed atomic load.
#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/obs/field.h"

namespace xdec::obs {

enum class LogLevel : uint8_t {
  Off = 0,
  Error,
  Warn,
  Info,
  Debug,
  Trace,
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;
[[nodiscard]] bool parseLogLevel(std::string_view text, LogLevel& out) noexcept;

class LogCategory {
 public:
  /// Registers the category. Intended for a function-local static (see
  /// XDEC_DEFINE_LOG_CATEGORY); `defaultLevel` is what it runs at when no
  /// configuration names it.
  explicit LogCategory(std::string_view name, LogLevel defaultLevel = LogLevel::Warn) noexcept;

  LogCategory(const LogCategory&) = delete;
  LogCategory& operator=(const LogCategory&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] LogLevel defaultLevel() const noexcept { return default_; }
  [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }
  void setLevel(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
  [[nodiscard]] bool enabled(LogLevel level) const noexcept {
    return level != LogLevel::Off && level <= this->level();
  }

 private:
  std::string_view name_;
  LogLevel default_;
  std::atomic<LogLevel> level_;
};

struct LogRecord {
  /// monotonicNs() at emission.
  uint64_t timeNs = 0;
  /// Wall-clock time, microseconds since the Unix epoch.
  int64_t wallUs = 0;
  LogLevel level = LogLevel::Info;
  std::string_view category;
  std::string message;
  std::source_location where;
  uint32_t thread = 0;
  /// Job::id() of the job active on the emitting thread, 0 for none.
  uint64_t job = 0;
  /// Scoped context, outermost first (see context.h).
  Fields context;
  Fields fields;
};

class LogSink {
 public:
  virtual ~LogSink() = default;
  /// Called with the router's lock released; may be called from any thread,
  /// so implementations synchronise themselves. Must not block for long: a
  /// GUI sink queues and returns.
  virtual void write(const LogRecord& record) = 0;
  virtual void flush() {}
};

using SinkId = uint64_t;

/// Process-wide sinks. The process starts with one: a stderr text sink.
SinkId addLogSink(std::shared_ptr<LogSink> sink);
void removeLogSink(SinkId id);
/// Removes every process-wide sink, including the default stderr one.
void clearLogSinks();
void flushLogSinks();

/// Applies a spec (`cat=level,...`, a bare level, `*=level`). Categories not
/// registered yet pick the setting up when they register. Returns false and
/// fills `error` on a malformed clause; clauses before it stay applied.
bool applyLogSpec(std::string_view spec, std::string* error = nullptr);

/// Applies XDEC_LOG. Runs automatically when the first category registers;
/// calling it again re-applies it over whatever was set since.
void initLoggingFromEnvironment();

/// Every registered category back to its default level, and forgets pending
/// configuration. For tests.
void resetLogLevels();

/// Sets one registered category by name; false if unknown.
bool setLogLevel(std::string_view categoryName, LogLevel level);
void setAllLogLevels(LogLevel level);

[[nodiscard]] std::vector<LogCategory*> logCategories();

/// Routes a finished record to the process-wide sinks and the current job's.
void dispatch(const LogRecord& record);

namespace detail {
void emit(const LogCategory& category, LogLevel level, std::source_location where, Fields fields,
          std::string message);

template <class... Args>
void emitFormatted(const LogCategory& category, LogLevel level, std::source_location where,
                   Fields fields, std::format_string<Args...> fmt, Args&&... args) {
  emit(category, level, where, std::move(fields), std::format(fmt, std::forward<Args>(args)...));
}
}  // namespace detail

}  // namespace xdec::obs

/// Declares a category accessor in a header.
#define XDEC_DECLARE_LOG_CATEGORY(accessor) ::xdec::obs::LogCategory& accessor() noexcept;

/// Defines a category in exactly one translation unit. The function-local
/// static avoids static initialisation order problems between subsystems.
#define XDEC_DEFINE_LOG_CATEGORY(accessor, nameLiteral) \
  XDEC_DEFINE_LOG_CATEGORY_LEVEL(accessor, nameLiteral, ::xdec::obs::LogLevel::Warn)

#define XDEC_DEFINE_LOG_CATEGORY_LEVEL(accessor, nameLiteral, levelValue) \
  ::xdec::obs::LogCategory& accessor() noexcept {                         \
    static ::xdec::obs::LogCategory category{nameLiteral, levelValue};    \
    return category;                                                      \
  }

/// XDEC_LOG_F(cat, level, obs::kv("round", r), "fmt", args...) attaches typed
/// fields; the plain forms below attach none.
#define XDEC_LOG_F(categoryExpr, levelValue, fieldsExpr, ...)                              \
  do {                                                                                     \
    ::xdec::obs::LogCategory& xdecLogCat = (categoryExpr);                                 \
    if (xdecLogCat.enabled(levelValue)) {                                                  \
      ::xdec::obs::detail::emitFormatted(xdecLogCat, levelValue,                           \
                                         std::source_location::current(), (fieldsExpr),    \
                                         __VA_ARGS__);                                     \
    }                                                                                      \
  } while (0)

#define XDEC_LOG(categoryExpr, levelValue, ...) \
  XDEC_LOG_F(categoryExpr, levelValue, ::xdec::obs::Fields{}, __VA_ARGS__)

#define XDEC_LOG_ERROR(cat, ...) XDEC_LOG(cat, ::xdec::obs::LogLevel::Error, __VA_ARGS__)
#define XDEC_LOG_WARN(cat, ...) XDEC_LOG(cat, ::xdec::obs::LogLevel::Warn, __VA_ARGS__)
#define XDEC_LOG_INFO(cat, ...) XDEC_LOG(cat, ::xdec::obs::LogLevel::Info, __VA_ARGS__)
#define XDEC_LOG_DEBUG(cat, ...) XDEC_LOG(cat, ::xdec::obs::LogLevel::Debug, __VA_ARGS__)
#define XDEC_LOG_TRACE(cat, ...) XDEC_LOG(cat, ::xdec::obs::LogLevel::Trace, __VA_ARGS__)
