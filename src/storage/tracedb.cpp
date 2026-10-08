#include "xdec/storage/tracedb.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <span>

#include "xdec/support/json.h"

namespace xdec::storage {
namespace {

[[nodiscard]] uint32_t get32(std::span<const uint8_t> bytes,
                             std::size_t at) noexcept {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(bytes[at + i]) << (i * 8);
  }
  return value;
}

[[nodiscard]] uint64_t get64(std::span<const uint8_t> bytes,
                             std::size_t at) noexcept {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(bytes[at + i]) << (i * 8);
  }
  return value;
}

[[nodiscard]] Result<std::vector<uint8_t>> readBytes(
    const std::filesystem::path& path, bool required) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    if (!required && !std::filesystem::exists(path)) {
      return std::vector<uint8_t>{};
    }
    return err(DiagCode::IoError, "cannot open tracedb file '{}'", path.string());
  }
  const std::streampos end = input.tellg();
  if (end < 0) {
    return err(DiagCode::IoError, "cannot size tracedb file '{}'", path.string());
  }
  const auto size = static_cast<uint64_t>(end);
  if (size > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return err(DiagCode::OutOfRange, "tracedb file is too large '{}'", path.string());
  }
  std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
      return err(DiagCode::IoError, "cannot read tracedb file '{}'", path.string());
    }
  }
  return bytes;
}

[[nodiscard]] Result<uint64_t> unsignedMember(const json::Value& object,
                                               std::string_view key,
                                               bool required,
                                               uint64_t fallback = 0) {
  const json::Value* value = object.find(key);
  if (value == nullptr) {
    if (!required) return fallback;
    return err(DiagCode::BadFormat, "tracedb manifest lacks '{}'", key);
  }
  if (!value->isNumber() || value->asDouble() < 0) {
    return err(DiagCode::BadFormat, "tracedb manifest '{}' is not unsigned", key);
  }
  return static_cast<uint64_t>(value->asInt());
}

[[nodiscard]] std::filesystem::path segmentPath(
    const std::filesystem::path& root, uint32_t thread, uint32_t segment) {
  return root / "threads" / std::format("thr_{:08d}", thread) / "segments" /
         std::format("seg_{:08d}", segment);
}

template <class Row, class Decode>
[[nodiscard]] Result<std::vector<Row>> readRows(
    const std::filesystem::path& path, std::size_t rowSize, uint64_t expected,
    bool required, Decode decode) {
  XDEC_TRY(std::vector<uint8_t> bytes, readBytes(path, required));
  if (bytes.size() % rowSize != 0) {
    return err(DiagCode::BadFormat,
               "tracedb stream '{}' has {} trailing byte(s)", path.string(),
               bytes.size() % rowSize);
  }
  const uint64_t count = bytes.size() / rowSize;
  if (expected != 0 && count != expected) {
    return err(DiagCode::BadFormat,
               "tracedb stream '{}' declares {} row(s), contains {}",
               path.string(), expected, count);
  }
  std::vector<Row> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (std::size_t at = 0; at < bytes.size(); at += rowSize) {
    XDEC_TRY(Row row, decode(std::span<const uint8_t>{bytes}.subspan(at, rowSize)));
    rows.push_back(row);
  }
  return rows;
}

[[nodiscard]] Result<TraceRow> decodeTrace(std::span<const uint8_t> row) {
  TraceRow out;
  out.traceId = get64(row, 0);
  out.pc = get64(row, 8);
  out.timestamp = static_cast<int64_t>(get64(row, 16));
  out.mnemonicId = get32(row, 24);
  out.flags = get32(row, 28);
  out.category = row[32];
  out.semanticsId = get32(row, 36);
  out.globalEventId = get64(row, 56);
  if (out.traceId == 0 || out.globalEventId == 0) {
    return err(DiagCode::BadFormat, "tracedb trace row contains a zero id");
  }
  return out;
}

[[nodiscard]] Result<CallGraphRow> decodeCallGraph(
    std::span<const uint8_t> row) {
  CallGraphRow out;
  out.callId = get64(row, 0);
  out.parentCallId = get64(row, 8);
  out.callerEntry = get64(row, 16);
  out.calleeTarget = get64(row, 24);
  out.callsitePc = get64(row, 32);
  out.enterTraceId = get64(row, 40);
  out.exitTraceId = get64(row, 48);
  out.depth = get32(row, 56);
  out.flags = row[60];
  if (out.callId == 0 || out.enterTraceId == 0 ||
      (out.exitTraceId != 0 && out.exitTraceId < out.enterTraceId)) {
    return err(DiagCode::BadFormat, "invalid tracedb callgraph row");
  }
  return out;
}

