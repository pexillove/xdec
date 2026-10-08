// Spans, counters, jobs and the recorders built on them.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xdec/obs/clock.h"
#include "xdec/obs/job.h"
#include "xdec/obs/metrics.h"
#include "xdec/obs/recorder.h"
#include "xdec/obs/sinks.h"
#include "xdec/obs/span.h"

namespace obs = xdec::obs;
using namespace std::chrono_literals;

namespace {

XDEC_DEFINE_COUNTER(testCounter, "test.counter")
XDEC_DEFINE_LOG_CATEGORY(testGamma, "test.gamma")

class SpanCapture final : public obs::SpanSink {
 public:
  void write(const obs::SpanRecord& record) override {
    const std::lock_guard lock{mutex};
    records.push_back(record);
  }
  std::mutex mutex;
  std::vector<obs::SpanRecord> records;
};

class ProgressCapture final : public obs::ProgressSink {
 public:
  void progress(const obs::Job&, const obs::ProgressEvent& event) override {
    events.push_back(event);
  }
  std::vector<obs::ProgressEvent> events;
};

void spin(std::chrono::nanoseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
  }
}

}  // namespace

TEST_CASE("durations parse with and without units", "[obs][clock]") {
  std::chrono::nanoseconds out{};
  REQUIRE(obs::parseDuration("250ms", out));
  CHECK(out == 250ms);
  REQUIRE(obs::parseDuration("3s", out));
  CHECK(out == 3s);
  REQUIRE(obs::parseDuration("2m", out));
  CHECK(out == 120s);
  REQUIRE(obs::parseDuration("1h", out));
  CHECK(out == 3600s);
  REQUIRE(obs::parseDuration("45", out));
  CHECK(out == 45s);
  CHECK_FALSE(obs::parseDuration("", out));
  CHECK_FALSE(obs::parseDuration("ten", out));
  CHECK_FALSE(obs::parseDuration("5parsecs", out));
}

TEST_CASE("spans nest and charge child time to the parent", "[obs][span]") {
  auto capture = std::make_shared<SpanCapture>();
  const obs::SinkId id = obs::addSpanSink(capture);
  {
    obs::Span outer{"test", "outer"};
    spin(2ms);
    {
      obs::Span inner{"test", "inner", obs::kv("n", 1)};
      spin(5ms);
    }
    spin(2ms);
  }
  obs::removeSpanSink(id);

  REQUIRE(capture->records.size() == 2);
  const obs::SpanRecord& inner = capture->records[0];
  const obs::SpanRecord& outer = capture->records[1];
  CHECK(inner.name == "inner");
  CHECK(outer.name == "outer");
  CHECK(inner.parent == outer.id);
  CHECK(outer.parent == 0);
  CHECK(inner.depth == outer.depth + 1);
  CHECK(inner.selfNs == inner.durationNs);
  CHECK(outer.durationNs >= inner.durationNs);
  CHECK(outer.selfNs == outer.durationNs - inner.durationNs);
  CHECK(outer.selfNs >= 3'000'000);
  REQUIRE(inner.fields.size() == 1);
  CHECK(inner.fields[0].key == "n");
}

TEST_CASE("an early end records once and later fields are dropped", "[obs][span]") {
  auto capture = std::make_shared<SpanCapture>();
  const obs::SinkId id = obs::addSpanSink(capture);
  {
    obs::Span span{"test", "early"};
    span.set("k", 1);
    span.set("k", 2);
    span.end();
    span.set("late", 3);
  }
  obs::removeSpanSink(id);
  REQUIRE(capture->records.size() == 1);
  REQUIRE(capture->records[0].fields.size() == 1);
  CHECK(capture->records[0].fields[0].value.text() == "2");
}

TEST_CASE("job-local span sinks see only spans closed under the job", "[obs][span][job]") {
  auto capture = std::make_shared<SpanCapture>();
  obs::Job job{"spans"};
  job.addSpanSink(capture);
  CHECK(job.hasSpanSinks());
  { obs::Span outside{"test", "outside"}; }
  {
    obs::JobScope scope{job};
    obs::Span inside{"test", "inside"};
  }
  REQUIRE(capture->records.size() == 1);
  CHECK(capture->records[0].name == "inside");
  CHECK(capture->records[0].job == job.id());
}

