#include "xdec/exec/trace.h"

namespace xdec::exec {

std::string_view toString(BoundaryKind kind) noexcept {
  switch (kind) {
    case BoundaryKind::DirectCall: return "direct-call";
    case BoundaryKind::IndirectCall: return "indirect-call";
    case BoundaryKind::Syscall: return "syscall";
    case BoundaryKind::Intrinsic: return "intrinsic";
    case BoundaryKind::PageRequest: return "page-request";
  }
  return "?";
}

void CallbackTraceSink::onInstruction(const InstructionRecord& record) {
  if (instruction_) {
    instruction_(record);
  }
}

void CallbackTraceSink::onBoundary(const BoundaryRecord& record) {
  if (boundary_) {
    boundary_(record);
  }
}

void CallbackTraceSink::onBoundaryEffect(const BoundaryEffectRecord& record) {
  if (effect_) {
    effect_(record);
  }
}

}  // namespace xdec::exec
