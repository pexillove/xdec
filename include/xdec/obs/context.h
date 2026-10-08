// Scoped, per-thread context attached to every log record.
//
// The driver pushes the entry VA and round, the pass manager the pass name;
// a log line from deep inside an analysis then says which function, which
// round and which pass it came from without any of that being threaded
// through as parameters. Also what the watchdog and crash dump print for a
// thread that is stuck.
#pragma once

#include <string_view>

#include "xdec/obs/field.h"

namespace xdec::obs {

class ScopedContext {
 public:
  ScopedContext(std::string_view key, FieldValue value);
  ~ScopedContext();

  ScopedContext(const ScopedContext&) = delete;
  ScopedContext& operator=(const ScopedContext&) = delete;
};

/// The calling thread's context, outermost first.
[[nodiscard]] Fields contextSnapshot();

}  // namespace xdec::obs

#define XDEC_OBS_CONCAT_INNER(a, b) a##b
#define XDEC_OBS_CONCAT(a, b) XDEC_OBS_CONCAT_INNER(a, b)

/// XDEC_LOG_CONTEXT("pass", info.name); -- active until the end of the scope.
#define XDEC_LOG_CONTEXT(key, value) \
  ::xdec::obs::ScopedContext XDEC_OBS_CONCAT(xdecLogContext_, __LINE__) { key, value }
