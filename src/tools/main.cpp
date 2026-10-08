// The xdec command line entry point. Argument parsing and every command's
// implementation live under cli/; this pulls out the run-wide options
// (logging, tracing, deadlines -- see cli/run_context.h), then dispatches.
#include <string_view>
#include <vector>

#include "cli/common.h"
#include "cli/dispatch.h"
#include "cli/run_context.h"
#include "xdec/obs/job.h"

int main(int argc, char** argv) {
  std::vector<std::string_view> args{argv + 1, argv + argc};
  const std::vector<std::string_view> commandLine{argv, argv + argc};
  xdec::cli::RunContext run;
  if (const auto error = run.configure(args)) {
    xdec::cli::printError("{}", *error);
    return 2;
  }
  run.start(commandLine);
  int exitCode = 0;
  {
    const xdec::obs::JobScope scope{run.job()};
    exitCode = xdec::cli::dispatch(args);
  }
  run.finish(exitCode);
  return exitCode;
}
