// Shared, dependency-free CLI plumbing: output, number parsing, and the help
// text. Every command file in cli/ builds on this without pulling in the
// binary/spec/analysis layers that session.h needs.
//
// Output contract: stdout carries a command's *result* (the C source, a
// listing, a requested report) and nothing else, so it can be piped. Everything
// the tool says about the run -- status, notes, warnings, errors -- goes
// through the `cli` log category (note/warn/printError below), which lands on
// stderr by default and in `--log-file`, the run report and `--machine`'s
// JSONL stream like any other record.
#pragma once

#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

#include "xdec/obs/log.h"
#include "xdec/support/diag.h"

namespace xdec::cli {

XDEC_DECLARE_LOG_CATEGORY(cliLog)

/// A line of result output on stdout.
void printLine(std::string_view text);

template <class... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
  printLine(std::format(fmt, std::forward<Args>(args)...));
}

namespace detail {
void say(obs::LogLevel level, std::string message);
}

/// Status the user should see (stderr, `cli` category at info).
template <class... Args>
void note(std::format_string<Args...> fmt, Args&&... args) {
  detail::say(obs::LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

/// `warning: ...` on stderr.
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  detail::say(obs::LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

/// `error: ...` on stderr.
template <class... Args>
void printError(std::format_string<Args...> fmt, Args&&... args) {
  detail::say(obs::LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

/// Parses a decimal or 0x-prefixed hexadecimal integer.
bool parseNumber(std::string_view text, uint64_t& out);

/// Prints the diagnostic and returns the exit code for it: 130 for a
/// cancelled run, 124 for a timed-out one, 1 otherwise.
int reportError(const xdec::Diag& diag);

/// Prints the full `xdec <command>` usage text and returns the exit code
/// every no-args / unknown-command path uses.
int usage();

}  // namespace xdec::cli
