#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

#include "storage/tracedb_test_support.h"
#include "xdec/analysis/execution_coverage.h"

using xdec::analysis::CoverageSelection;
using xdec::analysis::ExecutionCoverage;
using xdec::analysis::ObservedSuccessor;
using xdec::storage::FlowKind;
using xdec::storage::Tracedb;
using xdec::test::tracedb::Engine;

namespace {

[[nodiscard]] bool hasEdge(const std::vector<ObservedSuccessor>& edges,
                           uint64_t source, uint64_t target) {
  return std::ranges::any_of(edges, [=](const ObservedSuccessor& edge) {
    return edge.sourcePc == source && edge.targetPc == target;
  });
}

}  // namespace

TEST_CASE("v5 coverage selects invocations and removes nested callees",
          "[analysis][execution-coverage]") {
  Engine engine{"coverage-v5"};
  engine.manifest(5, 14, 3);
  engine.trace({
      0x084,  // outer callsite
      0x100, 0x104, 0x108,  // outer, then child callsite
      0x200, 0x204,         // nested child
      0x10c, 0x110, 0x114,  // outer resumes
      0x088,                 // outer return and second callsite
      0x100, 0x118, 0x11c,  // second invocation
      0x08c,                 // second return
  });
  engine.calls({
      {2, 1, 0x100, 0x200, 0x108, 4, 7, 1},
      {1, 0, 0x080, 0x100, 0x084, 1, 10, 0},
      {3, 0, 0x080, 0x100, 0x088, 10, 14, 0},
  });

  auto database = Tracedb::open(engine.root());
  REQUIRE(database);
  CoverageSelection allInvocations;
  allInvocations.functionTarget = 0x100;
  auto all = ExecutionCoverage::load(*database, allInvocations);
  REQUIRE(all);
  REQUIRE(all->invocations().size() == 2);

  const auto& first = all->invocations()[0];
  CHECK(first.callId == 1);
  CHECK(first.executedPcs ==
        std::vector<uint64_t>{0x100, 0x104, 0x108, 0x10c, 0x110, 0x114});
  CHECK(std::ranges::find(first.executedPcs, 0x200) ==
        first.executedPcs.end());
  CHECK(hasEdge(first.successors, 0x108, 0x10c));
  CHECK_FALSE(hasEdge(first.successors, 0x108, 0x200));

  CoverageSelection secondOnly;
  secondOnly.functionTarget = 0x100;
  secondOnly.ordinals = {1};
  auto selected = ExecutionCoverage::load(*database, secondOnly);
  REQUIRE(selected);
  REQUIRE(selected->invocations().size() == 1);
  CHECK(selected->invocations()[0].callId == 3);
  CHECK(selected->invocations()[0].executedPcs ==
        std::vector<uint64_t>{0x100, 0x118, 0x11c});
}

TEST_CASE("v6 coverage uses flow rows without crossing nested calls",
          "[analysis][execution-coverage]") {
  Engine engine{"coverage-v6"};
  engine.manifest(6, 10, 2, 5);
  engine.trace({0x084, 0x100, 0x104, 0x108, 0x200,
                0x204, 0x10c, 0x110, 0x114, 0x088});
  engine.calls({
      {2, 1, 0x100, 0x200, 0x108, 4, 7, 1},
      {1, 0, 0x080, 0x100, 0x084, 1, 10, 0},
  });
  engine.flow({
      {1, 0x084, 0x100, static_cast<uint64_t>(FlowKind::Call)},
      {4, 0x108, 0x200, static_cast<uint64_t>(FlowKind::Call)},
      {6, 0x204, 0x10c, static_cast<uint64_t>(FlowKind::Return)},
      {8, 0x110, 0x114, static_cast<uint64_t>(FlowKind::DirectBranch)},
      {9, 0x114, 0x088, static_cast<uint64_t>(FlowKind::Return)},
  });

  auto database = Tracedb::open(engine.root());
  REQUIRE(database);
  CoverageSelection outer;
  outer.functionTarget = 0x100;
  auto coverage = ExecutionCoverage::load(*database, outer);
  REQUIRE(coverage);
  REQUIRE(coverage->invocations().size() == 1);
  const auto& edges = coverage->invocations()[0].successors;
  CHECK(hasEdge(edges, 0x110, 0x114));
  CHECK_FALSE(hasEdge(edges, 0x108, 0x200));
  CHECK_FALSE(hasEdge(edges, 0x204, 0x10c));
  CHECK_FALSE(hasEdge(edges, 0x108, 0x10c));  // no v5 adjacency fallback
}

TEST_CASE("coverage can select a call id without a target",
          "[analysis][execution-coverage]") {
  Engine engine{"coverage-id"};
  engine.manifest(5, 3, 1);
  engine.trace({0x80, 0x500, 0x84});
  engine.calls({{42, 0, 0x40, 0x500, 0x80, 1, 3, 0}});
  auto database = Tracedb::open(engine.root());
  REQUIRE(database);
  CoverageSelection selection;
  selection.callIds = {42};
  auto coverage = ExecutionCoverage::load(*database, selection);
  REQUIRE(coverage);
  REQUIRE(coverage->invocations().size() == 1);
  CHECK(coverage->invocations()[0].executedPcs ==
        std::vector<uint64_t>{0x500});
}

TEST_CASE("coverage matches and rebases an ASLR-loaded invocation",
          "[analysis][execution-coverage]") {
  constexpr uint64_t traceBase = 0x10453c000;
  constexpr uint64_t imageBase = 0x100000000;
  Engine engine{"coverage-aslr"};
  engine.manifest(5, 4, 1);
  engine.trace({0x84, 0x100, 0x104, 0x88});
  engine.calls({{1, 0, traceBase + 0x40, traceBase + 0x100,
                 traceBase + 0x84, 1, 4, 0}});

  auto database = Tracedb::open(engine.root());
  REQUIRE(database);
  CoverageSelection selection;
  selection.functionTarget = imageBase + 0x100;
  selection.imageBase = imageBase;
  auto coverage = ExecutionCoverage::load(*database, selection);
  REQUIRE(coverage);
  REQUIRE(coverage->invocations().size() == 1);
  CHECK(coverage->invocations()[0].functionTarget == imageBase + 0x100);
  CHECK(coverage->invocations()[0].executedPcs ==
        std::vector<uint64_t>{imageBase + 0x100, imageBase + 0x104});
  CHECK(coverage->contains(imageBase + 0x100));
}
