// Reader for xdec_trace tracedb engine directories.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/support/result.h"

namespace xdec::storage {

inline constexpr uint32_t kTracedbV5 = 5;
inline constexpr uint32_t kTracedbV6 = 6;
inline constexpr std::size_t kTraceRowSize = 64;
inline constexpr std::size_t kCallGraphRowSize = 64;

// v6 flow.row is a per-thread, per-segment stream:
//   +0 source traceId, +8 global event id, +16 source pc, +24 target pc,
//   +32 kind, +33..39 reserved.
inline constexpr std::size_t kFlowRowSize = 40;

enum class FlowKind : uint8_t {
  DirectBranch = 0,
  ConditionalBranch = 1,
  IndirectBranch = 2,
  Call = 3,
  Return = 4,
};

struct TraceRow {
  uint64_t traceId = 0;
  uint64_t pc = 0;
  int64_t timestamp = 0;
  uint32_t mnemonicId = 0;
  uint32_t flags = 0;
  uint8_t category = 0;
  uint32_t semanticsId = 0;
  uint64_t globalEventId = 0;
};

struct CallGraphRow {
  uint64_t callId = 0;
  uint64_t parentCallId = 0;
  uint64_t callerEntry = 0;
  uint64_t calleeTarget = 0;
  uint64_t callsitePc = 0;
  uint64_t enterTraceId = 0;
  uint64_t exitTraceId = 0;
  uint32_t depth = 0;
  uint8_t flags = 0;

  [[nodiscard]] bool indirect() const noexcept { return (flags & 1u) != 0; }
  [[nodiscard]] bool noReturn() const noexcept { return (flags & 8u) != 0; }
};

struct FlowRow {
  uint64_t traceId = 0;
  uint64_t globalEventId = 0;
  uint64_t sourcePc = 0;
  uint64_t targetPc = 0;
  FlowKind kind = FlowKind::DirectBranch;
};

struct TracedbSegment {
  uint32_t id = 0;
  uint64_t minTraceId = 0;
  uint64_t maxTraceId = 0;
  uint64_t traceCount = 0;
  uint64_t callGraphCount = 0;
  uint64_t flowCount = 0;
};

struct TracedbThread {
  uint32_t id = 0;
  std::vector<TracedbSegment> segments;
};

class Tracedb {
 public:
  [[nodiscard]] static Result<Tracedb> open(const std::filesystem::path& root);

  [[nodiscard]] uint32_t version() const noexcept { return version_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] const std::vector<TracedbThread>& threads() const noexcept {
    return threads_;
  }
  [[nodiscard]] const TracedbThread* findThread(uint32_t id) const noexcept;

  [[nodiscard]] Result<std::vector<TraceRow>> readTrace(uint32_t threadId) const;
  [[nodiscard]] Result<std::vector<CallGraphRow>> readCallGraph(
      uint32_t threadId) const;
  [[nodiscard]] Result<std::vector<FlowRow>> readFlow(uint32_t threadId) const;

  // Dictionary entries are insertion ordered; a row's dictionary id indexes
  // the returned vector directly.
  [[nodiscard]] Result<std::vector<std::string>> readDictionary(
      std::string_view name) const;

 private:
  std::filesystem::path root_;
  uint32_t version_ = 0;
  std::vector<TracedbThread> threads_;
};

}  // namespace xdec::storage
