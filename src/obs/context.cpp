#include "xdec/obs/context.h"

#include <mutex>

#include "thread_state.h"

namespace xdec::obs {

ScopedContext::ScopedContext(std::string_view key, FieldValue value) {
  detail::ThreadState& state = detail::threadState();
  const std::lock_guard lock{state.mutex};
  state.context.push_back(Field{key, std::move(value)});
}

ScopedContext::~ScopedContext() {
  detail::ThreadState& state = detail::threadState();
  const std::lock_guard lock{state.mutex};
  if (!state.context.empty()) {
    state.context.pop_back();
  }
}

Fields contextSnapshot() {
  detail::ThreadState& state = detail::threadState();
  const std::lock_guard lock{state.mutex};
  return state.context;
}

}  // namespace xdec::obs
