# Task: current stage profile of .NET Framework vs binding — request paths (issue #3869)

Cells (ratio040, main ee11390257, 2026-11-05), Framework vs binding msg/s and ratio:
- request-backpressure 1024: 29,571 vs 58,990 (0.501); 4096: 23,943 vs 49,454 (0.484)
- request-serial 1024: 5,825 vs 11,303 (0.515); 4096: 5,557 vs 11,320 (0.491)
- send-saturation passes (0.774 / 0.837), so the gap is on the request/reply path.
Target: Framework/binding >= 0.6 in all four request cells.
Result JSON with per-thread counters: /root/worktree/zlink-ratio040/framework/bench/grpc/log/ratio040/dotnet/run*/.

Previous .NET evidence on older mains (reuse methods and tools; do not reuse numbers):
- /root/jobs/fx/cx/v21/prof-dn/ (brief.md, run_cell.sh, analyze_profiles.py, report.md) — stage profile method.
- /root/jobs/fx/cx/v21/p032net/ and p032net2/ (report.md, capture_contention.sh, tools/) — contention capture: `ZLinkProcessExecutionPool._gate` had ~7.8 contention starts per message in request-serial and ~0.6 in request-backpressure, attributed to `RunWorker`. First check on current main whether that lock and code are unchanged (git log/blame of ZLinkProcessExecutionPool.cs since that report) and re-measure it.
- C++ equivalent on current main for comparison of method: /root/jobs/fx/cx/v21/s033/cprof/report.md.

## Rules
- Evidence only: do NOT modify tracked files in the repo. Scripts, copies of run scripts, traces and builds go under /root/jobs/fx/cx/v21/s033/dprof/ only.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree: /root/worktree/zlink-ratio040 (main ee11390257 + uncommitted pinning in */run_local.sh — keep it: target CPUs 8-15, source CPUs 0-7). The .NET bench is already built there; do not rebuild in place. Run `/root/jobs/fx/cx/bench-preflight.sh /root/worktree/zlink-ratio040 dotnet` before your first run; if it fails, stop and write that in report.md.
- Measurement exclusivity: wrap EVERY bench/profile/trace run in `flock -o /root/jobs/fx/cx/measure.lock ...`; another job may hold it for a while, so wait on it. Never run two at once.
- One cell at a time with run_local.sh env: PATTERNS, PAYLOAD_SIZES=1024, IMPLEMENTATIONS, RUNS=1, WARMUP_SECONDS, DURATION_SECONDS, SKIP_BUILD=1, RUN_STAMP, OUTPUT_DIR under your job dir. Profile request-backpressure 1024 and request-serial 1024, Framework and binding, both processes.
- Tools: perf with perf maps (DOTNET_PerfMapEnabled=1) and/or dotnet-trace/dotnet-counters as used in the earlier jobs; attach only during the active window. Also record one un-profiled run per implementation and pattern and report its msg/s next to the ratio040 value.
- Keep MemAvailable >= 12 GiB. Delete raw trace files over 1 GB after extracting summaries.
- Time box: about 90 minutes. Write the report even if items are missing; mark them "not measured".

## Output: /root/jobs/fx/cx/v21/s033/dprof/report.md (exact path)
Framework vs binding side by side, for each of the two patterns:
1. Per-thread table: thread role (pool worker, socket/pump, timer, GC, bench driver), CPU µs per message, share of a core. Name the saturated or critical-path thread; for request-serial (one outstanding request), give the latency breakdown per round trip instead: time from submit to wire send, network/peer, receive to completion, completion to next submit.
2. Per-stage µs/message for the critical thread and each whole process: allocation and GC, thread hand-off and wake (Monitor/lock contention, SemaphoreSlim, ThreadPool, channel/queue), Task/ValueTask/async state machines, codec (protobuf, envelope header, service wire), binding/Core call (P/Invoke, kernel send/recv), Framework bookkeeping (registry, admission, deadline), benchmark driver, other. State how functions map to stages.
3. Counts per message: context switches, lock contention starts (by lock), allocations (bytes and count, method stated), GCs per 1M messages, syscalls.
4. Top 20 Framework-only methods on the critical thread (self and inclusive µs/message, file:method).
5. Ranked candidate changes: for each, the stage removed, file:method, the µs/message it can remove at most on the critical path, and the resulting upper-bound ratio. Say which single change you would implement first.
