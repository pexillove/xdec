// Process-relative monotonic time and small per-thread identifiers.
#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace xdec::obs {

/// Nanoseconds since the first call into the observability layer. Every log
/// record, span and progress event shares this origin, so a run's timeline
/// reads the same in the text log, the JSONL log and the Chrome trace.
[[nodiscard]] uint64_t monotonicNs() noexcept;

/// Converts a steady_clock point to the same origin as monotonicNs().
[[nodiscard]] uint64_t toMonotonicNs(std::chrono::steady_clock::time_point point) noexcept;

/// A small dense index for the calling thread (0 for the first thread that
/// asks, usually main). Stable for the thread's lifetime.
[[nodiscard]] uint32_t threadIndex() noexcept;

/// Parses "250ms", "30s", "2m", "1h", or a bare number of seconds.
[[nodiscard]] bool parseDuration(std::string_view text, std::chrono::nanoseconds& out) noexcept;

}  // namespace xdec::obs