TEST_CASE("the span summary aggregates by category and name", "[obs][span][sinks]") {
  obs::SpanSummarySink summary;
  obs::SpanRecord record;
  record.category = "pass";
  record.name = "dce";
  record.durationNs = 10;
  record.selfNs = 4;
  summary.write(record);
  record.durationNs = 30;
  record.selfNs = 30;
  summary.write(record);
  record.name = "sccp";
  summary.write(record);

  const auto stats = summary.stats();
  REQUIRE(stats.size() == 2);
  bool sawDce = false;
  for (const auto& stat : stats) {
    if (stat.name == "dce") {
      sawDce = true;
      CHECK(stat.count == 2);
      CHECK(stat.totalNs == 40);
      CHECK(stat.selfNs == 34);
      CHECK(stat.maxNs == 30);
    }
  }
  CHECK(sawDce);
  CHECK(summary.format().find("dce") != std::string::npos);
}

TEST_CASE("counters register once and snapshot by name", "[obs][metrics]") {
  testCounter().reset();
  testCounter().add();
  testCounter().add(4);
  bool found = false;
  for (const auto& [name, value] : obs::counterSnapshot()) {
    if (name == "test.counter") {
      found = true;
      CHECK(value == 5);
    }
  }
  CHECK(found);
  obs::resetCounters();
  CHECK(testCounter().value() == 0);
}

TEST_CASE("process memory is reported", "[obs][metrics]") {
  const obs::MemoryUsage usage = obs::processMemory();
  CHECK(usage.currentBytes > 0);
  CHECK(usage.peakBytes >= usage.currentBytes);
}

TEST_CASE("checkpoint is a no-op without a job", "[obs][job]") {
  CHECK(obs::Job::current() == nullptr);
  CHECK_NOTHROW(obs::checkpoint("free"));
  CHECK_FALSE(obs::windDownRequested());
}

TEST_CASE("job scopes nest and restore the previous job", "[obs][job]") {
  obs::Job outer{"outer"};
  obs::Job inner{"inner"};
  CHECK(outer.id() != inner.id());
  {
    obs::JobScope a{outer};
    CHECK(obs::Job::current() == &outer);
    {
      obs::JobScope b{inner};
      CHECK(obs::Job::current() == &inner);
    }
    CHECK(obs::Job::current() == &outer);
  }
  CHECK(obs::Job::current() == nullptr);
}

TEST_CASE("cancelling a job makes the next checkpoint throw", "[obs][job]") {
  obs::Job job{"cancel"};
  obs::JobScope scope{job};
  CHECK_NOTHROW(obs::checkpoint("before"));
  job.requestCancel();
  CHECK(job.stopReason() == obs::StopReason::Cancelled);
  CHECK(job.windDownReason() == "cancel");
  try {
    obs::checkpoint("here");
    FAIL("checkpoint did not throw");
  } catch (const obs::CancelledError& error) {
    CHECK(error.reason() == obs::StopReason::Cancelled);
    CHECK(error.where() == "here");
  }
}

TEST_CASE("a soft deadline winds down without stopping", "[obs][job]") {
  obs::Job job{"soft"};
  obs::JobScope scope{job};
  job.setSoftDeadline(1ns);
  spin(1ms);
  CHECK(obs::windDownRequested());
  CHECK(job.windDownReason() == "deadline");
  CHECK(job.stopReason() == obs::StopReason::None);
  CHECK_NOTHROW(obs::checkpoint("soft"));
}

TEST_CASE("a hard deadline stops with a timeout", "[obs][job]") {
  obs::Job job{"hard"};
  obs::JobScope scope{job};
  job.setHardDeadline(1ns);
  spin(1ms);
  CHECK(job.stopReason() == obs::StopReason::Timeout);
  CHECK(job.windDownRequested());
  CHECK_THROWS_AS(obs::checkpoint("hard"), obs::CancelledError);
}

