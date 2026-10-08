// Hierarchical timing spans.
//
// One mechanism for every "how long did this take" question: pass runs and
// fixpoint iterations, driver rounds and settles, emit stages, session loading.
// A span always measures itself and always sits on its thread's span stack
// (which is what the watchdog and crash dump read to say where a thread is);
// it is only *recorded* when a span sink is installed -- a Chrome trace file,
// the summary table, the run report.
//
// Spans are for coarse work (thousands per run, not millions): each costs a
// small lock on its own thread's state and, when recorded, an allocation.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "xdec/obs/field.h"
#include "xdec/obs/log.h"

namespace xdec::obs {

struct SpanRecord {
  uint64_t id = 0;
  /// 0 for a span with no enclosing span on its thread.
  uint64_t parent = 0;
  std::string_view category;
  std::string name;
  uint64_t startNs = 0;
  uint64_t durationNs = 0;
  /// Duration minus the durations of the spans directly nested inside it.
  uint64_t selfNs = 0;
  uint32_t thread = 0;
  uint64_t job = 0;
  uint32_t depth = 0;
  Fields fields;
};

class SpanSink {
 public:
  virtual ~SpanSink() = default;
  /// Any thread; implementations synchronise themselves.
  virtual void write(const SpanRecord& record) = 0;
  virtual void flush() {}
};

SinkId addSpanSink(std::shared_ptr<SpanSink> sink);
void removeSpanSink(SinkId id);
void clearSpanSinks();
void flushSpanSinks();

/// Routes a finished span to the process-wide span sinks and the current job's.
void dispatch(const SpanRecord& record);

class Span {
 public:
  /// `category` must be a string literal (it outlives the span in sinks).
  Span(std::string_view category, std::string name, Fields fields = {});
  ~Span();

  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  /// Adds or replaces an attribute; typically a size measured at the end.
  void set(std::string_view key, FieldValue value);
  [[nodiscard]] std::chrono::nanoseconds elapsed() const noexcept;
  [[nodiscard]] uint64_t id() const noexcept { return id_; }
  /// Ends the span now rather than at scope exit. Idempotent.
  void end();

 private:
  std::string_view category_;
  std::string name_;
  Fields fields_;
  uint64_t id_ = 0;
  uint64_t parent_ = 0;
  uint64_t startNs_ = 0;
  uint32_t depth_ = 0;
  bool ended_ = false;
};

}  // namespace xdec::obs

/// XDEC_SPAN(span, "driver", "round", "round", r) -- a named local span with
/// optional key/value attributes.
#define XDEC_SPAN(var, category, name, ...) \
  ::xdec::obs::Span var { category, name, ::xdec::obs::kv(__VA_ARGS__) }
