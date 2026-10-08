#include "xdec/analysis/address_expr.h"

#include "xdec/analysis/typed_variables.h"
#include "xdec/types/database.h"

namespace xdec::analysis {

namespace {

bool sameBase(const AddressExpr& lhs, const AddressExpr& rhs) {
  return lhs.kind == rhs.kind && lhs.base == rhs.base &&
         lhs.byteOffset == rhs.byteOffset;
}

}  // namespace

bool AddressExprAnalysis::isPointerType(types::TypeId type) const {
  if (!type.valid() || database_ == nullptr) {
    return false;
  }
  const types::TypeEntry* entry = database_->get(database_->resolveTypedef(type));
  return entry != nullptr &&
         (entry->kind == types::TypeKind::Pointer ||
          entry->kind == types::TypeKind::Array);
}

types::TypeId AddressExprAnalysis::argumentPointerType(il::RegId root) const {
  if (const Variable* argument = variables_.argumentFor(root);
      argument != nullptr && argument->importedType.has_value() &&
      isPointerType(*argument->importedType)) {
    return *argument->importedType;
  }
  if (typed_ != nullptr) {
    if (const std::optional<types::TypeId> type = typed_->forArgument(root);
        type.has_value() && isPointerType(*type)) {
      return *type;
    }
  }
  return {};
}

types::TypeId AddressExprAnalysis::valuePointerType(il::ValueId value) const {
  if (const Variable* temporary = variables_.tempFor(value);
      temporary != nullptr && temporary->importedType.has_value() &&
      isPointerType(*temporary->importedType)) {
    return *temporary->importedType;
  }
  if (typed_ != nullptr) {
    if (const std::optional<types::TypeId> type = typed_->forValue(value);
        type.has_value() && isPointerType(*type)) {
      return *type;
    }
  }
  return {};
}

std::optional<AddressExpr> AddressExprAnalysis::classify(
    il::ExprId expression) const {
  if (!expression.valid()) {
    return std::nullopt;
  }
  if (const auto found = cache_.find(expression.index());
      found != cache_.end()) {
    return found->second;
  }
  std::unordered_set<uint32_t> activeValues;
  std::optional<AddressExpr> result =
      classifyImpl(expression, 0, activeValues);
  cache_.emplace(expression.index(), result);
  return result;
}

std::optional<AddressExpr> AddressExprAnalysis::classifyImpl(
    il::ExprId expression, unsigned depth,
    std::unordered_set<uint32_t>& activeValues) const {
  if (!expression.valid() || depth > kMaxDepth) {
    return std::nullopt;
  }

  const AddressInfo simple = frame_.classify(expression);
  if (simple.kind == AddressKind::StackSlot) {
    return AddressExpr{AddressBaseKind::StackSlot, {}, simple.delta, {}};
  }
  if (simple.kind == AddressKind::Global) {
    return AddressExpr{AddressBaseKind::Global, {},
                       static_cast<int64_t>(simple.address), {}};
  }

  const il::Expr expressionValue = function_.expr(expression);
  switch (expressionValue.op) {
    case il::ExprOp::ZExt:
    case il::ExprOp::Trunc:
    case il::ExprOp::Bitcast:
      return classifyImpl(expressionValue.operand(0), depth + 1,
                          activeValues);
    case il::ExprOp::Add:
    case il::ExprOp::Sub: {
      uint64_t constant = 0;
      std::optional<AddressExpr> base;
      int64_t displacement = 0;
      if (function_.asConstant(expressionValue.operand(1), constant)) {
        base = classifyImpl(expressionValue.operand(0), depth + 1,
                            activeValues);
        displacement = static_cast<int64_t>(constant);
        if (expressionValue.op == il::ExprOp::Sub) {
          displacement = -displacement;
        }
      } else if (expressionValue.op == il::ExprOp::Add &&
                 function_.asConstant(expressionValue.operand(0), constant)) {
        base = classifyImpl(expressionValue.operand(1), depth + 1,
                            activeValues);
        displacement = static_cast<int64_t>(constant);
      }
      if (!base.has_value()) {
        return std::nullopt;
      }
      base->byteOffset += displacement;
      return base;
    }
    case il::ExprOp::EntryReg: {
      const il::RegId root{
          static_cast<uint32_t>(expressionValue.immediate)};
      const Variable* argument = variables_.argumentFor(root);
      const types::TypeId pointerType = argumentPointerType(root);
      if ((argument == nullptr || argument->type.pointerDepth == 0) &&
          !pointerType.valid()) {
        return std::nullopt;
      }
      return AddressExpr{
          AddressBaseKind::Argument, expression, 0, pointerType,
          pointerType.valid() ? AddressEvidence::ImportedType
                              : AddressEvidence::InferredPointer};
    }
    case il::ExprOp::Value: {
      const il::ValueId value{
          static_cast<uint32_t>(expressionValue.immediate)};
      if (!function_.hasValue(value) ||
          !activeValues.insert(value.index()).second) {
        return std::nullopt;
      }
      const il::OpId definitionId = function_.value(value).definition;
      if (!definitionId.valid()) {
        activeValues.erase(value.index());
        return std::nullopt;
      }
      const il::Op& definition = function_.op(definitionId);
      if (definition.code == il::OpCode::Phi) {
        std::optional<AddressExpr> merged;
        for (const il::ExprId incoming : function_.operands(definition)) {
          std::optional<AddressExpr> candidate =
              classifyImpl(incoming, depth + 1, activeValues);
          if (!candidate.has_value()) {
            activeValues.erase(value.index());
            return std::nullopt;
          }
          if (!merged.has_value()) {
            merged = candidate;
          } else if (!sameBase(*merged, *candidate)) {
            activeValues.erase(value.index());
            return std::nullopt;
          } else if (merged->pointerType != candidate->pointerType) {
            merged->pointerType = {};
            merged->evidence = AddressEvidence::InferredPointer;
          }
        }
        activeValues.erase(value.index());
        return merged;
      }
      activeValues.erase(value.index());
      const Variable* temporary = variables_.tempFor(value);
      const types::TypeId pointerType = valuePointerType(value);
      if ((temporary == nullptr || temporary->type.pointerDepth == 0) &&
          !pointerType.valid()) {
        return std::nullopt;
      }
      return AddressExpr{
          AddressBaseKind::Value, expression, 0, pointerType,
          pointerType.valid() ? AddressEvidence::ImportedType
                              : AddressEvidence::InferredPointer};
    }
    case il::ExprOp::Select: {
      std::optional<AddressExpr> whenTrue = classifyImpl(
          expressionValue.operand(1), depth + 1, activeValues);
      std::optional<AddressExpr> whenFalse = classifyImpl(
          expressionValue.operand(2), depth + 1, activeValues);
      if (!whenTrue.has_value() || !whenFalse.has_value() ||
          !sameBase(*whenTrue, *whenFalse)) {
        return std::nullopt;
      }
      if (whenTrue->pointerType != whenFalse->pointerType) {
        whenTrue->pointerType = {};
        whenTrue->evidence = AddressEvidence::InferredPointer;
      }
      return whenTrue;
    }
    default:
      return std::nullopt;
  }
}

}  // namespace xdec::analysis
