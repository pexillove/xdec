#include "xdec/obs/sinks.h"

#include <algorithm>
#include <cstdio>
#include <format>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace xdec::obs {
namespace {

std::size_t terminalWidth() noexcept {
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info)) {
    return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
  }
#else
  winsize size{};
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    return size.ws_col;
  }
#endif
  return 100;
}

std::string sourceText(const std::source_location& where) {
  std::string_view file = where.file_name();
  if (const std::size_t slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
    file.remove_prefix(slash + 1);
  }
  return std::format("{}:{}", file, where.line());
}

std::FILE* openTruncated(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
#if defined(_WIN32)
  std::FILE* file = _wfopen(path.c_str(), L"wb");
#else
  std::FILE* file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr && error != nullptr) {
    *error = std::format("cannot open '{}' for writing", path.string());
  }
  return file;
}

std::string seconds(uint64_t ns) { return std::format("{:.6f}", static_cast<double>(ns) / 1e9); }
std::string millis(uint64_t ns) { return std::format("{:.3f}", static_cast<double>(ns) / 1e6); }
std::string micros(uint64_t ns) { return std::format("{:.3f}", static_cast<double>(ns) / 1e3); }

}  // namespace

// ---------------------------------------------------------------------------
// Terminal

Terminal& Terminal::instance() {
  static auto* terminal = new Terminal;
  return *terminal;
}

bool Terminal::stderrIsTty() noexcept {
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(STDERR_FILENO) != 0;
#endif
}

void Terminal::eraseLocked() {
  if (!statusDrawn_) {
    return;
  }
  const std::size_t width = std::min(status_.size(), terminalWidth() - 1);
  std::string blank = "\r";
  blank.append(width, ' ');
  blank += '\r';
  std::fwrite(blank.data(), 1, blank.size(), stderr);
  statusDrawn_ = false;
}

void Terminal::drawLocked() {
  if (!statusEnabled_ || status_.empty()) {
    return;
  }
  const std::size_t width = terminalWidth();
  std::string line = "\r";
  line += status_.size() >= width ? status_.substr(0, width - 1) : status_;
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
  statusDrawn_ = true;
}

void Terminal::write(std::string_view text) {
  const std::lock_guard lock{mutex_};
  eraseLocked();
  std::fwrite(text.data(), 1, text.size(), stderr);
  std::fflush(stderr);
  drawLocked();
}

void Terminal::setStatus(std::string line) {
  const std::lock_guard lock{mutex_};
  eraseLocked();
  status_ = std::move(line);
  drawLocked();
}

void Terminal::enableStatus(bool enabled) {
  const std::lock_guard lock{mutex_};
  if (!enabled) {
    eraseLocked();
  }
  statusEnabled_ = enabled;
  drawLocked();
}

bool Terminal::statusEnabled() const {
  const std::lock_guard lock{mutex_};
  return statusEnabled_;
}

// ---------------------------------------------------------------------------
// TextSink

TextSink::TextSink() : TextSink(Options{}) {}
TextSink::TextSink(Options options) : options_(options) {}

std::shared_ptr<TextSink> TextSink::toFile(const std::filesystem::path& path, Options options,
                                           std::string* error) {
  std::FILE* file = openTruncated(path, error);
  if (file == nullptr) {
    return nullptr;
  }
  auto sink = std::make_shared<TextSink>(options);
  sink->file_ = file;
  return sink;
}

TextSink::~TextSink() {
  if (file_ != nullptr) {
    std::fclose(file_);
  }
}

std::string TextSink::format(const LogRecord& record, const Options& options) {
  std::string line;
  if (record.category == "cli") {
    if (record.level == LogLevel::Error) {
      line = "error: ";
    } else if (record.level == LogLevel::Warn) {
      line = "warning: ";
    }
    line += record.message;
    line += '\n';
    return line;
  }
  if (options.showTime) {
    line += std::format("{:>9.3f}s ", static_cast<double>(record.timeNs) / 1e9);
  }
  line += std::format("[{:<5} {}] {}", toString(record.level), record.category, record.message);
  if (!record.fields.empty()) {
    line += "  {";
    for (std::size_t i = 0; i < record.fields.size(); ++i) {
      line += std::format("{}{}={}", i == 0 ? "" : " ", record.fields[i].key,
                          record.fields[i].value.text());
    }
    line += '}';
  }
  if (options.showContext && !record.context.empty()) {
    line += "  (";
    for (std::size_t i = 0; i < record.context.size(); ++i) {
      line += std::format("{}{}={}", i == 0 ? "" : " ", record.context[i].key,
                          record.context[i].value.text());
    }
    line += ')';
  }
  if (options.showSource) {
    line += "  @" + sourceText(record.where);
  }
  line += '\n';
  return line;
}

