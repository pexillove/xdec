// Address provenance for scalar IL expressions.
//
// IL deliberately models addresses as machine integers. This analysis keeps
// the separate, presentation-level fact that an expression is a byte
// displacement from a proven pointer base, without changing the IL type
// system or teaching arithmetic passes about C pointers.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/il/function.h"
#include "xdec/types/type.h"

namespace xdec::types {
class TypeDatabase;
}

namespace xdec::analysis {

class TypedVariables;

enum class AddressBaseKind : uint8_t {
  StackSlot,
  Global,
  Argument,
  Value,
};

enum class AddressEvidence : uint8_t {
  Structural,
  InferredPointer,
  ImportedType,
};

struct AddressExpr {
  AddressBaseKind kind = AddressBaseKind::Global;
  /// Argument/Value: the leaf expression carrying the pointer.
  il::ExprId base;
  /// StackSlot: displacement from entry SP. Global: absolute VA.
  /// Argument/Value: byte displacement from `base`.
  int64_t byteOffset = 0;
  /// Imported pointer type when one is known. Invalid means only the inferred
  /// pointer shape is known.
  types::TypeId pointerType;
  AddressEvidence evidence = AddressEvidence::Structural;
};

class AddressExprAnalysis {
 public:
  AddressExprAnalysis(const il::Function& function, const VariableTable& variables,
                      const StackFrame& frame, const TypedVariables* typed = nullptr,
                      const types::TypeDatabase* database = nullptr)
      : function_(function),
        variables_(variables),
        frame_(frame),
        typed_(typed),
        database_(database) {}

  [[nodiscard]] std::optional<AddressExpr> classify(il::ExprId expression) const;

 private:
  [[nodiscard]] std::optional<AddressExpr> classifyImpl(
      il::ExprId expression, unsigned depth,
      std::unordered_set<uint32_t>& activeValues) const;
  [[nodiscard]] bool isPointerType(types::TypeId type) const;
  [[nodiscard]] types::TypeId argumentPointerType(il::RegId root) const;
  [[nodiscard]] types::TypeId valuePointerType(il::ValueId value) const;

  const il::Function& function_;
  const VariableTable& variables_;
  const StackFrame& frame_;
  const TypedVariables* typed_;
  const types::TypeDatabase* database_;
  mutable std::unordered_map<uint32_t, std::optional<AddressExpr>> cache_;
  static constexpr unsigned kMaxDepth = 64;
};

}  // namespace xdec::analysis
