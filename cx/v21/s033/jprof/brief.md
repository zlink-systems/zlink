# Task: current stage profile of Java Framework vs binding — small-message and request-serial paths (issues #3870, #3871)

Cells (ratio040, main ee11390257, 2026-11-05), Framework vs binding msg/s and ratio:
- send-saturation 1024: 114,094 vs 311,460 (0.366); 4096 passes (0.632)
- request-backpressure 1024: 57,145 vs 124,104 (0.460); 4096 passes (0.619)
- request-serial 1024: 8,651 vs 14,862 (0.582); 4096: 8,125 vs 14,037 (0.579)
4K cells passing while 1K fails means a fixed per-message cost; request-serial is a latency (round-trip) gap.
Target: Framework/binding >= 0.6 in these five cells. Kotlin uses the same Java runtime.
Result JSON with per-thread counters: /root/worktree/zlink-ratio040/framework/bench/grpc/log/ratio040/java/run*/.

Previous Java evidence on older mains (reuse methods and tools; do not reuse numbers): /root/jobs/fx/cx/v21/prof-jv/, gap-jv/, p032jbp/, p032jvn/ (brief.md, report.md, scripts). The request-serial 4K drop was checked by A/B in /root/jobs/fx/cx/j3871/ (no commit cause).
C++ equivalent on current main for comparison of method: /root/jobs/fx/cx/v21/s033/cprof/report.md.

## Rules
- Evidence only: do NOT modify tracked files in the repo. Scripts, copies of run scripts, traces and builds go under /root/jobs/fx/cx/v21/s033/jprof/ only.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree: /root/worktree/zlink-ratio040 (main ee11390257 + uncommitted pinning in */run_local.sh — keep it: target CPUs 8-15, source CPUs 0-7). The Java bench is already built there; do not rebuild in place. Run `/root/jobs/fx/cx/bench-preflight.sh /root/worktree/zlink-ratio040 java` before your first run; if it fails, stop and write that in report.md.
- Measurement exclusivity: wrap EVERY bench/profile/trace run in `flock -o /root/jobs/fx/cx/measure.lock ...`; another job may hold it for a while, so wait on it. Never run two at once.
- One cell at a time with run_local.sh env: PATTERNS, PAYLOAD_SIZES=1024, IMPLEMENTATIONS, RUNS=1, WARMUP_SECONDS, DURATION_SECONDS, SKIP_BUILD=1, RUN_STAMP, OUTPUT_DIR under your job dir. Profile send-saturation 1024, request-backpressure 1024 and request-serial 1024, Framework and binding, both processes.
- Tools: async-profiler (cpu, alloc and lock modes) and/or JFR, plus perf stat as used in the earlier jobs; attach only during the active window. Also record one un-profiled run per implementation and pattern and report its msg/s next to the ratio040 value.
- Keep MemAvailable >= 12 GiB. Delete raw trace files over 1 GB after extracting summaries.
- Time box: about 90 minutes. Write the report even if items are missing; mark them "not measured".

## Output: /root/jobs/fx/cx/v21/s033/jprof/report.md (exact path)
Framework vs binding side by side, for each of the three patterns:
1. Per-thread table: thread role (pool worker, socket/pump, timer, GC, bench driver), CPU µs per message, share of a core. Name the saturated or critical-path thread; for request-serial (one outstanding request), give the latency breakdown per round trip instead: time from submit to wire send, network/peer, receive to completion, completion to next submit.
2. Per-stage µs/message for the critical thread and each whole process: allocation and GC, thread hand-off and wake (monitor/lock contention, LockSupport park/unpark, executors, queues), CompletableFuture/callback objects, codec (protobuf, envelope header, service wire), binding/Core call (JNI/FFM, kernel send/recv), Framework bookkeeping (registry, admission, deadline), benchmark driver, other. State how functions map to stages.
3. Counts per message: context switches, lock contention starts (by lock), allocations (bytes and count, method stated), GCs per 1M messages, syscalls.
4. Top 20 Framework-only methods on the critical thread (self and inclusive µs/message, file:method).
5. Ranked candidate changes: for each, the stage removed, file:method, the µs/message it can remove at most on the critical path, and the resulting upper-bound ratio. Say which single change you would implement first.
