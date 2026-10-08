#include "xdec/obs/threads.h"

#include <algorithm>
#include <format>

#include "thread_state.h"
#include "xdec/obs/clock.h"

namespace xdec::obs {
namespace detail {
namespace {

struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<ThreadState>> threads;
};

Registry& registry() {
  static Registry* instance = new Registry;  // outlives thread_local destructors
  return *instance;
}

/// Registers on construction, unregisters when the thread exits.
struct Holder {
  std::shared_ptr<ThreadState> state = std::make_shared<ThreadState>();

  Holder() {
    state->index = threadIndex();
    Registry& reg = registry();
    const std::lock_guard lock{reg.mutex};
    reg.threads.push_back(state);
  }
  ~Holder() {
    Registry& reg = registry();
    const std::lock_guard lock{reg.mutex};
    std::erase(reg.threads, state);
  }
};

}  // namespace

ThreadState& threadState() {
  thread_local Holder holder;
  return *holder.state;
}

std::vector<std::shared_ptr<ThreadState>> allThreadStates() {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  return reg.threads;
}

}  // namespace detail

std::vector<ThreadSnapshot> threadSnapshots(bool tryOnly) {
  std::vector<ThreadSnapshot> out;
  std::vector<std::shared_ptr<detail::ThreadState>> states;
  if (tryOnly) {
    detail::Registry& reg = detail::registry();
    std::unique_lock lock{reg.mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
      return out;
    }
    states = reg.threads;
  } else {
    states = detail::allThreadStates();
  }
  for (const auto& state : states) {
    std::unique_lock lock{state->mutex, std::defer_lock};
    if (tryOnly) {
      if (!lock.try_lock()) {
        continue;
      }
    } else {
      lock.lock();
    }
    if (state->spans.empty() && state->context.empty()) {
      continue;
    }
    ThreadSnapshot snapshot;
    snapshot.thread = state->index;
    snapshot.job = state->job;
    snapshot.context = state->context;
    for (const detail::OpenSpan& span : state->spans) {
      snapshot.spans.push_back(SpanFrame{span.category, span.name, span.startNs});
    }
    out.push_back(std::move(snapshot));
  }
  std::ranges::sort(out, {}, &ThreadSnapshot::thread);
  return out;
}

std::string formatThreadSnapshots(const std::vector<ThreadSnapshot>& threads, uint64_t nowNs) {
  std::string out;
  for (const ThreadSnapshot& thread : threads) {
    out += std::format("  thread {} (job {})", thread.thread, thread.job);
    if (!thread.context.empty()) {
      out += "  context:";
      for (const Field& field : thread.context) {
        out += std::format(" {}={}", field.key, field.value.text());
      }
    }
    out += '\n';
    for (std::size_t i = 0; i < thread.spans.size(); ++i) {
      const SpanFrame& span = thread.spans[i];
      const double ageSeconds =
          nowNs > span.startNs ? static_cast<double>(nowNs - span.startNs) / 1e9 : 0.0;
      out += std::format("    {:>{}}{} {} (open {:.3f}s)\n", "", i * 2, span.category, span.name,
                         ageSeconds);
    }
  }
  if (out.empty()) {
    out = "  (no instrumented thread has open spans)\n";
  }
  return out;
}

}  // namespace xdec::obs
