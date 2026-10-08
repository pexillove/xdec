// Process-wide counters and memory usage.
//
// A counter is a named relaxed atomic, declared like a log category so a
// subsystem owns its own and the registry enumerates them for the run report.
// Counters are for events worth a total (rounds, discoveries, resolved
// branches, explored paths), not for per-op hot paths.
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace xdec::obs {

class Counter {
 public:
  explicit Counter(std::string_view name) noexcept;

  Counter(const Counter&) = delete;
  Counter& operator=(const Counter&) = delete;

  void add(uint64_t amount = 1) noexcept { value_.fetch_add(amount, std::memory_order_relaxed); }
  [[nodiscard]] uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  void reset() noexcept { value_.store(0, std::memory_order_relaxed); }

 private:
  std::string_view name_;
  std::atomic<uint64_t> value_{0};
};

/// Every registered counter's current value, sorted by name.
[[nodiscard]] std::vector<std::pair<std::string_view, uint64_t>> counterSnapshot();
void resetCounters();

struct MemoryUsage {
  uint64_t currentBytes = 0;
  uint64_t peakBytes = 0;
};

/// Resident set size of this process, now and at its peak. Zero where the
/// platform offers no way to ask.
[[nodiscard]] MemoryUsage processMemory() noexcept;

}  // namespace xdec::obs

#define XDEC_DECLARE_COUNTER(accessor) ::xdec::obs::Counter& accessor() noexcept;
#define XDEC_DEFINE_COUNTER(accessor, nameLiteral)  \
  ::xdec::obs::Counter& accessor() noexcept {       \
    static ::xdec::obs::Counter counter{nameLiteral}; \
    return counter;                                 \
  }
