#include "xdec/obs/monitor.h"

#include <format>

#include "xdec/obs/clock.h"
#include "xdec/obs/metrics.h"
#include "xdec/obs/sinks.h"
#include "xdec/obs/threads.h"

namespace xdec::obs {

XDEC_DEFINE_LOG_CATEGORY(watchdogLog, "watchdog")

namespace {

std::string clockText(uint64_t ns) {
  const uint64_t total = ns / 1'000'000'000;
  if (total >= 3600) {
    return std::format("{}:{:02}:{:02}", total / 3600, (total / 60) % 60, total % 60);
  }
  return std::format("{:02}:{:02}", total / 60, total % 60);
}

}  // namespace

Monitor::Monitor(Job& job, Options options) : job_(job), options_(options) {
  if (options_.status) {
    Terminal::instance().enableStatus(true);
  }
  thread_ = std::thread([this] { run(); });
}

Monitor::~Monitor() { stop(); }

void Monitor::stop() {
  {
    const std::lock_guard lock{mutex_};
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  if (options_.status) {
    Terminal::instance().setStatus({});
    Terminal::instance().enableStatus(false);
  }
}

std::string Monitor::statusLine(const Job& job) {
  const uint64_t now = monotonicNs();
  std::string line = std::format("[{}]", clockText(now - job.startNs()));
  const ProgressEvent last = job.lastProgress();
  if (!last.detail.empty()) {
    line += ' ';
    line += last.detail;
  } else if (!last.phase.empty()) {
    line += ' ';
    line += last.phase;
  }
  // The innermost open span of the job's thread says what is running *now*,
  // which the last progress event may not (a single long pass sends none).
  for (const ThreadSnapshot& thread : threadSnapshots()) {
    if (thread.job != job.id() || thread.spans.empty()) {
      continue;
    }
    const SpanFrame& inner = thread.spans.back();
    line += std::format(" | {} {} {:.1f}s", inner.category, inner.name,
                        static_cast<double>(now - inner.startNs) / 1e9);
    break;
  }
  const MemoryUsage memory = processMemory();
  if (memory.currentBytes != 0) {
    line += std::format(" | {} MiB", memory.currentBytes >> 20);
  }
  if (const std::string_view reason = job.windDownReason(); !reason.empty()) {
    line += std::format(" | winding down ({})", reason);
  }
  return line;
}

void Monitor::run() {
  const JobScope scope{job_};
  const uint64_t stallNs = static_cast<uint64_t>(options_.stallAfter.count());
  uint64_t threshold = stallNs;
  uint64_t reportedActivity = 0;
  std::unique_lock lock{mutex_};
  while (!stopping_) {
    wake_.wait_for(lock, options_.tick);
    if (stopping_) {
      break;
    }
    lock.unlock();
    if (options_.onTick) {
      options_.onTick();
    }
    if (options_.status) {
      Terminal::instance().setStatus(statusLine(job_));
    }
    if (stallNs != 0) {
      const uint64_t now = monotonicNs();
      const uint64_t activity = job_.lastActivityNs();
      if (activity != reportedActivity) {
        threshold = stallNs;
      }
      if (now > activity && now - activity >= threshold) {
        stallReports_.fetch_add(1, std::memory_order_relaxed);
        reportedActivity = activity;
        XDEC_LOG_WARN(watchdogLog(),
                      "job '{}' has made no progress for {:.1f}s; open spans:\n{}", job_.name(),
                      static_cast<double>(now - activity) / 1e9,
                      formatThreadSnapshots(threadSnapshots(), now));
        threshold *= 2;
      }
    }
    lock.lock();
  }
}

}  // namespace xdec::obs
