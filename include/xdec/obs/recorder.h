// Collects what a run report needs from the log and span streams: warnings
// and errors (bounded), the span summary, the time range. The report itself
// is assembled by the caller, which knows the command and its results.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "xdec/obs/log.h"
#include "xdec/obs/sinks.h"
#include "xdec/obs/span.h"

namespace xdec::obs {

class RunRecorder final : public LogSink, public SpanSink {
 public:
  explicit RunRecorder(LogLevel keep = LogLevel::Warn, std::size_t maxRecords = 1000);

  void write(const LogRecord& record) override;
  void write(const SpanRecord& record) override;

  [[nodiscard]] std::vector<LogRecord> records() const;
  /// Records beyond maxRecords that were counted but not kept.
  [[nodiscard]] std::size_t droppedRecords() const;
  [[nodiscard]] std::size_t countAtLevel(LogLevel level) const;
  [[nodiscard]] const SpanSummarySink& summary() const noexcept { return summary_; }
  /// Top-level spans (depth 0), in completion order.
  [[nodiscard]] std::vector<SpanRecord> topLevelSpans() const;

 private:
  LogLevel keep_;
  std::size_t maxRecords_;
  mutable std::mutex mutex_;
  std::vector<LogRecord> records_;
  std::size_t dropped_ = 0;
  std::size_t counts_[6] = {};
  std::vector<SpanRecord> topLevel_;
  SpanSummarySink summary_;
};

}  // namespace xdec::obs
