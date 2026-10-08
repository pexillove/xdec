// isRegisterPreservingHelper / callTargetName (see the header for which
// contracts are listed and why the list stays short).
#include "xdec/analysis/call_abi.h"

#include <array>

#include "xdec/analysis/plt_stub.h"

namespace xdec::analysis {

namespace {

constexpr std::array<std::string_view, 2> kRegisterPreservingHelpers = {
    "__chkstk_darwin",
    "__chkstk",
};

[[nodiscard]] il::ExprId rewritten(const CallNameSources& sources, il::ExprId id) {
  return sources.rewrite ? sources.rewrite(id) : id;
}

[[nodiscard]] bool foldAddress(const il::Function& function, il::ExprId id, uint64_t& out,
                               unsigned depth) {
  if (depth > 8 || !id.valid()) {
    return false;
  }
  if (function.asConstant(id, out)) {
    return true;
  }
  const il::Expr& expr = function.expr(id);
  switch (expr.op) {
    case il::ExprOp::ZExt:
    case il::ExprOp::SExt:
    case il::ExprOp::Trunc:
    case il::ExprOp::Bitcast:
      return foldAddress(function, expr.operand(0), out, depth + 1);
    case il::ExprOp::Add:
    case il::ExprOp::Sub: {
      uint64_t lhs = 0;
      uint64_t rhs = 0;
      if (!foldAddress(function, expr.operand(0), lhs, depth + 1) ||
          !foldAddress(function, expr.operand(1), rhs, depth + 1)) {
        return false;
      }
      out = expr.op == il::ExprOp::Add ? lhs + rhs : lhs - rhs;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

bool isRegisterPreservingHelper(std::string_view name) noexcept {
  for (const std::string_view helper : kRegisterPreservingHelpers) {
    if (name == helper || (name.size() == helper.size() + 1 && name.front() == '_' &&
                           name.substr(1) == helper)) {
      return true;
    }
  }
  return false;
}

bool registerPreservingHelperClobbers(std::string_view name) noexcept {
  return name == "x16" || name == "x17" || name == "x30";
}

std::optional<std::string> callTargetName(const il::Function& function, il::ExprId target,
                                          const CallNameSources& sources) {
  const il::ExprId resolved = rewritten(sources, target);
  uint64_t address = 0;
  if (foldAddress(function, resolved, address, 0)) {
    if (sources.symbolAt) {
      if (std::string name = sources.symbolAt(address); !name.empty()) {
        return name;
      }
    }
    if (sources.image != nullptr && sources.memory != nullptr) {
      return importNameForPltStub(address, *sources.image, *sources.memory);
    }
    return std::nullopt;
  }
  if (sources.memory == nullptr) {
    return std::nullopt;
  }
  const il::Expr& expr = function.expr(resolved);
  if (expr.op != il::ExprOp::Value) {
    return std::nullopt;
  }
  const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
  if (!function.hasValue(value)) {
    return std::nullopt;
  }
  const il::OpId definition = function.value(value).definition;
  if (!definition.valid() || function.op(definition).code != il::OpCode::Load) {
    return std::nullopt;
  }
  const auto operands = function.operands(function.op(definition));
  uint64_t slot = 0;
  if (operands.empty() ||
      !foldAddress(function, rewritten(sources, operands[0]), slot, 0)) {
    return std::nullopt;
  }
  std::string name = sources.memory->loaderValueAt(slot).importName;
  if (name.empty()) {
    return std::nullopt;
  }
  return name;
}

}  // namespace xdec::analysis
