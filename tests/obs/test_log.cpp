// Log categories, the spec grammar, record dispatch and the formatters every
// front end shares. Each test case runs in its own process under ctest, but
// they still undo what they install so the suite also runs as one binary.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xdec/obs/context.h"
#include "xdec/obs/field.h"
#include "xdec/obs/job.h"
#include "xdec/obs/log.h"
#include "xdec/obs/sinks.h"

namespace obs = xdec::obs;

namespace {

XDEC_DEFINE_LOG_CATEGORY(testAlpha, "test.alpha")
XDEC_DEFINE_LOG_CATEGORY_LEVEL(testBeta, "test.beta", ::xdec::obs::LogLevel::Info)

class CaptureSink final : public obs::LogSink {
 public:
  void write(const obs::LogRecord& record) override {
    const std::lock_guard lock{mutex};
    records.push_back(record);
  }
  std::mutex mutex;
  std::vector<obs::LogRecord> records;
};

/// Replaces the process sinks with a capture for the test's lifetime.
struct Captured {
  Captured() {
    obs::clearLogSinks();
    obs::resetLogLevels();
    obs::addLogSink(sink);
  }
  ~Captured() {
    obs::clearLogSinks();
    obs::resetLogLevels();
  }
  std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
};

}  // namespace

TEST_CASE("log levels parse by name and alias", "[obs][log]") {
  obs::LogLevel level = obs::LogLevel::Off;
  CHECK(obs::parseLogLevel("debug", level));
  CHECK(level == obs::LogLevel::Debug);
  CHECK(obs::parseLogLevel("warning", level));
  CHECK(level == obs::LogLevel::Warn);
  CHECK(obs::parseLogLevel("none", level));
  CHECK(level == obs::LogLevel::Off);
  CHECK_FALSE(obs::parseLogLevel("loud", level));
}

TEST_CASE("a category starts at its default level", "[obs][log]") {
  obs::resetLogLevels();
  CHECK(testAlpha().level() == obs::LogLevel::Warn);
  CHECK(testBeta().level() == obs::LogLevel::Info);
  CHECK(testAlpha().enabled(obs::LogLevel::Error));
  CHECK_FALSE(testAlpha().enabled(obs::LogLevel::Info));
  CHECK_FALSE(testAlpha().enabled(obs::LogLevel::Off));
}

TEST_CASE("log spec rules apply in order", "[obs][log]") {
  obs::resetLogLevels();
  REQUIRE(obs::applyLogSpec("info,test.alpha=trace"));
  CHECK(testAlpha().level() == obs::LogLevel::Trace);
  CHECK(testBeta().level() == obs::LogLevel::Info);

  REQUIRE(obs::applyLogSpec("test.beta=off, error"));
  CHECK(testAlpha().level() == obs::LogLevel::Error);
  CHECK(testBeta().level() == obs::LogLevel::Error);

  obs::resetLogLevels();
  CHECK(testAlpha().level() == obs::LogLevel::Warn);
  CHECK(testBeta().level() == obs::LogLevel::Info);
}

TEST_CASE("a bad log spec reports the clause", "[obs][log]") {
  obs::resetLogLevels();
  std::string error;
  CHECK_FALSE(obs::applyLogSpec("test.alpha=shout", &error));
  CHECK(error.find("test.alpha=shout") != std::string::npos);
  CHECK_FALSE(obs::applyLogSpec("=debug", &error));
  obs::resetLogLevels();
}

TEST_CASE("setLogLevel finds registered categories by name", "[obs][log]") {
  // Categories register on first use; applyLogSpec is what reaches later ones.
  (void)testAlpha();
  obs::resetLogLevels();
  CHECK(obs::setLogLevel("test.alpha", obs::LogLevel::Debug));
  CHECK(testAlpha().level() == obs::LogLevel::Debug);
  CHECK_FALSE(obs::setLogLevel("test.nonexistent", obs::LogLevel::Debug));

  bool listed = false;
  for (const obs::LogCategory* category : obs::logCategories()) {
    listed = listed || category->name() == "test.alpha";
  }
  CHECK(listed);
  obs::resetLogLevels();
}

TEST_CASE("disabled records never format or dispatch", "[obs][log]") {
  Captured captured;
  int evaluated = 0;
  auto sideEffect = [&] { return ++evaluated; };
  XDEC_LOG_DEBUG(testAlpha(), "value {}", sideEffect());
  CHECK(evaluated == 0);
  CHECK(captured.sink->records.empty());

  XDEC_LOG_WARN(testAlpha(), "value {}", sideEffect());
  CHECK(evaluated == 1);
  REQUIRE(captured.sink->records.size() == 1);
  CHECK(captured.sink->records[0].message == "value 1");
  CHECK(captured.sink->records[0].category == "test.alpha");
  CHECK(captured.sink->records[0].level == obs::LogLevel::Warn);
}

