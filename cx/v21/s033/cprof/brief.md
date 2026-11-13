# Task: current stage profile of C++ Framework vs binding — request-backpressure 1024 (issue #3867)

Cell (ratio040, main ee11390257, 2026-11-05): request-backpressure 1024 Framework 36,315 msg/s vs binding 220,467 (0.165); 4096: 35,570 vs 140,737 (0.253). Result JSON with per-thread core counters: /root/worktree/zlink-ratio040/framework/bench/grpc/log/ratio040/cpp/run*/.
Previous static analysis (no profiler; counts and candidates R, W, T, M, H): /root/jobs/fx/cx/ratio034/gap14-analysis.md, first section. It concludes "No complete post-merge stage-time profile exists" — this task supplies that profile.
Previous profiling method and scripts on an older main (reuse the method; do not reuse its numbers): /root/jobs/fx/cx/v21/prof-cpp/ (brief.md, run.sh, instrumented/run_local.sh, analyze.py, alloc_counter.c, report.md).

## Rules
- Evidence only: do NOT modify tracked files in the repo. Scripts, copies of run scripts, profiles and builds go under /root/jobs/fx/cx/v21/s033/cprof/ only.
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree: /root/worktree/zlink-ratio040 (main ee11390257 + uncommitted pinning in */run_local.sh — keep it: target CPUs 8-15, source CPUs 0-7). C++ bench binaries are already built from this commit; do not rebuild them in place. Run `/root/jobs/fx/cx/bench-preflight.sh /root/worktree/zlink-ratio040 cpp` before your first run; if it fails, stop and write that in report.md.
- Measurement exclusivity: wrap EVERY bench/profile run in `flock -o /root/jobs/fx/cx/measure.lock ...`; another job may hold it for a while, so just wait on it. Never run two at once. A separate build (for example with frame pointers) goes into your job directory only, via `/root/jobs/fx/cx/cpp-build-slot.sh` with `--parallel 3`, under `flock -s /root/jobs/fx/cx/measure.lock`.
- One cell: copy framework/bench/grpc/cpp/run_local.sh into your job dir if needed; use PATTERNS=request-backpressure PAYLOAD_SIZES=1024, IMPLEMENTATIONS, RUNS=1, WARMUP_SECONDS, DURATION_SECONDS, SKIP_BUILD=1, RUN_STAMP, OUTPUT_DIR under your job dir.
- Profile both the Framework run and the binding run, both processes (source and target), profiler attached only during the active window (perf `--call-graph dwarf` with a short window, or a frame-pointer build). Also record one un-profiled run per implementation and report its msg/s next to the ratio040 value.
- Keep MemAvailable >= 12 GiB. Delete raw profile files over 1 GB after extracting summaries.
- Time box: about 90 minutes. Write the report even if items are missing; mark them "not measured".

## Output: /root/jobs/fx/cx/v21/s033/cprof/report.md (exact path)
Framework vs binding side by side:
1. Per-thread table: thread role, CPU µs per message, share of a core. Name the saturated (critical-path) thread.
2. Per-stage table for the critical thread and for each whole process, µs/message: allocation (malloc/free/new), lane/worker hand-off and wake (futex, condition variables, queues), task/coroutine/result-state objects, codec (protobuf, envelope header, service wire), binding/Core call (including kernel send/recv), Framework bookkeeping (registry, admission, deadline), benchmark driver, other. State how functions map to stages.
3. Counts per message: context switches, futex calls, heap allocations (state the method), syscalls.
4. Top 20 Framework-only functions on the critical thread (self and inclusive µs/message, file:function).
5. Re-rank candidates R, W, T, M, H from the previous analysis using measured µs: stage removed, measured µs it could remove at most on the critical thread, resulting upper-bound ratio. Add any cost the profile reveals that the analysis missed, with file:function.
End with a short summary: what fraction of the gap to 0.6 each top stage represents, and the one change you would implement first.
