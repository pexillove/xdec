#include "xdec/analysis/execution_coverage.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <unordered_set>

namespace xdec::analysis {
namespace {

struct Candidate {
  uint32_t thread = 0;
  storage::CallGraphRow frame;
  uint64_t traceRuntimeBase = 0;
  uint64_t outputImageBase = 0;
};

struct Interval {
  uint64_t first = 0;
  uint64_t last = 0;  // inclusive
};

[[nodiscard]] Interval bodyInterval(const storage::CallGraphRow& frame) {
  const uint64_t first =
      frame.enterTraceId + (frame.indirect() ? uint64_t{0} : uint64_t{1});
  uint64_t last = frame.exitTraceId;
  if (!frame.noReturn() && last != 0) --last;
  return {first, last};
}

[[nodiscard]] bool intervalContains(const Interval& interval,
                                    uint64_t id) noexcept {
  return interval.first <= interval.last && id >= interval.first &&
         id <= interval.last;
}

[[nodiscard]] bool childOf(
    const storage::CallGraphRow& possibleChild,
    const storage::CallGraphRow& parent,
    const std::map<uint64_t, const storage::CallGraphRow*>& byId) {
  uint64_t id = possibleChild.parentCallId;
  std::unordered_set<uint64_t> visited;
  while (id != 0 && visited.insert(id).second) {
    if (id == parent.callId) return true;
    const auto found = byId.find(id);
    if (found == byId.end()) return false;
    id = found->second->parentCallId;
  }
  return false;
}

[[nodiscard]] bool selectedById(const CoverageSelection& selection,
                                uint64_t callId) {
  return std::ranges::find(selection.callIds, callId) !=
         selection.callIds.end();
}

void finishCoverage(InvocationCoverage& coverage, uint64_t traceRuntimeBase,
                    uint64_t outputImageBase,
                    std::vector<uint64_t> pcs,
                    std::map<std::pair<uint64_t, uint64_t>, uint64_t> edges) {
  const auto rebase = [&](uint64_t pc) {
    if (outputImageBase == 0 || traceRuntimeBase == 0 || pc >= traceRuntimeBase) {
      return pc;
    }
    return outputImageBase + pc;
  };
  for (uint64_t& pc : pcs) pc = rebase(pc);
  std::ranges::sort(pcs);
  pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
  coverage.executedPcs = std::move(pcs);
  coverage.successors.reserve(edges.size());
  for (const auto& [edge, count] : edges) {
    coverage.successors.push_back({rebase(edge.first), rebase(edge.second), count});
  }
}

}  // namespace

Result<ExecutionCoverage> ExecutionCoverage::load(
    const storage::Tracedb& database, const CoverageSelection& selection) {
  std::vector<Candidate> candidates;
  for (const storage::TracedbThread& thread : database.threads()) {
    if (selection.threadId && *selection.threadId != thread.id) continue;
    XDEC_TRY(std::vector<storage::TraceRow> trace,
             database.readTrace(thread.id));
    XDEC_TRY(std::vector<storage::CallGraphRow> frames,
             database.readCallGraph(thread.id));
    std::map<uint64_t, uint64_t> pcByTraceId;
    for (const storage::TraceRow& row : trace) {
      pcByTraceId.emplace(row.traceId, row.pc);
    }
    for (const storage::CallGraphRow& frame : frames) {
      uint64_t traceRuntimeBase = 0;
      const auto anchor = pcByTraceId.find(frame.enterTraceId);
      if (anchor != pcByTraceId.end()) {
        const uint64_t runtimeAnchor =
            frame.indirect() ? frame.calleeTarget : frame.callsitePc;
        if (runtimeAnchor >= anchor->second) {
          traceRuntimeBase = runtimeAnchor - anchor->second;
        }
      }
      bool targetMatches = false;
      if (selection.functionTarget != 0) {
        if (selection.imageBase != 0 && traceRuntimeBase != 0 &&
            selection.functionTarget >= selection.imageBase &&
            frame.calleeTarget >= traceRuntimeBase) {
          targetMatches =
              selection.functionTarget - selection.imageBase ==
              frame.calleeTarget - traceRuntimeBase;
        } else {
          targetMatches = frame.calleeTarget == selection.functionTarget;
        }
      }
      if (targetMatches || selectedById(selection, frame.callId)) {
        candidates.push_back(
            {thread.id, frame, traceRuntimeBase, selection.imageBase});
      }
    }
  }
  std::ranges::sort(candidates, [](const Candidate& lhs, const Candidate& rhs) {
    if (lhs.thread != rhs.thread) return lhs.thread < rhs.thread;
    if (lhs.frame.enterTraceId != rhs.frame.enterTraceId) {
      return lhs.frame.enterTraceId < rhs.frame.enterTraceId;
    }
    return lhs.frame.callId < rhs.frame.callId;
  });

  if (!selection.ordinals.empty() || !selection.callIds.empty()) {
    std::vector<Candidate> filtered;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      const bool ordinal =
          std::ranges::find(selection.ordinals, index) !=
          selection.ordinals.end();
      if (ordinal || selectedById(selection, candidates[index].frame.callId)) {
        filtered.push_back(candidates[index]);
      }
    }
    candidates = std::move(filtered);
  }

