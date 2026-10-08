// log-categories: everything else that does not fit another domain.
#include "common.h"
#include "xdec/obs/log.h"

namespace xdec::cli {

int commandLogCategories() {
  printLine("logging categories (set with --log name=level or XDEC_LOG=name=level):");
  for (const auto* category : xdec::obs::logCategories()) {
    print("  {:<12} {:<6} (default {})", category->name(), toString(category->level()),
          toString(category->defaultLevel()));
  }
  return 0;
}

}  // namespace xdec::cli
