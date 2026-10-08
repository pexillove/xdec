#include "run_context.h"

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>

#include "common.h"
#include "xdec/obs/clock.h"
#include "xdec/obs/metrics.h"
#include "xdec/obs/process.h"

#ifndef XDEC_VERSION
#define XDEC_VERSION "unknown"
#endif
#ifndef XDEC_GIT_REV
#define XDEC_GIT_REV "unknown"
#endif

namespace xdec::cli {
namespace {

RunContext* gCurrent = nullptr;

json::Value toJson(const obs::FieldValue& value) {
  return std::visit(
      [](const auto& v) -> json::Value {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return json::Value{};
        } else if constexpr (std::is_same_v<T, std::string>) {
          return json::Value{v};
        } else {
          return json::Value{v};
        }
      },
      value.storage());
}

json::Value toJson(const obs::Fields& fields) {
  std::vector<json::Member> members;
  members.reserve(fields.size());
  for (const obs::Field& field : fields) {
    members.emplace_back(std::string{field.key}, toJson(field.value));
  }
  return json::Value::object(std::move(members));
}

double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }
double mib(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

std::string isoTime(int64_t wallUs) {
  const std::chrono::system_clock::time_point point{std::chrono::microseconds{wallUs}};
  return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(point));
}

int64_t wallNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

RunContext::RunContext() { gCurrent = this; }

RunContext::~RunContext() {
  finish(0);
  if (gCurrent == this) {
    gCurrent = nullptr;
  }
}

RunContext* RunContext::current() noexcept { return gCurrent; }

std::optional<std::string> RunContext::configure(std::vector<std::string_view>& args) {
  std::vector<std::string_view> rest;
  rest.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view option = args[i];
    const auto value = [&]() -> std::optional<std::string_view> {
      if (i + 1 >= args.size()) {
        return std::nullopt;
      }
      return args[++i];
    };
    const auto needValue = [&](std::string_view what) -> std::optional<std::string_view> {
      auto text = value();
      if (!text) {
        return std::nullopt;
      }
      (void)what;
      return text;
    };
    const auto duration = [&](std::optional<std::string_view> text,
                              std::chrono::nanoseconds& out) -> bool {
      return text && obs::parseDuration(*text, out);
    };

    if (option == "--log") {
      const auto text = needValue("spec");
      std::string error;
      if (!text || !obs::applyLogSpec(*text, &error)) {
        return text ? error : std::string{"'--log' needs a spec, e.g. driver=info,pass=debug"};
      }
      options_.logSpecs.emplace_back(*text);
    } else if (option == "--log-file") {
      const auto text = needValue("path");
      if (!text) {
        return "'--log-file' needs a path";
      }
      options_.logFile = std::filesystem::path{std::string{*text}};
    } else if (option == "--log-format") {
      const auto text = needValue("format");
      if (text && *text == "text") {
        options_.logFormat = GlobalOptions::LogFormat::Text;
      } else if (text && (*text == "json" || *text == "jsonl")) {
        options_.logFormat = GlobalOptions::LogFormat::Json;
      } else {
        return "'--log-format' takes 'text' or 'json'";
      }
    } else if (option == "--trace") {
      const auto text = needValue("path");
      if (!text) {
        return "'--trace' needs a path (Chrome trace JSON, open in Perfetto)";
      }
      options_.tracePath = std::filesystem::path{std::string{*text}};
    } else if (option == "--report") {
      const auto text = needValue("path");
      if (!text) {
        return "'--report' needs a path";
      }
      options_.reportPath = std::filesystem::path{std::string{*text}};
    } else if (option == "--crash-dir") {
      const auto text = needValue("dir");
      if (!text) {
        return "'--crash-dir' needs a directory";
      }
      options_.crashDir = std::filesystem::path{std::string{*text}};
    } else if (option == "--progress") {
      const auto text = needValue("mode");
      if (text && *text == "auto") {
        options_.progress = GlobalOptions::Progress::Auto;
      } else if (text && *text == "always") {
        options_.progress = GlobalOptions::Progress::Always;
      } else if (text && *text == "never") {
        options_.progress = GlobalOptions::Progress::Never;
      } else {
        return "'--progress' takes 'auto', 'always' or 'never'";
      }
    } else if (option == "--timeout") {
      std::chrono::nanoseconds parsed{};
      if (!duration(needValue("duration"), parsed) || parsed.count() == 0) {
        return "'--timeout' takes a duration above zero, e.g. 90s, 5m, 1500ms";
      }
      options_.timeout = parsed;
    } else if (option == "--hard-timeout") {
      std::chrono::nanoseconds parsed{};
      if (!duration(needValue("duration"), parsed) || parsed.count() == 0) {
        return "'--hard-timeout' takes a duration above zero";
      }
      options_.hardTimeout = parsed;
    } else if (option == "--on-timeout") {
      const auto text = needValue("policy");
      if (text && *text == "partial") {
        options_.onTimeout = GlobalOptions::OnTimeout::Partial;
      } else if (text && *text == "abort") {
        options_.onTimeout = GlobalOptions::OnTimeout::Abort;
      } else {
        return "'--on-timeout' takes 'partial' or 'abort'";
      }
    } else if (option == "--watchdog") {
      std::chrono::nanoseconds parsed{};
      if (!duration(needValue("duration"), parsed)) {
        return "'--watchdog' takes a duration (0 disables)";
      }
      options_.watchdog = parsed;
    } else if (option == "-q" || option == "--quiet") {
      options_.quiet = true;
    } else if (option == "-v" || option == "--verbose") {
      options_.verbose = true;
    } else if (option == "--machine") {
      options_.machine = true;
    } else if (option == "--profile") {
      options_.profile = true;
    } else {
      rest.push_back(option);
    }
  }
  if (options_.verbose) {
    // Before any --log spec, whose rules apply after it and so win.
    obs::applyLogSpec("info");
    for (const std::string& spec : options_.logSpecs) {
      obs::applyLogSpec(spec);
    }
  }
  if (options_.quiet) {
    obs::setLogLevel("cli", obs::LogLevel::Warn);
  }
  args = std::move(rest);
  return std::nullopt;
}