  ExecutionCoverage result;
  for (const Candidate& candidate : candidates) {
    XDEC_TRY(std::vector<storage::TraceRow> trace,
             database.readTrace(candidate.thread));
    XDEC_TRY(std::vector<storage::CallGraphRow> frames,
             database.readCallGraph(candidate.thread));
    XDEC_TRY(std::vector<storage::FlowRow> flow,
             database.readFlow(candidate.thread));

    const Interval body = bodyInterval(candidate.frame);
    if (body.first > body.last) {
      return err(DiagCode::BadFormat,
                 "tracedb call {} has an empty or invalid body interval",
                 candidate.frame.callId);
    }

    std::map<uint64_t, const storage::CallGraphRow*> byId;
    for (const storage::CallGraphRow& frame : frames) {
      if (!byId.emplace(frame.callId, &frame).second) {
        return err(DiagCode::BadFormat,
                   "duplicate tracedb call id {} in thread {}",
                   frame.callId, candidate.thread);
      }
    }
    std::vector<Interval> nested;
    for (const storage::CallGraphRow& frame : frames) {
      if (frame.callId != candidate.frame.callId &&
          childOf(frame, candidate.frame, byId)) {
        nested.push_back(bodyInterval(frame));
      }
    }
    const auto owned = [&](uint64_t id) {
      return intervalContains(body, id) &&
             std::ranges::none_of(
                 nested, [id](const Interval& item) {
                   return intervalContains(item, id);
                 });
    };

    InvocationCoverage coverage;
    coverage.threadId = candidate.thread;
    coverage.callId = candidate.frame.callId;
    coverage.parentCallId = candidate.frame.parentCallId;
    coverage.functionTarget = candidate.frame.calleeTarget;
    coverage.enterTraceId = candidate.frame.enterTraceId;
    coverage.exitTraceId = candidate.frame.exitTraceId;
    coverage.noReturn = candidate.frame.noReturn();

    std::vector<uint64_t> pcs;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> edges;
    const storage::TraceRow* previous = nullptr;
    std::unordered_set<uint64_t> ownedTraceIds;
    for (const storage::TraceRow& row : trace) {
      if (!owned(row.traceId)) continue;
      pcs.push_back(row.pc);
      ownedTraceIds.insert(row.traceId);
      if (database.version() == storage::kTracedbV5 && previous != nullptr) {
        ++edges[{previous->pc, row.pc}];
      }
      previous = &row;
    }

    if (database.version() >= storage::kTracedbV6) {
      for (const storage::FlowRow& row : flow) {
        // A flow row names the source instruction. Requiring the immediately
        // following trace row to be owned removes call and return crossings,
        // not merely the nested instructions themselves.
        if (ownedTraceIds.contains(row.traceId) &&
            ownedTraceIds.contains(row.traceId + 1)) {
          ++edges[{row.sourcePc, row.targetPc}];
        }
      }
    }
    if (candidate.outputImageBase != 0 && candidate.traceRuntimeBase != 0 &&
        candidate.frame.calleeTarget >= candidate.traceRuntimeBase) {
      coverage.functionTarget =
          candidate.outputImageBase +
          (candidate.frame.calleeTarget - candidate.traceRuntimeBase);
    }
    finishCoverage(coverage, candidate.traceRuntimeBase,
                   candidate.outputImageBase, std::move(pcs),
                   std::move(edges));
    result.invocations_.push_back(std::move(coverage));
  }
  return result;
}

bool ExecutionCoverage::contains(uint64_t pc) const noexcept {
  return std::ranges::any_of(invocations_, [pc](const InvocationCoverage& invocation) {
    return std::ranges::binary_search(invocation.executedPcs, pc);
  });
}

std::vector<uint64_t> ExecutionCoverage::successors(uint64_t pc) const {
  std::vector<uint64_t> result;
  for (const InvocationCoverage& invocation : invocations_) {
    for (const ObservedSuccessor& edge : invocation.successors) {
      if (edge.sourcePc == pc &&
          std::ranges::find(result, edge.targetPc) == result.end()) {
        result.push_back(edge.targetPc);
      }
    }
  }
  std::ranges::sort(result);
  return result;
}

std::size_t ExecutionCoverage::instructionCount() const noexcept {
  std::vector<uint64_t> pcs;
  for (const InvocationCoverage& invocation : invocations_) {
    pcs.insert(pcs.end(), invocation.executedPcs.begin(),
               invocation.executedPcs.end());
  }
  std::ranges::sort(pcs);
  return static_cast<std::size_t>(
      std::unique(pcs.begin(), pcs.end()) - pcs.begin());
}

}  // namespace xdec::analysis
