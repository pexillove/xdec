// A background thread that watches one job: renders the terminal status line
// and, as a watchdog, reports when the job stops making progress.
//
// "Progress" is any progress event or span boundary on the job. A single pass
// that runs for minutes produces neither, which is exactly the situation the
// watchdog exists for: it logs (category "watchdog", level warn) how long the
// job has been silent and every thread's open spans and context, then again
// at doubling intervals while the silence lasts. It never stops the job.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "xdec/obs/job.h"

namespace xdec::obs {

XDEC_DECLARE_LOG_CATEGORY(watchdogLog)

class Monitor {
 public:
  struct Options {
    std::chrono::milliseconds tick{200};
    /// Draw the status line through Terminal.
    bool status = false;
    /// Report a stall after this long without progress; zero disables.
    std::chrono::nanoseconds stallAfter{0};
    /// Called on the monitor thread every tick (memory sampling, say).
    std::function<void()> onTick;
  };

  Monitor(Job& job, Options options);
  ~Monitor();

  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  void stop();
  [[nodiscard]] unsigned stallReports() const noexcept {
    return stallReports_.load(std::memory_order_relaxed);
  }

  /// The status line for `job` as of now, as the monitor would draw it.
  [[nodiscard]] static std::string statusLine(const Job& job);

 private:
  void run();

  Job& job_;
  Options options_;
  std::atomic<unsigned> stallReports_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace xdec::obs
