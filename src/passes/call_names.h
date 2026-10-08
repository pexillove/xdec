// The pass context's view of where a call target's name comes from, for
// analysis::callTargetName.
#pragma once

#include "xdec/analysis/call_abi.h"
#include "xdec/pass/pass.h"

namespace xdec::passes {

/// Borrows from `context`, which must outlive the result.
[[nodiscard]] inline analysis::CallNameSources callNameSources(const pass::Context& context) {
  analysis::CallNameSources sources;
  sources.image = context.image();
  sources.memory = &context.memoryFacts();
  sources.symbolAt = [&context](uint64_t va) {
    const pass::SymbolName symbol = context.nameAt(va);
    return symbol.isFunction ? symbol.name : std::string{};
  };
  return sources;
}

}  // namespace xdec::passes
