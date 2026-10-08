#include "xdec/obs/recorder.h"

namespace xdec::obs {

RunRecorder::RunRecorder(LogLevel keep, std::size_t maxRecords)
    : keep_(keep), maxRecords_(maxRecords) {}

void RunRecorder::write(const LogRecord& record) {
  const std::lock_guard lock{mutex_};
  ++counts_[static_cast<std::size_t>(record.level)];
  if (record.level == LogLevel::Off || record.level > keep_) {
    return;
  }
  if (records_.size() >= maxRecords_) {
    ++dropped_;
    return;
  }
  records_.push_back(record);
}

void RunRecorder::write(const SpanRecord& record) {
  summary_.write(record);
  if (record.depth == 0) {
    const std::lock_guard lock{mutex_};
    topLevel_.push_back(record);
  }
}

std::vector<LogRecord> RunRecorder::records() const {
  const std::lock_guard lock{mutex_};
  return records_;
}

std::size_t RunRecorder::droppedRecords() const {
  const std::lock_guard lock{mutex_};
  return dropped_;
}

std::size_t RunRecorder::countAtLevel(LogLevel level) const {
  const std::lock_guard lock{mutex_};
  return counts_[static_cast<std::size_t>(level)];
}

std::vector<SpanRecord> RunRecorder::topLevelSpans() const {
  const std::lock_guard lock{mutex_};
  return topLevel_;
}

}  // namespace xdec::obs
