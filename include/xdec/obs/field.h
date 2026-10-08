// Key/value attributes shared by log records, spans, and progress events.
//
// Keys are string literals by contract: a record may sit in a ring buffer long
// after the code that produced it returned, so a key must outlive everything.
// Values are typed so a JSON consumer (a GUI, the run report, eval tooling)
// gets numbers as numbers; addresses go through hex(), because a 64-bit VA
// does not survive a round trip through a JavaScript number.
#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace xdec::obs {

class FieldValue {
 public:
  using Storage = std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string>;

  FieldValue() = default;
  FieldValue(bool value) : value_(value) {}  // NOLINT
  template <std::signed_integral T>
  FieldValue(T value) : value_(static_cast<int64_t>(value)) {}  // NOLINT
  template <std::unsigned_integral T>
    requires(!std::same_as<T, bool>)
  FieldValue(T value) : value_(static_cast<uint64_t>(value)) {}  // NOLINT
  FieldValue(double value) : value_(value) {}                    // NOLINT
  FieldValue(std::string value) : value_(std::move(value)) {}    // NOLINT
  FieldValue(std::string_view value) : value_(std::string{value}) {}  // NOLINT
  FieldValue(const char* value) : value_(std::string{value}) {}       // NOLINT

  [[nodiscard]] const Storage& storage() const noexcept { return value_; }
  /// Human-readable rendering, used by the text sink.
  [[nodiscard]] std::string text() const;
  /// Appends the value as a JSON literal.
  void appendJson(std::string& out) const;

 private:
  Storage value_;
};

struct Field {
  std::string_view key;
  FieldValue value;
};

using Fields = std::vector<Field>;

/// `0x...` spelling of an address, as a string value.
[[nodiscard]] FieldValue hex(uint64_t value);

namespace detail {
inline void appendFields(Fields&) {}
template <class V, class... Rest>
void appendFields(Fields& out, std::string_view key, V&& value, Rest&&... rest) {
  out.push_back(Field{key, FieldValue(std::forward<V>(value))});
  appendFields(out, std::forward<Rest>(rest)...);
}
}  // namespace detail

/// kv("round", 3, "entry", hex(va)) -- alternating literal keys and values.
template <class... Args>
[[nodiscard]] Fields kv(Args&&... args) {
  static_assert(sizeof...(Args) % 2 == 0, "kv() takes key/value pairs");
  Fields out;
  out.reserve(sizeof...(Args) / 2);
  detail::appendFields(out, std::forward<Args>(args)...);
  return out;
}

/// Appends `text` as a quoted, escaped JSON string.
void appendJsonString(std::string& out, std::string_view text);

/// Appends `{"k":v,...}`.
void appendJsonObject(std::string& out, const Fields& fields);

}  // namespace xdec::obs
