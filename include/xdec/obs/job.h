// A unit of work that can be observed, stopped, and timed out.
//
// One Job per decompilation (or per batch): it owns an id every log record and
// span is stamped with, its own sinks (a GUI tab's log panel), a progress
// channel, and the stop controls. It is found through a thread-local
// "current job" set by JobScope rather than threaded through every signature,
// so a checkpoint deep inside an analysis needs no plumbing -- and a GUI worker
// thread running a different function simply sets its own.
//
// Stopping has two strengths, because "give me what you have" and "stop now"
// are different requests:
//
//   wind down  (soft deadline, first interrupt) -- loops that *grow* the work
//              (driver discovery rounds) stop growing and the run finishes
//              with what it proved, reported as partial;
//   stop       (hard deadline, cancel, second interrupt) -- every checkpoint
//              throws CancelledError, unwinding to the API boundary, which
//              turns it into a Cancelled/Timeout diagnostic.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/obs/field.h"
#include "xdec/obs/log.h"
#include "xdec/obs/span.h"

namespace xdec::obs {

enum class StopReason : uint8_t {
  None,
  /// requestCancel(), or a second interrupt.
  Cancelled,
  /// The hard deadline passed.
  Timeout,
};

[[nodiscard]] std::string_view toString(StopReason reason) noexcept;

struct ProgressEvent {
  /// Coarse stage: "load", "discover", "pipeline", "emit".
  std::string phase;
  /// One line a human reads: "round 3 settle 2: 4512 block(s)".
  std::string detail;
  uint64_t current = 0;
  /// 0 when the total is unknown.
  uint64_t total = 0;
  uint64_t timeNs = 0;
  Fields fields;
};

class Job;

class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  virtual void progress(const Job& job, const ProgressEvent& event) = 0;
};

class Job {
 public:
  explicit Job(std::string name = {});
  ~Job();

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  [[nodiscard]] uint64_t id() const noexcept { return id_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] uint64_t startNs() const noexcept { return startNs_; }

  // -- stopping -------------------------------------------------------------

  /// Stop now: every later checkpoint throws. Safe from any thread.
  void requestCancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
  /// Ctrl+C semantics: first call winds down, second cancels. Async-signal-safe
  /// (a lock-free atomic increment).
  void interrupt() noexcept { interrupts_.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] unsigned interruptCount() const noexcept {
    return interrupts_.load(std::memory_order_relaxed);
  }

  /// Deadlines as durations from now. Zero clears.
  void setSoftDeadline(std::chrono::nanoseconds fromNow) noexcept;
  void setHardDeadline(std::chrono::nanoseconds fromNow) noexcept;

  /// Whether work-growing loops should stop growing (see the header).
  [[nodiscard]] bool windDownRequested() const noexcept;
  /// Why checkpoints are throwing, or None.
  [[nodiscard]] StopReason stopReason() const noexcept;
  /// Why the run wound down: "deadline", "interrupt", or empty.
  [[nodiscard]] std::string_view windDownReason() const noexcept;

  // -- progress -------------------------------------------------------------

  void progress(ProgressEvent event);
  [[nodiscard]] ProgressEvent lastProgress() const;
  /// monotonicNs() of the last progress event or span boundary on this job;
  /// what the watchdog measures stalls against.
  [[nodiscard]] uint64_t lastActivityNs() const noexcept {
    return lastActivityNs_.load(std::memory_order_relaxed);
  }
  void touch() noexcept;

  // -- job-local sinks --------------------------------------------------------

  void addLogSink(std::shared_ptr<LogSink> sink);
  void addSpanSink(std::shared_ptr<SpanSink> sink);
  void addProgressSink(std::shared_ptr<ProgressSink> sink);
  [[nodiscard]] bool hasSpanSinks() const noexcept {
    return spanSinkCount_.load(std::memory_order_relaxed) != 0;
  }

  /// Delivery, used by the routers in log.cpp/span.cpp.
  void deliver(const LogRecord& record) const;
  void deliver(const SpanRecord& record) const;

  /// The job active on the calling thread, or null.
  [[nodiscard]] static Job* current() noexcept;

 private:
  friend class JobScope;

  uint64_t id_;
  std::string name_;
  uint64_t startNs_;
  std::atomic<bool> cancelled_{false};
  std::atomic<unsigned> interrupts_{0};
  std::atomic<uint64_t> softDeadlineNs_{0};
  std::atomic<uint64_t> hardDeadlineNs_{0};
  std::atomic<uint64_t> lastActivityNs_{0};
  std::atomic<unsigned> spanSinkCount_{0};

  mutable std::mutex mutex_;
  ProgressEvent last_;
  std::vector<std::shared_ptr<LogSink>> logSinks_;
  std::vector<std::shared_ptr<SpanSink>> spanSinks_;
  std::vector<std::shared_ptr<ProgressSink>> progressSinks_;
};

/// Makes `job` current on this thread for the scope; restores the previous one.
class JobScope {
 public:
  explicit JobScope(Job& job) noexcept;
  ~JobScope();

  JobScope(const JobScope&) = delete;
  JobScope& operator=(const JobScope&) = delete;

 private:
  Job* previous_;
};

/// Thrown by checkpoint(). Deliberately not a std::exception subclass: a
/// `catch (const std::exception&)` written to contain some unrelated failure
/// must not swallow a cancellation.
class CancelledError {
 public:
  CancelledError(StopReason reason, std::string where)
      : reason_(reason), where_(std::move(where)) {}
  [[nodiscard]] StopReason reason() const noexcept { return reason_; }
  [[nodiscard]] const std::string& where() const noexcept { return where_; }

 private:
  StopReason reason_;
  std::string where_;
};

/// Throws CancelledError when the current job has been stopped. No job, no-op.
/// Cheap enough for per-branch and per-path loops: a thread-local read, two
/// relaxed atomic loads, and a clock read only when a hard deadline is set.
void checkpoint(std::string_view where);

/// The current job's windDownRequested(); false with no job.
[[nodiscard]] bool windDownRequested() noexcept;

/// Sends a progress event to the current job; no job, no-op.
void reportProgress(std::string_view phase, std::string detail, uint64_t current = 0,
                    uint64_t total = 0, Fields fields = {});

}  // namespace xdec::obs
