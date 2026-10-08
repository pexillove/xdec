#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <vector>

#include "storage/tracedb_test_support.h"
#include "xdec/storage/tracedb.h"

using xdec::storage::FlowKind;
using xdec::storage::Tracedb;
using xdec::test::tracedb::Engine;

TEST_CASE("tracedb v5 reads trace callgraph and dictionaries",
          "[storage][tracedb]") {
  Engine engine{"v5-reader"};
  engine.manifest(5, 3, 1);
  engine.trace({0x100, 0x104, 0x108});
  // id, parent, caller, callee, callsite, enter, exit, depth|flags<<32
  engine.calls({{7, 0, 0x80, 0x100, 0x84, 1, 3, 0}});

  std::ofstream dictionary{engine.root() / "dicts" / "mnemonic.dict",
                           std::ios::binary};
  std::array<uint8_t, 12> header{};
  xdec::test::tracedb::put32(header.data(), 0x43494454);
  xdec::test::tracedb::put32(header.data() + 4, 1);
  dictionary.write(reinterpret_cast<const char*>(header.data()), header.size());
  std::array<uint8_t, 4> length{};
  xdec::test::tracedb::put32(length.data(), 3);
  dictionary.write(reinterpret_cast<const char*>(length.data()), length.size());
  dictionary.write("add", 3);
  dictionary.close();

  auto opened = Tracedb::open(engine.root());
  REQUIRE(opened);
  CHECK(opened->version() == 5);
  REQUIRE(opened->threads().size() == 1);
  CHECK(opened->threads()[0].id == 1);

  auto trace = opened->readTrace(1);
  REQUIRE(trace);
  REQUIRE(trace->size() == 3);
  CHECK((*trace)[1].traceId == 2);
  CHECK((*trace)[1].pc == 0x104);
  CHECK((*trace)[1].globalEventId == 101);

  auto calls = opened->readCallGraph(1);
  REQUIRE(calls);
  REQUIRE(calls->size() == 1);
  CHECK((*calls)[0].callId == 7);
  CHECK((*calls)[0].calleeTarget == 0x100);

  auto names = opened->readDictionary("mnemonic.dict");
  REQUIRE(names);
  REQUIRE(names->size() == 1);
  CHECK((*names)[0] == "add");

  auto flow = opened->readFlow(1);
  REQUIRE(flow);
  CHECK(flow->empty());
}

TEST_CASE("tracedb v6 decodes the authoritative flow stream",
          "[storage][tracedb]") {
  Engine engine{"v6-flow"};
  engine.manifest(6, 2, 0, 1);
  engine.trace({0x200, 0x204});
  engine.calls({});
  engine.flow({{1, 0x200, 0x204,
                static_cast<uint64_t>(FlowKind::DirectBranch)}});

  auto opened = Tracedb::open(engine.root());
  REQUIRE(opened);
  auto rows = opened->readFlow(1);
  REQUIRE(rows);
  REQUIRE(rows->size() == 1);
  CHECK((*rows)[0].traceId == 1);
  CHECK((*rows)[0].sourcePc == 0x200);
  CHECK((*rows)[0].targetPc == 0x204);
  CHECK((*rows)[0].globalEventId == 1);
  CHECK((*rows)[0].kind == FlowKind::DirectBranch);
}

TEST_CASE("tracedb rejects unsupported manifests and partial rows",
          "[storage][tracedb]") {
  SECTION("future manifest") {
    Engine engine{"future"};
    engine.manifest(7, 0, 0);
    auto opened = Tracedb::open(engine.root());
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code() == xdec::DiagCode::UnsupportedFormat);
  }

  SECTION("partial trace row") {
    Engine engine{"partial"};
    engine.manifest(5, 1, 0);
    std::ofstream out{engine.segment() / "trace.row", std::ios::binary};
    out.put('\0');
    out.close();
    auto opened = Tracedb::open(engine.root());
    REQUIRE(opened);
    auto rows = opened->readTrace(1);
    REQUIRE_FALSE(rows);
    CHECK(rows.error().code() == xdec::DiagCode::BadFormat);
  }

  SECTION("manifest id range disagrees with rows") {
    Engine engine{"range"};
    engine.manifest(5, 2, 0);
    engine.trace({0x100});
    auto opened = Tracedb::open(engine.root());
    REQUIRE(opened);
    auto rows = opened->readTrace(1);
    REQUIRE_FALSE(rows);
    CHECK(rows.error().code() == xdec::DiagCode::BadFormat);
  }

  SECTION("unknown v6 flow fields") {
    Engine engine{"flow-reserved"};
    engine.manifest(6, 1, 0, 1);
    engine.trace({0x300});
    engine.calls({});
    engine.flow({{1, 0x300, 0x304, 1}});
    std::fstream out{engine.segment() / "flow.row",
                     std::ios::binary | std::ios::in | std::ios::out};
    out.seekp(39);
    out.put('\1');
    out.close();
    auto opened = Tracedb::open(engine.root());
    REQUIRE(opened);
    auto rows = opened->readFlow(1);
    REQUIRE_FALSE(rows);
    CHECK(rows.error().code() == xdec::DiagCode::UnsupportedFormat);
  }
}
