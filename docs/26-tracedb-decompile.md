# 26 — Tracedb-only decompilation

`xdec` can decompile exactly the control flow covered by one tracedb engine:

```text
xdec decompile app 0x100012340 --engine results/engine -o function.c
```

`--engine` changes the product deliberately. The result is the union of the
selected function's recorded invocations, not a claim about every path the
binary could take. Instructions and branch arms absent from the trace are not
lifted. Observed targets remain part of the function even when they are far
from its entry; a numeric `--max-span` is neither needed nor applied.

## Data used

The reader accepts tracedb v5 and v6:

- `trace.row` supplies executed instruction addresses;
- `callgraph.row` identifies the requested invocation and removes nested
  callees from the caller's instruction stream;
- v6 `flow.row` supplies authoritative concrete branch, call, and return
  outcomes directly from the interpreter.

For compatibility, a v5 engine derives caller transitions from thread-local
instruction order after nested call intervals have been removed. New traces
should use v6 because adjacency cannot describe modelled calls and callbacks
as precisely as interpreter outcomes.

Trace PCs are module offsets. Callgraph addresses are runtime addresses.
The reader infers the traced module base from each callsite/callee anchor and
rebases coverage onto the image loaded by xdec, so ASLR need not match.

## Static mode

Without `--engine`, `decompile` retains its static discovery pipeline. Its
advanced tuning flags remain accepted for existing scripts but are intentionally
not shown in the short help; trace decompilation needs only the engine path.

## Limits

The requested address must have a recorded callgraph invocation. Output from
trace mode is always marked partial in machine reports because unseen paths are
excluded by design. To include another path, capture an execution that takes it
and decompile the resulting engine.
