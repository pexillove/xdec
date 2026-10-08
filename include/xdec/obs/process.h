// Process-level hooks: crash dumps and Ctrl+C.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "xdec/obs/job.h"
#include "xdec/obs/sinks.h"

namespace xdec::obs {

struct CrashOptions {
  /// Where `xdec-crash-<pid>.txt` is written.
  std::filesystem::path directory = ".";
  /// Recent log records to include; may be null.
  std::shared_ptr<RingBufferSink> recent;
};

/// On an unhandled exception, std::terminate, or a fatal signal / SEH
/// exception, writes a crash report (reason, every thread's spans and context,
/// recent log records, counters, memory) before the process dies. Best effort:
/// the process is already broken when this runs.
void installCrashHandlers(CrashOptions options);

/// Writes the same report on demand; returns its path, empty on failure.
std::filesystem::path writeCrashReport(std::string_view reason);

/// Routes SIGINT (Ctrl+C) to `job.interrupt()`: the first winds the run down
/// to a partial result, the second cancels it, the third exits immediately.
void installInterruptHandler(Job& job);
void uninstallInterruptHandler();

}  // namespace xdec::obs