TEST_CASE("a zero deadline means none", "[obs][job]") {
  obs::Job job{"none"};
  job.setSoftDeadline(0ns);
  job.setHardDeadline(0ns);
  CHECK_FALSE(job.windDownRequested());
  CHECK(job.stopReason() == obs::StopReason::None);
}

TEST_CASE("interrupts escalate from wind-down to cancel", "[obs][job]") {
  obs::Job job{"interrupt"};
  job.interrupt();
  CHECK(job.windDownReason() == "interrupt");
  CHECK(job.stopReason() == obs::StopReason::None);
  job.interrupt();
  CHECK(job.interruptCount() == 2);
  CHECK(job.stopReason() == obs::StopReason::Cancelled);
}

TEST_CASE("progress reaches the job's sinks and marks activity", "[obs][job]") {
  auto capture = std::make_shared<ProgressCapture>();
  obs::Job job{"progress"};
  job.addProgressSink(capture);
  obs::reportProgress("free", "no job");
  CHECK(capture->events.empty());

  const uint64_t before = job.lastActivityNs();
  spin(1ms);
  {
    obs::JobScope scope{job};
    obs::reportProgress("pipeline", "pass 2/5 dce", 2, 5);
  }
  REQUIRE(capture->events.size() == 1);
  CHECK(capture->events[0].phase == "pipeline");
  CHECK(capture->events[0].current == 2);
  CHECK(capture->events[0].total == 5);
  CHECK(job.lastProgress().detail == "pass 2/5 dce");
  CHECK(job.lastActivityNs() > before);
}

TEST_CASE("the run recorder keeps warnings and top-level spans", "[obs][recorder]") {
  obs::RunRecorder recorder{obs::LogLevel::Warn, 2};
  auto logAt = [&](obs::LogLevel level, std::string message) {
    obs::LogRecord record;
    record.level = level;
    record.message = std::move(message);
    recorder.write(record);
  };
  logAt(obs::LogLevel::Info, "chatter");
  logAt(obs::LogLevel::Warn, "one");
  logAt(obs::LogLevel::Error, "two");
  logAt(obs::LogLevel::Warn, "three");
  CHECK(recorder.records().size() == 2);
  CHECK(recorder.droppedRecords() == 1);
  CHECK(recorder.countAtLevel(obs::LogLevel::Warn) == 2);
  CHECK(recorder.countAtLevel(obs::LogLevel::Error) == 1);

  obs::SpanRecord top;
  top.name = "decompile";
  top.depth = 0;
  recorder.write(top);
  obs::SpanRecord child;
  child.name = "pass";
  child.depth = 1;
  recorder.write(child);
  REQUIRE(recorder.topLevelSpans().size() == 1);
  CHECK(recorder.topLevelSpans()[0].name == "decompile");
  CHECK(recorder.summary().stats().size() == 2);
}

TEST_CASE("the Chrome trace sink writes a closed JSON array", "[obs][sinks]") {
  const auto path = std::filesystem::temp_directory_path() /
                    ("xdec-trace-test-" + std::to_string(obs::monotonicNs()) + ".json");
  std::string error;
  auto sink = obs::ChromeTraceSink::create(path, &error);
  REQUIRE(sink);
  obs::SpanRecord record;
  record.category = "pass";
  record.name = "dce \"quoted\"";
  record.startNs = 1000;
  record.durationNs = 2000;
  sink->write(record);
  sink->counter("memory", 3000, obs::kv("rss", 1024u));
  sink->close();

  std::ifstream in{path};
  std::stringstream text;
  text << in.rdbuf();
  in.close();
  std::filesystem::remove(path);
  const std::string json = text.str();
  REQUIRE_FALSE(json.empty());
  CHECK(json.find('[') == json.find_first_not_of(" \r\n"));
  CHECK(json.find_last_not_of(" \r\n") == json.rfind(']'));
  CHECK(json.find(R"(dce \"quoted\")") != std::string::npos);
  CHECK(json.find(R"("ph":"C")") != std::string::npos);
}

TEST_CASE("a category defined outside the library registers like one inside", "[obs][log]") {
  CHECK(testGamma().name() == "test.gamma");
  CHECK_FALSE(testGamma().enabled(obs::LogLevel::Debug));
}