[[nodiscard]] Result<FlowRow> decodeFlow(std::span<const uint8_t> row) {
  FlowRow out;
  out.traceId = get64(row, 0);
  out.globalEventId = get64(row, 8);
  out.sourcePc = get64(row, 16);
  out.targetPc = get64(row, 24);
  out.kind = static_cast<FlowKind>(row[32]);
  if (out.traceId == 0 || out.globalEventId == 0 ||
      row[32] > static_cast<uint8_t>(FlowKind::Return)) {
    return err(DiagCode::BadFormat, "invalid tracedb flow row");
  }
  if (std::any_of(row.begin() + 33, row.end(),
                  [](uint8_t byte) { return byte != 0; })) {
    return err(DiagCode::UnsupportedFormat,
               "tracedb flow row uses unknown reserved fields");
  }
  return out;
}

template <class Row>
[[nodiscard]] Result<void> appendOrdered(std::vector<Row>& destination,
                                         std::vector<Row> rows,
                                         uint64_t Row::*id) {
  for (const Row& row : rows) {
    if (!destination.empty() && row.*id <= destination.back().*id) {
      return err(DiagCode::BadFormat,
                 "tracedb rows are not strictly ordered by trace id");
    }
    destination.push_back(row);
  }
  return ok();
}

}  // namespace

Result<Tracedb> Tracedb::open(const std::filesystem::path& root) {
  XDEC_TRY(json::Value manifest,
           json::parseFile((root / "manifest.json").string()));
  if (!manifest.isObject()) {
    return err(DiagCode::BadFormat, "tracedb manifest root is not an object");
  }
  XDEC_TRY(uint64_t version, unsignedMember(manifest, "version", true));
  if (version != kTracedbV5 && version != kTracedbV6) {
    return err(DiagCode::UnsupportedFormat,
               "unsupported tracedb manifest version {}", version);
  }
  const json::Value* threads = manifest.find("threads");
  if (threads == nullptr || !threads->isArray()) {
    return err(DiagCode::BadFormat, "tracedb manifest lacks a threads array");
  }

  Tracedb database;
  database.root_ = root;
  database.version_ = static_cast<uint32_t>(version);
  for (const json::Value& threadValue : threads->items()) {
    if (!threadValue.isObject()) {
      return err(DiagCode::BadFormat, "tracedb thread is not an object");
    }
    XDEC_TRY(uint64_t threadId,
             unsignedMember(threadValue, "thread_id", true));
    if (threadId == 0 || threadId > std::numeric_limits<uint32_t>::max()) {
      return err(DiagCode::BadFormat, "invalid tracedb thread id {}", threadId);
    }
    TracedbThread thread;
    thread.id = static_cast<uint32_t>(threadId);
    const json::Value* segments = threadValue.find("segments");
    if (segments == nullptr || !segments->isArray()) {
      return err(DiagCode::BadFormat,
                 "tracedb thread {} lacks a segments array", threadId);
    }
    for (const json::Value& segmentValue : segments->items()) {
      if (!segmentValue.isObject()) {
        return err(DiagCode::BadFormat, "tracedb segment is not an object");
      }
      TracedbSegment segment;
      XDEC_TRY(uint64_t id, unsignedMember(segmentValue, "id", true));
      XDEC_TRY(segment.minTraceId,
               unsignedMember(segmentValue, "min_trace_id", true));
      XDEC_TRY(segment.maxTraceId,
               unsignedMember(segmentValue, "max_trace_id", true));
      XDEC_TRY(segment.traceCount,
               unsignedMember(segmentValue, "n_trace", true));
      XDEC_TRY(segment.callGraphCount,
               unsignedMember(segmentValue, "n_callgraph", false));
      XDEC_TRY(segment.flowCount,
               unsignedMember(segmentValue, "n_flow", false));
      if (id > std::numeric_limits<uint32_t>::max() ||
          (segment.traceCount != 0 &&
           (segment.minTraceId == 0 ||
            segment.maxTraceId < segment.minTraceId))) {
        return err(DiagCode::BadFormat, "invalid tracedb segment metadata");
      }
      segment.id = static_cast<uint32_t>(id);
      thread.segments.push_back(segment);
    }
    std::ranges::sort(thread.segments, {}, &TracedbSegment::minTraceId);
    database.threads_.push_back(std::move(thread));
  }
  std::ranges::sort(database.threads_, {}, &TracedbThread::id);
  for (std::size_t i = 1; i < database.threads_.size(); ++i) {
    if (database.threads_[i - 1].id == database.threads_[i].id) {
      return err(DiagCode::BadFormat, "duplicate tracedb thread id {}",
                 database.threads_[i].id);
    }
  }
  return database;
}