void TextSink::write(const LogRecord& record) {
  if (record.level > options_.maxLevel) {
    return;
  }
  const std::string line = format(record, options_);
  if (file_ == nullptr) {
    Terminal::instance().write(line);
    return;
  }
  const std::lock_guard lock{mutex_};
  std::fwrite(line.data(), 1, line.size(), file_);
}

void TextSink::flush() {
  const std::lock_guard lock{mutex_};
  std::fflush(file_ != nullptr ? file_ : stderr);
}

// ---------------------------------------------------------------------------
// JsonLinesSink

JsonLinesSink::JsonLinesSink() = default;

std::shared_ptr<JsonLinesSink> JsonLinesSink::toFile(const std::filesystem::path& path,
                                                     std::string* error) {
  std::FILE* file = openTruncated(path, error);
  if (file == nullptr) {
    return nullptr;
  }
  auto sink = std::make_shared<JsonLinesSink>();
  sink->file_ = file;
  return sink;
}

JsonLinesSink::~JsonLinesSink() {
  if (file_ != nullptr) {
    std::fclose(file_);
  }
}

std::string JsonLinesSink::format(const LogRecord& record) {
  std::string out = R"({"type":"log","t":)";
  out += seconds(record.timeNs);
  out += std::format(R"(,"wall_us":{},"level":")", record.wallUs);
  out += toString(record.level);
  out += R"(","cat":)";
  appendJsonString(out, record.category);
  out += R"(,"msg":)";
  appendJsonString(out, record.message);
  out += std::format(R"(,"thread":{},"job":{})", record.thread, record.job);
  if (!record.context.empty()) {
    out += R"(,"ctx":)";
    appendJsonObject(out, record.context);
  }
  if (!record.fields.empty()) {
    out += R"(,"fields":)";
    appendJsonObject(out, record.fields);
  }
  out += R"(,"src":)";
  appendJsonString(out, sourceText(record.where));
  out += "}\n";
  return out;
}

std::string JsonLinesSink::format(const SpanRecord& record) {
  std::string out = R"({"type":"span","cat":)";
  appendJsonString(out, record.category);
  out += R"(,"name":)";
  appendJsonString(out, record.name);
  out += std::format(R"(,"start":{},"dur_ms":{},"self_ms":{},"thread":{},"job":{},"depth":{},)"
                     R"("id":{},"parent":{})",
                     seconds(record.startNs), millis(record.durationNs), millis(record.selfNs),
                     record.thread, record.job, record.depth, record.id, record.parent);
  if (!record.fields.empty()) {
    out += R"(,"fields":)";
    appendJsonObject(out, record.fields);
  }
  out += "}\n";
  return out;
}

std::string JsonLinesSink::format(const Job& job, const ProgressEvent& event) {
  std::string out = R"({"type":"progress","t":)";
  out += seconds(event.timeNs);
  out += std::format(R"(,"job":{},"phase":)", job.id());
  appendJsonString(out, event.phase);
  out += R"(,"detail":)";
  appendJsonString(out, event.detail);
  out += std::format(R"(,"current":{},"total":{})", event.current, event.total);
  if (!event.fields.empty()) {
    out += R"(,"fields":)";
    appendJsonObject(out, event.fields);
  }
  out += "}\n";
  return out;
}

void JsonLinesSink::writeLine(std::string line) {
  if (file_ == nullptr) {
    Terminal::instance().write(line);
    return;
  }
  const std::lock_guard lock{mutex_};
  std::fwrite(line.data(), 1, line.size(), file_);
}

void JsonLinesSink::write(const LogRecord& record) { writeLine(format(record)); }
void JsonLinesSink::write(const SpanRecord& record) { writeLine(format(record)); }
void JsonLinesSink::progress(const Job& job, const ProgressEvent& event) {
  writeLine(format(job, event));
}

void JsonLinesSink::flush() {
  const std::lock_guard lock{mutex_};
  std::fflush(file_ != nullptr ? file_ : stderr);
}

// ---------------------------------------------------------------------------
// RingBufferSink

RingBufferSink::RingBufferSink(std::size_t capacity, LogLevel minimum)
    : capacity_(std::max<std::size_t>(capacity, 1)), minimum_(minimum) {}

void RingBufferSink::write(const LogRecord& record) {
  if (record.level > minimum_) {
    return;
  }
  const std::lock_guard lock{mutex_};
  if (records_.size() == capacity_) {
    records_.pop_front();
  }
  records_.push_back(record);
}

std::vector<LogRecord> RingBufferSink::snapshot() const {
  const std::lock_guard lock{mutex_};
  return {records_.begin(), records_.end()};
}

std::vector<LogRecord> RingBufferSink::trySnapshot() const {
  std::unique_lock lock{mutex_, std::try_to_lock};
  if (!lock.owns_lock()) {
    return {};
  }
  return {records_.begin(), records_.end()};
}

// ---------------------------------------------------------------------------
// ChromeTraceSink

