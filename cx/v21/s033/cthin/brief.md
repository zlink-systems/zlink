# Job: read-only design study — a thin C++ request path (0.33.0 #1639)

Model/level: gpt-6.1-sol ultra — architecture choice across the public API, registry, lane and binding
layers; 대표님 asked for this study (2026-11-06). Read-only.
No sub-agents or other models (no Astra).

## Question
On request-backpressure 1K the C++ Framework runs at about 0.2 of the raw binding. Throughput is capped
by the source submitter thread: ~19 µs CPU per request, ~93% busy (main), vs ~3.5 µs for the binding's
whole request including the handler. Three rounds of local cuts (#3884, #3888, e86f55886a) moved it by a
few percent each. For 0.6 the submitter must reach ~6 µs. Which layers would have to go, and does the spec
allow it?

1. Walk one request through both paths, source side first (call -> wire -> Core submit; reply ->
   completion -> caller), then target side (receive -> handler -> reply). Table per step: what the
   binding does, what the Framework does (file:function), the spec clause that requires the Framework step
   (file:line) or "no clause", and measured cost where available. Use perf on condition B (source 0-7,
   target 8-15) for the costs; reuse runner scripts in /root/jobs/fx/cx/v21/s033/csubmit/.
2. Group the "no clause" steps into removable layers. For each: estimated µs saved, what replaces it,
   decision-location count before/after (must not grow), and risk.
3. Group the steps a spec clause requires. For each: is the clause's purpose achievable more cheaply
   (cite the clause's stated reason), or is it a fixed cost? Give the minimum submitter µs the current
   spec allows.
4. Answer plainly: can C++ reach 0.6 on request-backpressure 1K and send-saturation 1K within the current
   spec? If not, which clauses would have to change and what users would observe.

Inputs: /root/jobs/fx/cx/v21/s033/{cprof,coreio,fdesign,kdesign,zdesign,cadvice,csubmit,cdrain}/ reports;
spec root framework/doc/framework/common/spec/server/.

## Write limits and forbidden
Worktree /root/worktree/zlink-3867s is read-only for you (HEAD = main + e86f55886a). Temporary builds and
instrumentation only in copies under /root/jobs/fx/cx/v21/s033/cthin/. Forbidden: commit, push, branch,
stash, reset, any repository edit, spec edits, GitHub operations. C++ builds via
`/root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <cmd>` under `flock -s /root/jobs/fx/cx/measure.lock`;
measurements and perf under `flock -o`.

## Report
/root/jobs/fx/cx/v21/s033/cthin/report.md with the four answers, raw data paths, and what you could not
measure. Design only; no implementation.
