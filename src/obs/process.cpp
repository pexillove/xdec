#include "xdec/obs/process.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>

#include "xdec/obs/clock.h"
#include "xdec/obs/metrics.h"
#include "xdec/obs/threads.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace xdec::obs {
namespace {

struct CrashState {
  CrashOptions options;
  std::atomic<bool> installed{false};
  std::atomic_flag writing = ATOMIC_FLAG_INIT;
};

CrashState& crashState() {
  static auto* state = new CrashState;
  return *state;
}

int processId() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

std::string currentExceptionText() {
  const std::exception_ptr current = std::current_exception();
  if (!current) {
    return "std::terminate called without an active exception";
  }
  try {
    std::rethrow_exception(current);
  } catch (const std::exception& exception) {
    return std::format("unhandled exception: {}", exception.what());
  } catch (const CancelledError& cancelled) {
    return std::format("unhandled cancellation ({}) at {}", toString(cancelled.reason()),
                       cancelled.where());
  } catch (...) {
    return "unhandled exception of unknown type";
  }
}

void onTerminate() {
  writeCrashReport(currentExceptionText());
  std::abort();
}

#if defined(_WIN32)
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
  const DWORD code = info != nullptr && info->ExceptionRecord != nullptr
                         ? info->ExceptionRecord->ExceptionCode
                         : 0;
  const void* address = info != nullptr && info->ExceptionRecord != nullptr
                            ? info->ExceptionRecord->ExceptionAddress
                            : nullptr;
  writeCrashReport(std::format("SEH exception {:#010x} at {}", static_cast<unsigned>(code),
                               address));
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void onFatalSignal(int signal) {
  writeCrashReport(std::format("fatal signal {}", signal));
  std::signal(signal, SIG_DFL);
  std::raise(signal);
}

std::atomic<Job*> gInterruptJob{nullptr};

void onInterrupt(int) {
  Job* job = gInterruptJob.load();
  if (job == nullptr) {
    std::_Exit(130);
  }
  job->interrupt();
  if (job->interruptCount() >= 3) {
    std::_Exit(130);
  }
  std::signal(SIGINT, onInterrupt);
}

}  // namespace

std::filesystem::path writeCrashReport(std::string_view reason) {
  CrashState& state = crashState();
  if (state.writing.test_and_set()) {
    return {};
  }
  const std::filesystem::path path =
      state.options.directory / std::format("xdec-crash-{}.txt", processId());
#if defined(_WIN32)
  std::FILE* file = _wfopen(path.c_str(), L"wb");
#else
  std::FILE* file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return {};
  }
  const uint64_t now = monotonicNs();
  std::string text = std::format("xdec crash report\nreason: {}\nuptime: {:.3f}s\n\n", reason,
                                 static_cast<double>(now) / 1e9);
  text += "== threads ==\n";
  text += formatThreadSnapshots(threadSnapshots(/*tryOnly=*/true), now);
  text += "\n== recent log ==\n";
  if (state.options.recent) {
    for (const LogRecord& record : state.options.recent->trySnapshot()) {
      text += TextSink::format(record, TextSink::Options{.showTime = true,
                                                          .showContext = true,
                                                          .showSource = true});
    }
  }
  text += "\n== counters ==\n";
  for (const auto& [name, value] : counterSnapshot()) {
    text += std::format("  {} = {}\n", name, value);
  }
  const MemoryUsage memory = processMemory();
  text += std::format("\n== memory ==\n  current {} MiB, peak {} MiB\n", memory.currentBytes >> 20,
                      memory.peakBytes >> 20);
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
  const std::string notice = std::format("xdec: crash report written to '{}'\n", path.string());
  std::fwrite(notice.data(), 1, notice.size(), stderr);
  return path;
}

void installCrashHandlers(CrashOptions options) {
  CrashState& state = crashState();
  state.options = std::move(options);
  if (state.installed.exchange(true)) {
    return;
  }
  std::set_terminate(onTerminate);
#if defined(_WIN32)
  SetUnhandledExceptionFilter(onUnhandledException);
  std::signal(SIGABRT, onFatalSignal);
#else
  for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
    std::signal(signal, onFatalSignal);
  }
#endif
}

void installInterruptHandler(Job& job) {
  gInterruptJob.store(&job);
  std::signal(SIGINT, onInterrupt);
}

void uninstallInterruptHandler() {
  std::signal(SIGINT, SIG_DFL);
  gInterruptJob.store(nullptr);
}

}  // namespace xdec::obs