ChromeTraceSink::ChromeTraceSink(std::FILE* file) : file_(file) {
  std::fputs("[\n", file_);
}

std::shared_ptr<ChromeTraceSink> ChromeTraceSink::create(const std::filesystem::path& path,
                                                         std::string* error) {
  std::FILE* file = openTruncated(path, error);
  if (file == nullptr) {
    return nullptr;
  }
  return std::shared_ptr<ChromeTraceSink>(new ChromeTraceSink(file));
}

ChromeTraceSink::~ChromeTraceSink() { close(); }

void ChromeTraceSink::writeEvent(const std::string& event) {
  if (file_ == nullptr) {
    return;
  }
  if (!first_) {
    std::fputs(",\n", file_);
  }
  first_ = false;
  std::fwrite(event.data(), 1, event.size(), file_);
}

void ChromeTraceSink::write(const SpanRecord& record) {
  const std::lock_guard lock{mutex_};
  if (file_ == nullptr) {
    return;
  }
  if (std::ranges::find(namedThreads_, record.thread) == namedThreads_.end()) {
    namedThreads_.push_back(record.thread);
    writeEvent(std::format(
        R"({{"name":"thread_name","ph":"M","pid":1,"tid":{},"args":{{"name":"{}"}}}})",
        record.thread, record.thread == 0 ? "main" : std::format("worker {}", record.thread)));
  }
  std::string event = R"({"name":)";
  appendJsonString(event, record.name);
  event += R"(,"cat":)";
  appendJsonString(event, record.category);
  event += std::format(R"(,"ph":"X","ts":{},"dur":{},"pid":1,"tid":{},"args":)",
                       micros(record.startNs), micros(record.durationNs), record.thread);
  Fields args = record.fields;
  args.push_back(Field{"self_ms", FieldValue{static_cast<double>(record.selfNs) / 1e6}});
  if (record.job != 0) {
    args.push_back(Field{"job", FieldValue{record.job}});
  }
  appendJsonObject(event, args);
  event += '}';
  writeEvent(event);
}

void ChromeTraceSink::counter(std::string_view name, uint64_t timeNs, const Fields& values) {
  const std::lock_guard lock{mutex_};
  std::string event = R"({"name":)";
  appendJsonString(event, name);
  event += std::format(R"(,"ph":"C","ts":{},"pid":1,"args":)", micros(timeNs));
  appendJsonObject(event, values);
  event += '}';
  writeEvent(event);
}

void ChromeTraceSink::flush() {
  const std::lock_guard lock{mutex_};
  if (file_ != nullptr) {
    std::fflush(file_);
  }
}

void ChromeTraceSink::close() {
  const std::lock_guard lock{mutex_};
  if (file_ == nullptr) {
    return;
  }
  std::fputs("\n]\n", file_);
  std::fclose(file_);
  file_ = nullptr;
}

// ---------------------------------------------------------------------------
// SpanSummarySink

void SpanSummarySink::write(const SpanRecord& record) {
  const std::lock_guard lock{mutex_};
  Stat& stat = stats_[{std::string{record.category}, record.name}];
  if (stat.count == 0) {
    stat.category = record.category;
    stat.name = record.name;
  }
  ++stat.count;
  stat.totalNs += record.durationNs;
  stat.selfNs += record.selfNs;
  stat.maxNs = std::max(stat.maxNs, record.durationNs);
}

std::vector<SpanSummarySink::Stat> SpanSummarySink::stats() const {
  std::vector<Stat> out;
  {
    const std::lock_guard lock{mutex_};
    out.reserve(stats_.size());
    for (const auto& [key, stat] : stats_) {
      out.push_back(stat);
    }
  }
  std::ranges::sort(out, [](const Stat& a, const Stat& b) { return a.selfNs > b.selfNs; });
  return out;
}

std::string SpanSummarySink::format(std::size_t limit) const {
  const std::vector<Stat> all = stats();
  std::string out = std::format("{:<10} {:<28} {:>7} {:>11} {:>11} {:>11}\n", "category", "name",
                                "count", "total ms", "self ms", "max ms");
  for (std::size_t i = 0; i < all.size() && i < limit; ++i) {
    const Stat& stat = all[i];
    std::string name = stat.name.size() > 28 ? stat.name.substr(0, 27) + "~" : stat.name;
    out += std::format("{:<10} {:<28} {:>7} {:>11.1f} {:>11.1f} {:>11.1f}\n", stat.category, name,
                       stat.count, static_cast<double>(stat.totalNs) / 1e6,
                       static_cast<double>(stat.selfNs) / 1e6,
                       static_cast<double>(stat.maxNs) / 1e6);
  }
  if (all.size() > limit) {
    out += std::format("({} more row(s))\n", all.size() - limit);
  }
  return out;
}

}  // namespace xdec::obs