const TracedbThread* Tracedb::findThread(uint32_t id) const noexcept {
  const auto found = std::ranges::lower_bound(threads_, id, {},
                                               &TracedbThread::id);
  return found != threads_.end() && found->id == id ? &*found : nullptr;
}

Result<std::vector<TraceRow>> Tracedb::readTrace(uint32_t threadId) const {
  const TracedbThread* thread = findThread(threadId);
  if (thread == nullptr) {
    return err(DiagCode::OutOfRange, "tracedb has no thread {}", threadId);
  }
  std::vector<TraceRow> result;
  for (const TracedbSegment& segment : thread->segments) {
    const auto path = segmentPath(root_, threadId, segment.id) / "trace.row";
    XDEC_TRY(std::vector<TraceRow> rows,
             readRows<TraceRow>(path, kTraceRowSize, segment.traceCount,
                                segment.traceCount != 0, decodeTrace));
    if (!rows.empty() &&
        (rows.front().traceId != segment.minTraceId ||
         rows.back().traceId != segment.maxTraceId)) {
      return err(DiagCode::BadFormat,
                 "tracedb stream '{}' does not match its declared id range",
                 path.string());
    }
    XDEC_TRY_VOID(appendOrdered(result, std::move(rows), &TraceRow::traceId));
  }
  return result;
}

Result<std::vector<CallGraphRow>> Tracedb::readCallGraph(
    uint32_t threadId) const {
  const TracedbThread* thread = findThread(threadId);
  if (thread == nullptr) {
    return err(DiagCode::OutOfRange, "tracedb has no thread {}", threadId);
  }
  std::vector<CallGraphRow> result;
  for (const TracedbSegment& segment : thread->segments) {
    const auto path =
        segmentPath(root_, threadId, segment.id) / "callgraph.row";
    XDEC_TRY(std::vector<CallGraphRow> rows,
             readRows<CallGraphRow>(path, kCallGraphRowSize,
                                    segment.callGraphCount,
                                    segment.callGraphCount != 0,
                                    decodeCallGraph));
    result.insert(result.end(), rows.begin(), rows.end());
  }
  return result;
}

Result<std::vector<FlowRow>> Tracedb::readFlow(uint32_t threadId) const {
  if (version_ < kTracedbV6) return std::vector<FlowRow>{};
  const TracedbThread* thread = findThread(threadId);
  if (thread == nullptr) {
    return err(DiagCode::OutOfRange, "tracedb has no thread {}", threadId);
  }
  std::vector<FlowRow> result;
  for (const TracedbSegment& segment : thread->segments) {
    const auto path = segmentPath(root_, threadId, segment.id) / "flow.row";
    XDEC_TRY(std::vector<FlowRow> rows,
             readRows<FlowRow>(path, kFlowRowSize, segment.flowCount,
                               segment.flowCount != 0, decodeFlow));
    XDEC_TRY_VOID(appendOrdered(result, std::move(rows), &FlowRow::traceId));
  }
  return result;
}

Result<std::vector<std::string>> Tracedb::readDictionary(
    std::string_view name) const {
  if (name.empty() || name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos) {
    return err(DiagCode::BadFormat, "invalid tracedb dictionary name");
  }
  XDEC_TRY(std::vector<uint8_t> bytes,
           readBytes(root_ / "dicts" / std::string{name}, true));
  if (bytes.size() < 12 || get32(bytes, 0) != 0x43494454u ||
      get32(bytes, 4) != 1) {
    return err(DiagCode::BadFormat, "invalid tracedb dictionary '{}'", name);
  }
  std::vector<std::string> result;
  std::size_t at = 12;
  while (at < bytes.size()) {
    if (bytes.size() - at < 4) {
      return err(DiagCode::BadFormat, "truncated tracedb dictionary '{}'", name);
    }
    const uint32_t size = get32(bytes, at);
    at += 4;
    if (size > bytes.size() - at) {
      return err(DiagCode::BadFormat, "truncated tracedb dictionary '{}'", name);
    }
    result.emplace_back(reinterpret_cast<const char*>(bytes.data() + at), size);
    at += size;
  }
  return result;
}

}  // namespace xdec::storage