void RunContext::start(std::span<const std::string_view> commandLine) {
  commandLine_.assign(commandLine.begin(), commandLine.end());
  startWallUs_ = wallNowUs();

  obs::clearLogSinks();
  if (options_.machine) {
    auto jsonl = std::make_shared<obs::JsonLinesSink>();
    obs::addLogSink(jsonl);
    job_.addProgressSink(jsonl);
  } else {
    obs::TextSink::Options console;
    console.maxLevel = options_.quiet ? obs::LogLevel::Warn : obs::LogLevel::Trace;
    obs::addLogSink(std::make_shared<obs::TextSink>(console));
  }

  recent_ = std::make_shared<obs::RingBufferSink>(2000);
  obs::addLogSink(recent_);
  obs::installCrashHandlers(obs::CrashOptions{
      .directory = options_.crashDir.empty() ? std::filesystem::path{"."} : options_.crashDir,
      .recent = recent_});

  if (!options_.logFile.empty()) {
    std::string error;
    const bool json =
        options_.logFormat == GlobalOptions::LogFormat::Json ||
        (options_.logFormat == GlobalOptions::LogFormat::Auto &&
         (options_.logFile.extension() == ".jsonl" || options_.logFile.extension() == ".json"));
    if (json) {
      if (auto sink = obs::JsonLinesSink::toFile(options_.logFile, &error)) {
        obs::addLogSink(sink);
        job_.addProgressSink(sink);
      } else {
        warn("{}; continuing without a log file", error);
      }
    } else {
      obs::TextSink::Options fileOptions;
      fileOptions.showSource = true;
      if (auto sink = obs::TextSink::toFile(options_.logFile, fileOptions, &error)) {
        obs::addLogSink(sink);
      } else {
        warn("{}; continuing without a log file", error);
      }
    }
  }

  if (!options_.tracePath.empty()) {
    std::string error;
    trace_ = obs::ChromeTraceSink::create(options_.tracePath, &error);
    if (trace_) {
      obs::addSpanSink(trace_);
    } else {
      warn("{}; continuing without a trace", error);
    }
  }

  if (!options_.reportPath.empty() || options_.profile) {
    recorder_ = std::make_shared<obs::RunRecorder>();
    obs::addLogSink(recorder_);
    obs::addSpanSink(recorder_);
  }

  if (options_.timeout) {
    job_.setSoftDeadline(*options_.timeout);
    std::chrono::nanoseconds hard = *options_.timeout;
    if (options_.hardTimeout) {
      hard = *options_.hardTimeout;
    } else if (options_.onTimeout == GlobalOptions::OnTimeout::Partial) {
      hard += std::max<std::chrono::nanoseconds>(std::chrono::seconds{30}, *options_.timeout / 2);
    }
    job_.setHardDeadline(hard);
  } else if (options_.hardTimeout) {
    job_.setHardDeadline(*options_.hardTimeout);
  }
  obs::installInterruptHandler(job_);

  const bool status =
      !options_.machine && !options_.quiet &&
      (options_.progress == GlobalOptions::Progress::Always ||
       (options_.progress == GlobalOptions::Progress::Auto && obs::Terminal::stderrIsTty()));
  obs::Monitor::Options monitorOptions;
  monitorOptions.status = status;
  monitorOptions.stallAfter = options_.watchdog;
  if (trace_) {
    monitorOptions.tick = std::chrono::milliseconds{250};
    monitorOptions.onTick = [trace = trace_] {
      const obs::MemoryUsage memory = obs::processMemory();
      trace->counter("memory", obs::monotonicNs(),
                     obs::kv("rss_mib", static_cast<double>(memory.currentBytes >> 20)));
    };
  }
  if (status || options_.watchdog.count() > 0 || trace_) {
    monitor_ = std::make_unique<obs::Monitor>(job_, std::move(monitorOptions));
  }
}

