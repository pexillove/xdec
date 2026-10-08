#include "xdec/obs/job.h"

#include "thread_state.h"
#include "xdec/obs/clock.h"

namespace xdec::obs {
namespace {

std::atomic<uint64_t> gNextJobId{1};
thread_local Job* tCurrent = nullptr;

uint64_t deadlineFromNow(std::chrono::nanoseconds fromNow) noexcept {
  if (fromNow.count() <= 0) {
    return 0;
  }
  return monotonicNs() + static_cast<uint64_t>(fromNow.count());
}

}  // namespace

std::string_view toString(StopReason reason) noexcept {
  switch (reason) {
    case StopReason::None:
      return "none";
    case StopReason::Cancelled:
      return "cancelled";
    case StopReason::Timeout:
      return "timeout";
  }
  return "?";
}

Job::Job(std::string name)
    : id_(gNextJobId.fetch_add(1, std::memory_order_relaxed)),
      name_(std::move(name)),
      startNs_(monotonicNs()) {
  lastActivityNs_.store(startNs_, std::memory_order_relaxed);
}

Job::~Job() = default;

void Job::setSoftDeadline(std::chrono::nanoseconds fromNow) noexcept {
  softDeadlineNs_.store(deadlineFromNow(fromNow), std::memory_order_relaxed);
}

void Job::setHardDeadline(std::chrono::nanoseconds fromNow) noexcept {
  hardDeadlineNs_.store(deadlineFromNow(fromNow), std::memory_order_relaxed);
}

StopReason Job::stopReason() const noexcept {
  if (cancelled_.load(std::memory_order_relaxed) ||
      interrupts_.load(std::memory_order_relaxed) >= 2) {
    return StopReason::Cancelled;
  }
  const uint64_t hard = hardDeadlineNs_.load(std::memory_order_relaxed);
  if (hard != 0 && monotonicNs() >= hard) {
    return StopReason::Timeout;
  }
  return StopReason::None;
}

bool Job::windDownRequested() const noexcept { return !windDownReason().empty(); }

std::string_view Job::windDownReason() const noexcept {
  if (interrupts_.load(std::memory_order_relaxed) >= 1) {
    return "interrupt";
  }
  const uint64_t soft = softDeadlineNs_.load(std::memory_order_relaxed);
  if (soft != 0 && monotonicNs() >= soft) {
    return "deadline";
  }
  switch (stopReason()) {
    case StopReason::Cancelled:
      return "cancel";
    case StopReason::Timeout:
      return "deadline";
    case StopReason::None:
      break;
  }
  return {};
}

void Job::touch() noexcept { lastActivityNs_.store(monotonicNs(), std::memory_order_relaxed); }

void Job::progress(ProgressEvent event) {
  event.timeNs = monotonicNs();
  std::vector<std::shared_ptr<ProgressSink>> sinks;
  {
    const std::lock_guard lock{mutex_};
    last_ = event;
    sinks = progressSinks_;
  }
  lastActivityNs_.store(event.timeNs, std::memory_order_relaxed);
  for (const auto& sink : sinks) {
    sink->progress(*this, event);
  }
}

ProgressEvent Job::lastProgress() const {
  const std::lock_guard lock{mutex_};
  return last_;
}

void Job::addLogSink(std::shared_ptr<LogSink> sink) {
  const std::lock_guard lock{mutex_};
  logSinks_.push_back(std::move(sink));
}

void Job::addSpanSink(std::shared_ptr<SpanSink> sink) {
  const std::lock_guard lock{mutex_};
  spanSinks_.push_back(std::move(sink));
  spanSinkCount_.store(static_cast<unsigned>(spanSinks_.size()), std::memory_order_relaxed);
}

void Job::addProgressSink(std::shared_ptr<ProgressSink> sink) {
  const std::lock_guard lock{mutex_};
  progressSinks_.push_back(std::move(sink));
}

void Job::deliver(const LogRecord& record) const {
  std::vector<std::shared_ptr<LogSink>> sinks;
  {
    const std::lock_guard lock{mutex_};
    if (logSinks_.empty()) {
      return;
    }
    sinks = logSinks_;
  }
  for (const auto& sink : sinks) {
    sink->write(record);
  }
}

void Job::deliver(const SpanRecord& record) const {
  std::vector<std::shared_ptr<SpanSink>> sinks;
  {
    const std::lock_guard lock{mutex_};
    sinks = spanSinks_;
  }
  for (const auto& sink : sinks) {
    sink->write(record);
  }
}

Job* Job::current() noexcept { return tCurrent; }

JobScope::JobScope(Job& job) noexcept : previous_(tCurrent) {
  tCurrent = &job;
  detail::ThreadState& state = detail::threadState();
  const std::lock_guard lock{state.mutex};
  state.job = job.id();
}

JobScope::~JobScope() {
  tCurrent = previous_;
  detail::ThreadState& state = detail::threadState();
  const std::lock_guard lock{state.mutex};
  state.job = previous_ != nullptr ? previous_->id() : 0;
}

void checkpoint(std::string_view where) {
  const Job* job = tCurrent;
  if (job == nullptr) {
    return;
  }
  const StopReason reason = job->stopReason();
  if (reason != StopReason::None) {
    throw CancelledError{reason, std::string{where}};
  }
}

bool windDownRequested() noexcept { return tCurrent != nullptr && tCurrent->windDownRequested(); }

void reportProgress(std::string_view phase, std::string detail, uint64_t current, uint64_t total,
                    Fields fields) {
  Job* job = tCurrent;
  if (job == nullptr) {
    return;
  }
  ProgressEvent event;
  event.phase = std::string{phase};
  event.detail = std::move(detail);
  event.current = current;
  event.total = total;
  event.fields = std::move(fields);
  job->progress(std::move(event));
}

}  // namespace xdec::obs
