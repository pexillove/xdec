// Per-thread observability state: the context stack, the span stack, and the
// current job id. Each thread's state has its own mutex so another thread (the
// watchdog, the crash handler) can read it; the owning thread's lock is never
// contended except during such a read.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/obs/field.h"

namespace xdec::obs::detail {

struct OpenSpan {
  uint64_t id = 0;
  std::string_view category;
  std::string name;
  uint64_t startNs = 0;
  /// Summed durations of spans that closed directly inside this one.
  uint64_t childNs = 0;
};

struct ThreadState {
  uint32_t index = 0;
  uint64_t job = 0;
  std::mutex mutex;
  Fields context;
  std::vector<OpenSpan> spans;
};

/// The calling thread's state, created and registered on first use.
[[nodiscard]] ThreadState& threadState();

/// Every live thread's state.
[[nodiscard]] std::vector<std::shared_ptr<ThreadState>> allThreadStates();

}  // namespace xdec::obs::detail
