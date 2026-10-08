#include "xdec/obs/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "sink_list.h"
#include "thread_state.h"
#include "xdec/obs/clock.h"
#include "xdec/obs/job.h"
#include "xdec/obs/sinks.h"

namespace xdec::obs {
namespace {

/// One `name=level` rule; `*` matches every category. Rules apply in order,
/// both to categories already registered and to ones that register later,
/// so `info,driver=debug` means what it reads as.
struct Rule {
  std::string category;
  LogLevel level;
};

struct Registry {
  std::mutex mutex;
  std::vector<LogCategory*> categories;
  std::vector<Rule> rules;
  bool environmentRead = false;
};

Registry& registry() {
  static Registry* instance = new Registry;
  return *instance;
}

detail::SinkList<LogSink>& sinks() {
  static detail::SinkList<LogSink>* instance = [] {
    auto* list = new detail::SinkList<LogSink>;
    list->add(std::make_shared<TextSink>());
    return list;
  }();
  return *instance;
}

void applyRule(LogCategory& category, const Rule& rule) {
  if (rule.category == "*" || rule.category == category.name()) {
    category.setLevel(rule.level);
  }
}

bool parseRules(std::string_view spec, std::vector<Rule>& out, std::string* error) {
  std::string_view remaining = spec;
  while (!remaining.empty()) {
    const std::size_t comma = remaining.find(',');
    std::string_view clause = remaining.substr(0, comma);
    while (!clause.empty() && clause.front() == ' ') {
      clause.remove_prefix(1);
    }
    while (!clause.empty() && clause.back() == ' ') {
      clause.remove_suffix(1);
    }
    if (!clause.empty()) {
      const std::size_t separator = clause.find('=');
      const std::string_view name =
          separator == std::string_view::npos ? std::string_view{"*"} : clause.substr(0, separator);
      const std::string_view levelText =
          separator == std::string_view::npos ? clause : clause.substr(separator + 1);
      LogLevel level = LogLevel::Warn;
      if (name.empty() || !parseLogLevel(levelText, level)) {
        if (error != nullptr) {
          *error = std::format("bad log clause '{}' (expected <level> or <category>=<level>, "
                               "level one of off/error/warn/info/debug/trace)",
                               clause);
        }
        return false;
      }
      out.push_back(Rule{std::string{name}, level});
    }
    if (comma == std::string_view::npos) {
      break;
    }
    remaining = remaining.substr(comma + 1);
  }
  return true;
}

/// Caller holds the registry mutex.
void readEnvironmentLocked(Registry& reg) {
  if (reg.environmentRead) {
    return;
  }
  reg.environmentRead = true;
  if (const char* spec = std::getenv("XDEC_LOG"); spec != nullptr) {
    std::vector<Rule> rules;
    parseRules(spec, rules, nullptr);
    reg.rules.insert(reg.rules.end(), rules.begin(), rules.end());
  }
}

/// Guards against a sink that logs from inside write().
thread_local bool tDispatching = false;

}  // namespace

std::string_view toString(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Off:
      return "off";
    case LogLevel::Error:
      return "error";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Info:
      return "info";
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Trace:
      return "trace";
  }
  return "?";
}

bool parseLogLevel(std::string_view text, LogLevel& out) noexcept {
  if (text == "off" || text == "none") {
    out = LogLevel::Off;
  } else if (text == "error") {
    out = LogLevel::Error;
  } else if (text == "warn" || text == "warning") {
    out = LogLevel::Warn;
  } else if (text == "info") {
    out = LogLevel::Info;
  } else if (text == "debug") {
    out = LogLevel::Debug;
  } else if (text == "trace" || text == "all") {
    out = LogLevel::Trace;
  } else {
    return false;
  }
  return true;
}

LogCategory::LogCategory(std::string_view name, LogLevel defaultLevel) noexcept
    : name_(name), default_(defaultLevel), level_(defaultLevel) {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  readEnvironmentLocked(reg);
  reg.categories.push_back(this);
  for (const Rule& rule : reg.rules) {
    applyRule(*this, rule);
  }
}

bool applyLogSpec(std::string_view spec, std::string* error) {
  std::vector<Rule> rules;
  const bool ok = parseRules(spec, rules, error);
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  readEnvironmentLocked(reg);
  for (const Rule& rule : rules) {
    for (LogCategory* category : reg.categories) {
      applyRule(*category, rule);
    }
    reg.rules.push_back(rule);
  }
  return ok;
}

void initLoggingFromEnvironment() {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  reg.environmentRead = false;
  const std::size_t before = reg.rules.size();
  readEnvironmentLocked(reg);
  for (std::size_t i = before; i < reg.rules.size(); ++i) {
    for (LogCategory* category : reg.categories) {
      applyRule(*category, reg.rules[i]);
    }
  }
}

void resetLogLevels() {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  reg.rules.clear();
  reg.environmentRead = true;
  for (LogCategory* category : reg.categories) {
    category->setLevel(category->defaultLevel());
  }
}

bool setLogLevel(std::string_view categoryName, LogLevel level) {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  bool found = false;
  for (LogCategory* category : reg.categories) {
    if (category->name() == categoryName) {
      category->setLevel(level);
      found = true;
    }
  }
  return found;
}

void setAllLogLevels(LogLevel level) {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  for (LogCategory* category : reg.categories) {
    category->setLevel(level);
  }
  reg.rules.push_back(Rule{"*", level});
}

std::vector<LogCategory*> logCategories() {
  Registry& reg = registry();
  const std::lock_guard lock{reg.mutex};
  std::vector<LogCategory*> result = reg.categories;
  std::ranges::sort(result, {}, &LogCategory::name);
  return result;
}

SinkId addLogSink(std::shared_ptr<LogSink> sink) { return sinks().add(std::move(sink)); }
void removeLogSink(SinkId id) { sinks().remove(id); }
void clearLogSinks() { sinks().clear(); }
void flushLogSinks() {
  for (const auto& entry : *sinks().snapshot()) {
    entry.sink->flush();
  }
}

void dispatch(const LogRecord& record) {
  if (tDispatching) {
    return;
  }
  tDispatching = true;
  for (const auto& entry : *sinks().snapshot()) {
    entry.sink->write(record);
  }
  if (const Job* job = Job::current(); job != nullptr) {
    job->deliver(record);
  }
  tDispatching = false;
}

namespace detail {

void emit(const LogCategory& category, LogLevel level, std::source_location where, Fields fields,
          std::string message) {
  LogRecord record;
  record.timeNs = monotonicNs();
  record.wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  record.level = level;
  record.category = category.name();
  record.message = std::move(message);
  record.where = where;
  record.thread = threadIndex();
  if (const Job* job = Job::current(); job != nullptr) {
    record.job = job->id();
  }
  {
    ThreadState& state = threadState();
    const std::lock_guard lock{state.mutex};
    record.context = state.context;
  }
  record.fields = std::move(fields);
  dispatch(record);
}

}  // namespace detail
}  // namespace xdec::obs
