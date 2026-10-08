// Where every instrumented thread is right now: its context and open spans.
//
// Read by the watchdog (a run that stopped making progress says which pass it
// is inside and for how long) and by the crash handler.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/obs/field.h"

namespace xdec::obs {

struct SpanFrame {
  std::string_view category;
  std::string name;
  uint64_t startNs = 0;
};

struct ThreadSnapshot {
  uint32_t thread = 0;
  uint64_t job = 0;
  Fields context;
  /// Outermost first.
  std::vector<SpanFrame> spans;
};

/// Every thread that has touched the context or span stacks and is still
/// alive. With `tryOnly`, threads whose state is locked are skipped rather
/// than waited for (the crash handler must not deadlock).
[[nodiscard]] std::vector<ThreadSnapshot> threadSnapshots(bool tryOnly = false);

/// Multi-line rendering: one block per thread, spans with their age.
[[nodiscard]] std::string formatThreadSnapshots(const std::vector<ThreadSnapshot>& threads,
                                                uint64_t nowNs);

}  // namespace xdec::obs
