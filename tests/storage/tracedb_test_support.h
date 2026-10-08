#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace xdec::test::tracedb {

inline void put32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

inline void put64(uint8_t* out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

class Engine {
 public:
  explicit Engine(std::string_view name)
      : root_(std::filesystem::temp_directory_path() /
              ("xdec-tracedb-test-" + std::string{name})) {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
    std::filesystem::create_directories(segment());
    std::filesystem::create_directories(root_ / "dicts");
  }

  ~Engine() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }
  [[nodiscard]] std::filesystem::path segment() const {
    return root_ / "threads" / "thr_00000001" / "segments" / "seg_00000000";
  }

  void manifest(uint32_t version, uint64_t traces, uint64_t calls,
                uint64_t flows = 0) const {
    std::ofstream out{root_ / "manifest.json", std::ios::binary};
    out << "{\"version\":" << version
        << ",\"threads\":[{\"thread_id\":1,\"segments\":[{\"id\":0,"
           "\"min_trace_id\":1,\"max_trace_id\":"
        << traces << ",\"n_trace\":" << traces
        << ",\"n_callgraph\":" << calls;
    if (version >= 6) out << ",\"n_flow\":" << flows;
    out << "}]}]}";
  }

  void trace(const std::vector<uint64_t>& pcs) const {
    std::ofstream out{segment() / "trace.row", std::ios::binary};
    for (std::size_t i = 0; i < pcs.size(); ++i) {
      std::array<uint8_t, 64> row{};
      put64(row.data(), i + 1);
      put64(row.data() + 8, pcs[i]);
      put32(row.data() + 24, static_cast<uint32_t>(i % 2));
      put64(row.data() + 56, 100 + i);
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }

  void calls(const std::vector<std::array<uint64_t, 8>>& values) const {
    std::ofstream out{segment() / "callgraph.row", std::ios::binary};
    for (const auto& value : values) {
      std::array<uint8_t, 64> row{};
      for (std::size_t i = 0; i < 7; ++i) put64(row.data() + i * 8, value[i]);
      put32(row.data() + 56, static_cast<uint32_t>(value[7] & 0xffff));
      row[60] = static_cast<uint8_t>(value[7] >> 32);
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }

  void flow(
      const std::vector<std::array<uint64_t, 4>>& values) const {
    std::ofstream out{segment() / "flow.row", std::ios::binary};
    for (const auto& value : values) {
      std::array<uint8_t, 40> row{};
      put64(row.data(), value[0]);
      put64(row.data() + 8, value[0]);
      put64(row.data() + 16, value[1]);
      put64(row.data() + 24, value[2]);
      row[32] = static_cast<uint8_t>(value[3]);
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }

 private:
  std::filesystem::path root_;
};

}  // namespace xdec::test::tracedb