void RunContext::setSection(std::string key, json::Value value) {
  for (auto& [existing, section] : sections_) {
    if (existing == key) {
      section = std::move(value);
      return;
    }
  }
  sections_.emplace_back(std::move(key), std::move(value));
}

void RunContext::setOutcome(std::string outcome) { outcome_ = std::move(outcome); }

void RunContext::noteDiag(const Diag& diag) { failure_ = diag; }

std::string RunContext::outcomeFor(int exitCode) const {
  if (failure_.has_value()) {
    if (failure_->code() == DiagCode::Cancelled) {
      return "cancelled";
    }
    if (failure_->code() == DiagCode::Timeout) {
      return "timeout";
    }
  }
  if (exitCode != 0) {
    return "failed";
  }
  return outcome_.empty() ? std::string{"ok"} : outcome_;
}

json::Value RunContext::buildReport(int exitCode) const {
  const uint64_t durationNs = obs::monotonicNs() - job_.startNs();
  json::Value report = json::Value::object({});
  report.set("schema", "xdec.run-report/1");
  report.set("tool", json::Value::object({{"name", "xdec"},
                                          {"version", XDEC_VERSION},
                                          {"git", XDEC_GIT_REV}}));
  std::vector<json::Value> argv;
  for (const std::string& arg : commandLine_) {
    argv.emplace_back(arg);
  }
  report.set("command", json::Value::array(std::move(argv)));
  report.set("started", isoTime(startWallUs_));
  report.set("duration_s", static_cast<double>(durationNs) / 1e9);
  report.set("outcome", outcomeFor(exitCode));
  report.set("exit_code", static_cast<int64_t>(exitCode));
  if (failure_.has_value()) {
    report.set("error", json::Value::object({{"code", std::string{toString(failure_->code())}},
                                             {"message", failure_->format()}}));
  }
  report.set("stop", json::Value::object(
                         {{"wind_down", std::string{job_.windDownReason()}},
                          {"stop_reason", std::string{obs::toString(job_.stopReason())}},
                          {"interrupts", static_cast<uint64_t>(job_.interruptCount())}}));

  const obs::MemoryUsage memory = obs::processMemory();
  report.set("memory", json::Value::object({{"peak_mib", mib(memory.peakBytes)},
                                            {"current_mib", mib(memory.currentBytes)}}));

  std::vector<json::Member> counters;
  for (const auto& [name, value] : obs::counterSnapshot()) {
    counters.emplace_back(std::string{name}, json::Value{value});
  }
  report.set("counters", json::Value::object(std::move(counters)));

  if (recorder_) {
    std::vector<json::Value> summary;
    for (const obs::SpanSummarySink::Stat& stat : recorder_->summary().stats()) {
      if (summary.size() >= 60) {
        break;
      }
      summary.push_back(json::Value::object({{"category", stat.category},
                                             {"name", stat.name},
                                             {"count", stat.count},
                                             {"total_ms", ms(stat.totalNs)},
                                             {"self_ms", ms(stat.selfNs)},
                                             {"max_ms", ms(stat.maxNs)}}));
    }
    std::vector<json::Value> top;
    for (const obs::SpanRecord& span : recorder_->topLevelSpans()) {
      top.push_back(json::Value::object({{"category", std::string{span.category}},
                                         {"name", span.name},
                                         {"start_s", static_cast<double>(span.startNs) / 1e9},
                                         {"duration_ms", ms(span.durationNs)},
                                         {"fields", toJson(span.fields)}}));
    }
    report.set("timing", json::Value::object({{"top_level", json::Value::array(std::move(top))},
                                              {"by_span", json::Value::array(std::move(summary))}}));

    std::vector<json::Value> records;
    for (const obs::LogRecord& record : recorder_->records()) {
      json::Value entry = json::Value::object({{"t", static_cast<double>(record.timeNs) / 1e9},
                                               {"level", std::string{toString(record.level)}},
                                               {"category", std::string{record.category}},
                                               {"message", record.message}});
      if (!record.context.empty()) {
        entry.set("context", toJson(record.context));
      }
      if (!record.fields.empty()) {
        entry.set("fields", toJson(record.fields));
      }
      records.push_back(std::move(entry));
    }
    report.set("diagnostics",
               json::Value::object(
                   {{"errors", static_cast<uint64_t>(recorder_->countAtLevel(obs::LogLevel::Error))},
                    {"warnings", static_cast<uint64_t>(recorder_->countAtLevel(obs::LogLevel::Warn))},
                    {"dropped", static_cast<uint64_t>(recorder_->droppedRecords())},
                    {"records", json::Value::array(std::move(records))}}));
  }
  report.set("watchdog",
             json::Value::object(
                 {{"stall_after_s", static_cast<double>(options_.watchdog.count()) / 1e9},
                  {"stall_reports",
                   static_cast<uint64_t>(monitor_ ? monitor_->stallReports() : 0)}}));
  for (const auto& [key, section] : sections_) {
    report.set(key, section);
  }
  return report;
}

