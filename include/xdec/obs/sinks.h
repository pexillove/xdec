// Built-in sinks.
#pragma once

#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "xdec/obs/job.h"
#include "xdec/obs/log.h"
#include "xdec/obs/span.h"

namespace xdec::obs {

/// Serialises everything written to stderr by this layer, and owns the one
/// transient status line an interactive CLI shows at the bottom of the
/// terminal. A sink writing to stderr goes through write(), which erases the
/// status line, writes, and redraws it, so the two never interleave.
class Terminal {
 public:
  static Terminal& instance();

  void write(std::string_view text);
  /// Replaces the status line (empty hides it). Only drawn when enabled.
  void setStatus(std::string line);
  void enableStatus(bool enabled);
  [[nodiscard]] bool statusEnabled() const;
  /// Whether stderr is an interactive terminal.
  [[nodiscard]] static bool stderrIsTty() noexcept;

 private:
  Terminal() = default;
  void eraseLocked();
  void drawLocked();

  mutable std::mutex mutex_;
  std::string status_;
  bool statusEnabled_ = false;
  bool statusDrawn_ = false;
};

/// Human-readable lines:
///   `  12.345s [info  driver] message  (entry=0x1000 round=3 pass=vars)`
/// The `cli` category is the tool talking to its user and prints bare:
/// `message`, `warning: message`, `error: message`.
class TextSink final : public LogSink {
 public:
  struct Options {
    bool showTime = true;
    bool showContext = true;
    bool showSource = false;
    /// Records less severe than this are dropped by this sink alone (the
    /// category levels decide what is recorded at all).
    LogLevel maxLevel = LogLevel::Trace;
  };
  /// Writes through Terminal (stderr).
  TextSink();
  explicit TextSink(Options options);
  /// Writes to a file, replacing it.
  static std::shared_ptr<TextSink> toFile(const std::filesystem::path& path, Options options,
                                          std::string* error);
  ~TextSink() override;

  void write(const LogRecord& record) override;
  void flush() override;

  [[nodiscard]] static std::string format(const LogRecord& record, const Options& options);

 private:
  Options options_;
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
};

/// One JSON object per line. Logs are `{"type":"log",...}`, progress events
/// `{"type":"progress",...}`, spans `{"type":"span",...}`; which of the three
/// a given instance receives depends on where it is registered.
class JsonLinesSink final : public LogSink, public SpanSink, public ProgressSink {
 public:
  /// Writes to stderr through Terminal.
  JsonLinesSink();
  static std::shared_ptr<JsonLinesSink> toFile(const std::filesystem::path& path,
                                               std::string* error);
  ~JsonLinesSink() override;

  void write(const LogRecord& record) override;
  void write(const SpanRecord& record) override;
  void progress(const Job& job, const ProgressEvent& event) override;
  void flush() override;

  [[nodiscard]] static std::string format(const LogRecord& record);
  [[nodiscard]] static std::string format(const SpanRecord& record);
  [[nodiscard]] static std::string format(const Job& job, const ProgressEvent& event);

 private:
  void writeLine(std::string line);

  std::FILE* file_ = nullptr;
  std::mutex mutex_;
};

/// The last `capacity` records, for a crash dump or a GUI log panel.
class RingBufferSink final : public LogSink {
 public:
  explicit RingBufferSink(std::size_t capacity, LogLevel minimum = LogLevel::Trace);
  void write(const LogRecord& record) override;
  [[nodiscard]] std::vector<LogRecord> snapshot() const;
  /// Non-blocking variant for the crash handler: empty if the lock is held.
  [[nodiscard]] std::vector<LogRecord> trySnapshot() const;

 private:
  std::size_t capacity_;
  LogLevel minimum_;
  mutable std::mutex mutex_;
  std::deque<LogRecord> records_;
};

/// Chrome Trace Event format (JSON array form), streamed as spans finish, so
/// a run that is killed still leaves a loadable file (the format tolerates a
/// missing closing bracket). Open in Perfetto or chrome://tracing.
class ChromeTraceSink final : public SpanSink {
 public:
  static std::shared_ptr<ChromeTraceSink> create(const std::filesystem::path& path,
                                                 std::string* error);
  ~ChromeTraceSink() override;

  void write(const SpanRecord& record) override;
  /// A counter track sample ("ph":"C").
  void counter(std::string_view name, uint64_t timeNs, const Fields& values);
  void flush() override;
  void close();

 private:
  explicit ChromeTraceSink(std::FILE* file);
  void writeEvent(const std::string& event);

  std::FILE* file_;
  std::mutex mutex_;
  bool first_ = true;
  std::vector<uint32_t> namedThreads_;
};

/// Aggregates spans by (category, name).
class SpanSummarySink final : public SpanSink {
 public:
  struct Stat {
    std::string category;
    std::string name;
    uint64_t count = 0;
    uint64_t totalNs = 0;
    uint64_t selfNs = 0;
    uint64_t maxNs = 0;
  };

  void write(const SpanRecord& record) override;
  /// Sorted by self time, largest first.
  [[nodiscard]] std::vector<Stat> stats() const;
  /// A fixed-width table of the top `limit` rows.
  [[nodiscard]] std::string format(std::size_t limit = 25) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, Stat> stats_;
};

}  // namespace xdec::obs
