// AddressRenderer (see the header for the dispatch this replaces).
#include "address_render.h"

#include <format>

#include "c_context.h"

namespace xdec::emit {

std::optional<AddressRenderResult> AddressRenderer::render(il::ExprId expr, AddressRole role,
                                                            uint32_t width) const {
  const analysis::AddressInfo info = ctx_.frame.classify(expr);
  switch (info.kind) {
    case analysis::AddressKind::StackSlot:
      return renderStackSlot(info.delta, role, width);
    case analysis::AddressKind::Global:
      return renderGlobal(info.address, role, width);
    case analysis::AddressKind::Other:
      if (const std::optional<analysis::AddressExpr> address =
              ctx_.addressExpressions.classify(expr)) {
        return renderPointerOffset(*address, role, width);
      }
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<AddressRenderResult> AddressRenderer::renderStackSlot(int64_t delta, AddressRole role,
                                                                     uint32_t width) const {
  if (role == AddressRole::MemoryLvalue) {
    std::string text = ctx_.stackSlotLvalue(delta, width);
    if (text.empty()) {
      return std::nullopt;
    }
    return AddressRenderResult{std::move(text), false};
  }
  // PointerValue and AddressOf both want the slot's own address, not a
  // width-bit view through it -- `&var_70`, whatever var_70 is declared as,
  // bare and uncast, matching every existing use of a recovered local's
  // address in this emitter.
  const analysis::Variable* local = ctx_.variables.localAt(delta);
  if (local == nullptr) {
    return std::nullopt;
  }
  return AddressRenderResult{std::format("&{}", local->name), true};
}

std::optional<AddressRenderResult> AddressRenderer::renderGlobal(uint64_t va, AddressRole role,
                                                                  uint32_t width) const {
  const std::string* name = ctx_.globalName(va);
  if (name == nullptr) {
    return std::nullopt;
  }
  if (role == AddressRole::MemoryLvalue) {
    // Exception: a byte access against the untyped fallback declaration
    // (globalName falls back to `uint8_t NAME[]` precisely when no header
    // binds this address) is already reading at the array's own element
    // type. `NAME[0]` says that directly; `(*(uint8_t*)(NAME))` would only
    // be undoing the very array-to-pointer decay it started from. A
    // header-bound address may not be an array at all, so this stays only
    // for the fallback declaration -- the same rule memoryLvalue always
    // applied here, before this took over printing it.
    const types::TypeBinder* binder = ctx_.binder();
    const bool headerTyped = binder != nullptr && binder->at(va).valid();
    if (width == 8 && !headerTyped) {
      return AddressRenderResult{std::format("{}[0x0]", *name), false};
    }
    return AddressRenderResult{std::format("(*({}*)({}))", intType(width), *name), false};
  }
  // PointerValue / AddressOf: the fallback declaration decays to a pointer
  // to its first byte on its own, so the name is already the address --
  // bare, the same as a header-bound global's own declared type is not this
  // renderer's business to second-guess with a cast.
  return AddressRenderResult{*name, true};
}

std::optional<AddressRenderResult> AddressRenderer::renderPointerOffset(
    const analysis::AddressExpr& address, AddressRole role,
    uint32_t width) const {
  if (address.kind != analysis::AddressBaseKind::Argument &&
      address.kind != analysis::AddressBaseKind::Value) {
    return std::nullopt;
  }

  const il::Expr& baseExpression = ctx_.function.expr(address.base);
  std::string base;
  if (baseExpression.op == il::ExprOp::EntryReg) {
    const analysis::Variable* argument = ctx_.variables.argumentFor(
        il::RegId{static_cast<uint32_t>(baseExpression.immediate)});
    if (argument == nullptr) {
      return std::nullopt;
    }
    base = ctx_.argumentName(*argument);
  } else if (baseExpression.op == il::ExprOp::Value) {
    const il::ValueId value{static_cast<uint32_t>(baseExpression.immediate)};
    if (const std::string* temporary = ctx_.tempFor(value)) {
      base = *temporary;
    } else if (const analysis::Variable* variable =
                   ctx_.variables.tempFor(value)) {
      base = variable->name;
    } else {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }

  std::string pointer = base;
  if (address.byteOffset != 0) {
    pointer = address.byteOffset > 0
                  ? std::format("((uint8_t*)({}) + {:#x})", base,
                                static_cast<uint64_t>(address.byteOffset))
                  : std::format("((uint8_t*)({}) - {:#x})", base,
                                static_cast<uint64_t>(-address.byteOffset));
  }
  if (role == AddressRole::MemoryLvalue) {
    return AddressRenderResult{
        std::format("(*({}*)({}))", intType(width), pointer), false};
  }
  if (role == AddressRole::PointerValue && address.byteOffset != 0 &&
      width != 0 && width != 8) {
    return AddressRenderResult{
        std::format("({}*)({})", intType(width), pointer), true};
  }
  return AddressRenderResult{std::move(pointer), true};
}

}  // namespace xdec::emit