void RunContext::finish(int exitCode) {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (monitor_) {
    monitor_->stop();
  }
  obs::uninstallInterruptHandler();
  if (trace_) {
    const obs::MemoryUsage memory = obs::processMemory();
    trace_->counter("memory", obs::monotonicNs(),
                    obs::kv("rss_mib", static_cast<double>(memory.currentBytes >> 20)));
  }
  obs::flushSpanSinks();
  if (trace_) {
    trace_->close();
  }
  if (!options_.reportPath.empty()) {
    const json::Value report = buildReport(exitCode);
    std::error_code ec;
    if (options_.reportPath.has_parent_path()) {
      std::filesystem::create_directories(options_.reportPath.parent_path(), ec);
    }
    std::ofstream stream(options_.reportPath, std::ios::binary | std::ios::trunc);
    if (stream) {
      stream << report.dump(2) << '\n';
    } else {
      warn("cannot write the run report to '{}'", options_.reportPath.string());
    }
  }
  if (options_.profile && recorder_) {
    obs::Terminal::instance().write("\n" + recorder_->summary().format() + "\n");
  }
  if (options_.machine) {
    std::string line = R"({"type":"result","outcome":)";
    obs::appendJsonString(line, outcomeFor(exitCode));
    line += std::format(R"(,"exit_code":{})", exitCode);
    if (!options_.reportPath.empty()) {
      line += R"(,"report":)";
      obs::appendJsonString(line, options_.reportPath.string());
    }
    line += "}\n";
    obs::Terminal::instance().write(line);
  }
  obs::flushLogSinks();
}

}  // namespace xdec::cli
