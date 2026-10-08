#include "xdec/obs/span.h"

#include <algorithm>
#include <atomic>

#include "sink_list.h"
#include "thread_state.h"
#include "xdec/obs/clock.h"
#include "xdec/obs/job.h"

namespace xdec::obs {
namespace {

detail::SinkList<SpanSink>& sinks() {
  static auto* instance = new detail::SinkList<SpanSink>;
  return *instance;
}

std::atomic<uint64_t> gNextSpanId{1};

}  // namespace

SinkId addSpanSink(std::shared_ptr<SpanSink> sink) { return sinks().add(std::move(sink)); }
void removeSpanSink(SinkId id) { sinks().remove(id); }
void clearSpanSinks() { sinks().clear(); }
void flushSpanSinks() {
  for (const auto& entry : *sinks().snapshot()) {
    entry.sink->flush();
  }
}

void dispatch(const SpanRecord& record) {
  if (!sinks().empty()) {
    for (const auto& entry : *sinks().snapshot()) {
      entry.sink->write(record);
    }
  }
  if (const Job* job = Job::current(); job != nullptr && job->hasSpanSinks()) {
    job->deliver(record);
  }
}

Span::Span(std::string_view category, std::string name, Fields fields)
    : category_(category), name_(std::move(name)), fields_(std::move(fields)) {
  id_ = gNextSpanId.fetch_add(1, std::memory_order_relaxed);
  startNs_ = monotonicNs();
  detail::ThreadState& state = detail::threadState();
  {
    const std::lock_guard lock{state.mutex};
    parent_ = state.spans.empty() ? 0 : state.spans.back().id;
    depth_ = static_cast<uint32_t>(state.spans.size());
    state.spans.push_back(detail::OpenSpan{id_, category_, name_, startNs_, 0});
  }
  if (Job* job = Job::current(); job != nullptr) {
    job->touch();
  }
}

Span::~Span() { end(); }

void Span::set(std::string_view key, FieldValue value) {
  for (Field& field : fields_) {
    if (field.key == key) {
      field.value = std::move(value);
      return;
    }
  }
  fields_.push_back(Field{key, std::move(value)});
}

std::chrono::nanoseconds Span::elapsed() const noexcept {
  return std::chrono::nanoseconds{static_cast<int64_t>(monotonicNs() - startNs_)};
}

void Span::end() {
  if (ended_) {
    return;
  }
  ended_ = true;
  const uint64_t endNs = monotonicNs();
  const uint64_t duration = endNs - startNs_;
  uint64_t childNs = 0;
  detail::ThreadState& state = detail::threadState();
  {
    const std::lock_guard lock{state.mutex};
    const auto it = std::ranges::find(state.spans, id_, &detail::OpenSpan::id);
    if (it != state.spans.end()) {
      childNs = it->childNs;
      state.spans.erase(it);
    }
    if (!state.spans.empty() && state.spans.back().id == parent_) {
      state.spans.back().childNs += duration;
    }
  }
  Job* job = Job::current();
  if (job != nullptr) {
    job->touch();
  }
  if (sinks().empty() && (job == nullptr || !job->hasSpanSinks())) {
    return;
  }
  SpanRecord record;
  record.id = id_;
  record.parent = parent_;
  record.category = category_;
  record.name = std::move(name_);
  record.startNs = startNs_;
  record.durationNs = duration;
  record.selfNs = duration > childNs ? duration - childNs : 0;
  record.thread = threadIndex();
  record.job = job != nullptr ? job->id() : 0;
  record.depth = depth_;
  record.fields = std::move(fields_);
  dispatch(record);
}

}  // namespace xdec::obs
