// The boundary between cancellation (an exception, see obs/job.h) and the
// Result convention: API entry points run their body through catchStop() so a
// stopped job comes back as a Cancelled/Timeout diagnostic, never as an
// exception escaping into the caller.
#pragma once

#include <format>
#include <utility>

#include "xdec/obs/job.h"
#include "xdec/support/diag.h"
#include "xdec/support/result.h"

namespace xdec {

[[nodiscard]] inline Diag toDiag(const obs::CancelledError& stop) {
  const DiagCode code =
      stop.reason() == obs::StopReason::Timeout ? DiagCode::Timeout : DiagCode::Cancelled;
  return Diag{code, stop.reason() == obs::StopReason::Timeout
                        ? std::format("hard deadline passed at {}", stop.where())
                        : std::format("cancelled at {}", stop.where())};
}

/// Runs `body` (returning a Result), converting a CancelledError into an error.
template <class Body>
auto catchStop(Body&& body) -> decltype(std::forward<Body>(body)()) {
  try {
    return std::forward<Body>(body)();
  } catch (const obs::CancelledError& stop) {
    return err(toDiag(stop));
  }
}

}  // namespace xdec
