// Calls that do not follow the platform's calling convention, and how to name
// a call's target early enough to find out.
//
// Register SSA models every call the same way: x0..x7 are its arguments, and
// x0..x18/x30 plus q0..q7 are dead afterwards (see passes/ssa_construct.cpp).
// For an ordinary callee that is the ABI's own promise. A few runtime helpers
// are called under a private contract instead, and modelling them as ordinary
// calls is not merely imprecise, it erases the enclosing function's inputs:
//
//   - `__chkstk_darwin` (Apple) and `__chkstk` (Windows on Arm) probe the
//     stack for a large frame. The size travels in x15, and every register
//     except x16/x17 survives the call. Compilers emit it in the prologue,
//     *before* the incoming arguments are read, so treating it as an ordinary
//     call turns every argument register into "whatever the callee left" and
//     the function comes out as `f(void)`.
//
// The list is short and every entry is a documented compiler contract; a
// missing entry keeps the conservative model, a wrong one would keep a dead
// register alive, so nothing is added on a guess.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "xdec/il/function.h"
#include "xdec/support/reader.h"

namespace xdec::analysis {

/// Whether `name` (with or without the Mach-O `_` prefix) is a helper that
/// takes no argument-register inputs and preserves every register except the
/// intra-procedure scratch pair x16/x17.
[[nodiscard]] bool isRegisterPreservingHelper(std::string_view name) noexcept;

/// Whether the register named `name` is one a register-preserving helper may
/// destroy: x16, x17, and the link register the branch itself writes.
[[nodiscard]] bool registerPreservingHelperClobbers(std::string_view name) noexcept;

/// Where to look a call target up.
struct CallNameSources {
  /// The plain image, for seeing through a PLT/stub a direct call targets.
  const ByteReader* image = nullptr;
  const MemoryFacts* memory = nullptr;
  /// The image's symbol table, for a direct call to a named function.
  std::function<std::string(uint64_t va)> symbolAt;
  /// Applied to every expression before it is evaluated. SSA construction
  /// needs this: it asks mid-rename, before register reads have been
  /// replaced by their versions in the IL itself.
  std::function<il::ExprId(il::ExprId)> rewrite;
};

/// The name of the function a call target reaches, from a direct address (its
/// symbol, or the import behind a stub at it) or from a `Load` of a
/// loader-bound GOT slot. Addresses are folded through casts and constant
/// `Add`/`Sub`, so an unoptimised `adrp`+`ldr` pair still names its slot.
[[nodiscard]] std::optional<std::string> callTargetName(const il::Function& function,
                                                        il::ExprId target,
                                                        const CallNameSources& sources);

}  // namespace xdec::analysis
