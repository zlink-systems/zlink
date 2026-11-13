# Task: current stage profile of Node Framework vs binding — send-saturation 4096 and borderline cells (issue #3872)

Cells (ratio040, main ee11390257, 2026-11-05), Framework vs binding msg/s and ratio:
- send-saturation 4096: 54,892 vs 91,921 (0.597) — fails
- borderline passes: request-serial 4096 0.601, request-serial 1024 0.605, request-backpressure 1024 0.620
Target: every Node cell >= 0.6 with margin (aim >= 0.65), since these sit at the noise edge.
Result JSON with per-thread counters: /root/worktree/zlink-ratio040/framework/bench/grpc/log/ratio040/node/run*/.

Previous Node evidence on older mains (reuse methods and tools; do not reuse numbers): /root/jobs/fx/cx/v21/gap-nd/, p032n4k/ (brief.md, report.md, scripts).
C++ equivalent on current main for comparison of method: /root/jobs/fx/cx/v21/s033/cprof/report.md.

## Rules
- Evidence only: do NOT modify tracked files in the repo. Scripts, copies of run scripts, traces and builds go under /root/jobs/fx/cx/v21/s033/nprof/ only.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree: /root/worktree/zlink-ratio040 (main ee11390257 + uncommitted pinning in */run_local.sh — keep it: target CPUs 8-15, source CPUs 0-7). The Node bench is already built there; do not rebuild in place. Run `/root/jobs/fx/cx/bench-preflight.sh /root/worktree/zlink-ratio040 node` before your first run; if it fails, stop and write that in report.md.
- Measurement exclusivity: wrap EVERY bench/profile/trace run in `flock -o /root/jobs/fx/cx/measure.lock ...`; another job may hold it for a while, so wait on it. Never run two at once.
- One cell at a time with run_local.sh env: PATTERNS, PAYLOAD_SIZES as needed, IMPLEMENTATIONS, RUNS=1, WARMUP_SECONDS, DURATION_SECONDS, SKIP_BUILD=1, RUN_STAMP, OUTPUT_DIR under your job dir. Profile send-saturation 4096 and request-serial 4096, Framework and binding, both processes.
- Tools: node --cpu-prof / --perf-basic-prof with perf, and --trace-gc or heap sampling for allocation as used in the earlier jobs; attach only during the active window. Also record one un-profiled run per implementation and pattern and report its msg/s next to the ratio040 value.
- Keep MemAvailable >= 12 GiB. Delete raw trace files over 1 GB after extracting summaries.
- Time box: about 90 minutes. Write the report even if items are missing; mark them "not measured".

## Output: /root/jobs/fx/cx/v21/s033/nprof/report.md (exact path)
Framework vs binding side by side, for each of the two patterns:
1. Per-thread table: thread role (main event loop, libuv threads, binding native threads, GC helpers, bench driver), CPU µs per message, share of a core. Name the saturated or critical-path thread; for request-serial (one outstanding request), give the latency breakdown per round trip instead: time from submit to wire send, network/peer, receive to completion, completion to next submit.
2. Per-stage µs/message for the critical thread and each whole process: allocation and GC, thread hand-off and wake (event-loop turns, microtask/promise queues, native-to-JS callbacks), Promise/async function objects, codec (protobuf, envelope header, service wire), binding/Core call (N-API calls, kernel send/recv), Framework bookkeeping (registry, admission, deadline), benchmark driver, other. State how functions map to stages.
3. Counts per message: context switches, event-loop turns and native callbacks per message, allocations (bytes and count, method stated), GCs per 1M messages, syscalls.
4. Top 20 Framework-only methods on the critical thread (self and inclusive µs/message, file:method).
5. Ranked candidate changes: for each, the stage removed, file:method, the µs/message it can remove at most on the critical path, and the resulting upper-bound ratio. Say which single change you would implement first.