TEST_CASE("records carry fields, context and the job", "[obs][log]") {
  Captured captured;
  obs::Job job{"logging"};
  obs::JobScope scope{job};
  {
    XDEC_LOG_CONTEXT("entry", obs::hex(0x1000));
    XDEC_LOG_CONTEXT("round", 3);
    XDEC_LOG_F(testAlpha(), obs::LogLevel::Error, obs::kv("blocks", 12u), "stuck");
  }
  XDEC_LOG_ERROR(testAlpha(), "outside");

  REQUIRE(captured.sink->records.size() == 2);
  const obs::LogRecord& inside = captured.sink->records[0];
  CHECK(inside.job == job.id());
  REQUIRE(inside.context.size() == 2);
  CHECK(inside.context[0].key == "entry");
  CHECK(inside.context[0].value.text() == "0x1000");
  CHECK(inside.context[1].value.text() == "3");
  REQUIRE(inside.fields.size() == 1);
  CHECK(inside.fields[0].value.text() == "12");
  CHECK(captured.sink->records[1].context.empty());
}

TEST_CASE("job-local sinks see only their job's records", "[obs][log]") {
  Captured captured;
  auto local = std::make_shared<CaptureSink>();
  obs::Job job{"local"};
  job.addLogSink(local);
  XDEC_LOG_ERROR(testAlpha(), "before");
  {
    obs::JobScope scope{job};
    XDEC_LOG_ERROR(testAlpha(), "during");
  }
  XDEC_LOG_ERROR(testAlpha(), "after");
  CHECK(captured.sink->records.size() == 3);
  REQUIRE(local->records.size() == 1);
  CHECK(local->records[0].message == "during");
}

TEST_CASE("JSON strings escape control characters and quotes", "[obs][json]") {
  std::string out;
  obs::appendJsonString(out, "a\"b\\c\n\t\x01");
  CHECK(out == R"("a\"b\\c\n\t\u0001")");

  out.clear();
  obs::appendJsonObject(out, obs::kv("n", -3, "u", 7u, "b", true, "s", "x"));
  CHECK(out == R"({"n":-3,"u":7,"b":true,"s":"x"})");
}

TEST_CASE("the text formatter shows level, category, context and fields", "[obs][sinks]") {
  obs::LogRecord record;
  record.level = obs::LogLevel::Warn;
  record.category = "driver";
  record.message = "round cap";
  record.context = obs::kv("entry", obs::hex(0x10));
  record.fields = obs::kv("rounds", 8);
  obs::TextSink::Options options;
  options.showTime = false;
  const std::string text = obs::TextSink::format(record, options);
  CHECK(text.find("warn") != std::string::npos);
  CHECK(text.find("driver") != std::string::npos);
  CHECK(text.find("round cap") != std::string::npos);
  CHECK(text.find("entry=0x10") != std::string::npos);
  CHECK(text.find("rounds=8") != std::string::npos);
  CHECK(text.back() == '\n');
}

TEST_CASE("the cli category prints bare, prefixed only for problems", "[obs][sinks]") {
  obs::LogRecord record;
  record.category = "cli";
  record.message = "wrote out.c";
  record.level = obs::LogLevel::Info;
  CHECK(obs::TextSink::format(record, {}) == "wrote out.c\n");
  record.level = obs::LogLevel::Error;
  CHECK(obs::TextSink::format(record, {}) == "error: wrote out.c\n");
}

TEST_CASE("the JSON-lines formatter writes one object per record", "[obs][sinks]") {
  obs::LogRecord record;
  record.level = obs::LogLevel::Error;
  record.category = "pass";
  record.message = "line \"one\"\nline two";
  record.fields = obs::kv("ops", 5u);
  const std::string line = obs::JsonLinesSink::format(record);
  REQUIRE(line.size() > 2);
  CHECK(line.find('\n') == line.size() - 1);
  CHECK(line.front() == '{');
  CHECK(line[line.size() - 2] == '}');
  CHECK(line.find(R"("level":"error")") != std::string::npos);
  CHECK(line.find(R"("cat":"pass")") != std::string::npos);
  CHECK(line.find(R"(line \"one\"\nline two)") != std::string::npos);
  CHECK(line.find(R"("ops":5)") != std::string::npos);
}

TEST_CASE("the ring buffer keeps the newest records at or above its floor", "[obs][sinks]") {
  obs::RingBufferSink ring{3, obs::LogLevel::Info};
  for (int i = 0; i < 5; ++i) {
    obs::LogRecord record;
    record.level = obs::LogLevel::Info;
    record.message = std::to_string(i);
    ring.write(record);
  }
  obs::LogRecord noisy;
  noisy.level = obs::LogLevel::Debug;
  noisy.message = "debug";
  ring.write(noisy);

  const auto kept = ring.snapshot();
  REQUIRE(kept.size() == 3);
  CHECK(kept[0].message == "2");
  CHECK(kept[2].message == "4");
}
