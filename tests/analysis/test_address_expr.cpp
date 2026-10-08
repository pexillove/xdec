#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/address_expr.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::AddressBaseKind;
using xdec::analysis::AddressExprAnalysis;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId reg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }
  ExprId i64(uint64_t value) {
    return function.constant(Type::integer(64), value);
  }
  ExprId add(ExprId base, uint64_t offset) {
    return function.binary(ExprOp::Add, base, i64(offset));
  }
  void markPointer(ExprId address) {
    function.appendLoad(entry, va, Type::integer(32), address);
    va += 4;
  }

  il::Function function;
  il::BlockId entry;
  uint64_t va = 0x1000;
};

TEST_CASE("address expressions preserve a pointer base and byte displacement",
          "[analysis][address-expr]") {
  Fixture f;
  const ExprId base = f.reg("x0");
  const ExprId address = f.add(
      f.function.cast(ExprOp::Bitcast, Type::integer(64), base), 0x28);
  f.markPointer(base);
  f.markPointer(address);
  f.function.appendReturn(f.entry, f.va);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  const AddressExprAnalysis addresses(f.function, variables, frame);
  const auto result = addresses.classify(address);

  REQUIRE(result.has_value());
  CHECK(result->kind == AddressBaseKind::Argument);
  CHECK(result->base == base);
  CHECK(result->byteOffset == 0x28);
}

TEST_CASE("nonconstant arithmetic is not promoted to an address",
          "[analysis][address-expr]") {
  Fixture f;
  const ExprId base = f.reg("x0");
  const ExprId proven = f.add(base, 8);
  f.markPointer(proven);
  const ExprId scaled =
      f.function.binary(ExprOp::Add, base, f.reg("x1"));
  const ExprId multiplied =
      f.function.binary(ExprOp::Mul, base, f.i64(4));
  f.function.appendReturn(f.entry, f.va);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  const AddressExprAnalysis addresses(f.function, variables, frame);
  CHECK_FALSE(addresses.classify(scaled).has_value());
  CHECK_FALSE(addresses.classify(multiplied).has_value());
}

TEST_CASE("select address provenance merges only identical bases",
          "[analysis][address-expr]") {
  Fixture f;
  const ExprId x0Address = f.add(f.reg("x0"), 8);
  const ExprId x1Address = f.add(f.reg("x1"), 8);
  f.markPointer(x0Address);
  f.markPointer(x1Address);
  const ExprId condition = f.function.binary(ExprOp::CmpEq, f.reg("x2"), f.i64(0));
  const ExprId same =
      f.function.select(condition, x0Address, x0Address);
  const ExprId conflict =
      f.function.select(condition, x0Address, x1Address);
  f.function.appendReturn(f.entry, f.va);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  const AddressExprAnalysis addresses(f.function, variables, frame);
  REQUIRE(addresses.classify(same).has_value());
  CHECK_FALSE(addresses.classify(conflict).has_value());
}

TEST_CASE("phi address provenance rejects disagreeing pointer bases",
          "[analysis][address-expr]") {
  Fixture f;
  const il::BlockId right = f.function.createBlock(0x2000);
  const il::BlockId merge = f.function.createBlock(0x3000);
  const ExprId x0Address = f.add(f.reg("x0"), 8);
  const ExprId x1Address = f.add(f.reg("x1"), 8);
  f.function.appendLoad(f.entry, 0x1000, Type::integer(32), x0Address);
  f.function.appendBranch(f.entry, 0x1004, merge);
  f.function.appendLoad(right, 0x2000, Type::integer(32), x1Address);
  f.function.appendBranch(right, 0x2004, merge);
  const ExprId incoming[] = {x0Address, x1Address};
  const il::OpId phi =
      f.function.appendPhi(merge, 0x3000, Type::integer(64), incoming);
  f.function.setOperands(phi, incoming);
  const ExprId phiValue = f.function.valueRef(f.function.op(phi).result);
  f.function.appendLoad(merge, 0x3004, Type::integer(32), phiValue);
  f.function.appendReturn(merge, 0x3008);
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  const AddressExprAnalysis addresses(f.function, variables, frame);
  CHECK_FALSE(addresses.classify(phiValue).has_value());
}

}  // namespace
