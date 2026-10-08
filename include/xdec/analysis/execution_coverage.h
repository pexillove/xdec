// Dynamic instruction and edge coverage recovered from a tracedb invocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "xdec/storage/tracedb.h"
#include "xdec/support/result.h"

namespace xdec::analysis {

struct CoverageSelection {
  // Address in the image being decompiled. When imageBase is nonzero, frames
  // are matched by module offset so ASLR does not have to agree with the trace.
  // Zero means match only by callIds.
  uint64_t functionTarget = 0;
  uint64_t imageBase = 0;
  std::optional<uint32_t> threadId;
  // Empty means every matching invocation. If present, callIds and ordinals
  // are a union. Ordinals are zero-based after sorting by thread/entry/id.
  std::vector<uint64_t> callIds;
  std::vector<std::size_t> ordinals;
};

struct ObservedSuccessor {
  uint64_t sourcePc = 0;
  uint64_t targetPc = 0;
  uint64_t observations = 0;

  friend bool operator==(const ObservedSuccessor&,
                         const ObservedSuccessor&) = default;
};

struct InvocationCoverage {
  uint32_t threadId = 0;
  uint64_t callId = 0;
  uint64_t parentCallId = 0;
  uint64_t functionTarget = 0;
  uint64_t enterTraceId = 0;
  uint64_t exitTraceId = 0;
  bool noReturn = false;
  std::vector<uint64_t> executedPcs;
  std::vector<ObservedSuccessor> successors;
};

class ExecutionCoverage {
 public:
  [[nodiscard]] static Result<ExecutionCoverage> load(
      const storage::Tracedb& database, const CoverageSelection& selection);

  [[nodiscard]] const std::vector<InvocationCoverage>& invocations() const noexcept {
    return invocations_;
  }
  [[nodiscard]] bool contains(uint64_t pc) const noexcept;
  [[nodiscard]] std::vector<uint64_t> successors(uint64_t pc) const;
  [[nodiscard]] std::size_t instructionCount() const noexcept;

 private:
  std::vector<InvocationCoverage> invocations_;
};

}  // namespace xdec::analysis
