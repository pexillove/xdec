#include "xdec/obs/field.h"

#include <cmath>
#include <format>

namespace xdec::obs {

std::string FieldValue::text() const {
  return std::visit(
      [](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          return value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return value;
        } else {
          return std::format("{}", value);
        }
      },
      value_);
}

void FieldValue::appendJson(std::string& out) const {
  std::visit(
      [&out](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          out += "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          out += value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
          appendJsonString(out, value);
        } else if constexpr (std::is_same_v<T, double>) {
          if (std::isfinite(value)) {
            out += std::format("{}", value);
          } else {
            out += "null";
          }
        } else {
          out += std::format("{}", value);
        }
      },
      value_);
}

FieldValue hex(uint64_t value) { return FieldValue{std::format("{:#x}", value)}; }

void appendJsonString(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void appendJsonObject(std::string& out, const Fields& fields) {
  out += '{';
  bool first = true;
  for (const Field& field : fields) {
    if (!first) {
      out += ',';
    }
    first = false;
    appendJsonString(out, field.key);
    out += ':';
    field.value.appendJson(out);
  }
  out += '}';
}

}  // namespace xdec::obs
