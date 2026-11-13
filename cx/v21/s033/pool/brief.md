Model gpt-6-luna, effort high: measurement experiment. Earlier job briefs are history, not instructions.

# Task: C++ completion dispatcher pool size 2 / 4 / 8 vs base, same session, 3 rounds (#1639)

## Background
- Decision-owner table: /root/jobs/fx/cx/v21/s033/disp2/report.md. The pool is `completion_dispatcher_t` in framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:53-65, size max(2, available_processor_count()) = 8 for the source pinned to CPU 0-7. Boost.Asio wakes one idle worker per post (scheduler.ipp:357-378, 674-685) and another one in do_run_one when the queue is still non-empty (scheduler.ipp:492-495). M5: 8 pool threads in the source spend 54 % of samples in sleep/wake, ~27 µs/msg.
- Harness to reuse (copy into this job folder, do not edit the originals): /root/jobs/fx/cx/v21/s033/psched8/ (configure-branch.sh, measure.sh, run-B.sh, processes.py, collect-22k.py, analyze-22k.py, summarize.py, measurement-conditions.json, report.md for the table shape).
- Base build: /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build (HEAD 168c679590), already built; do not rebuild.

## Measurement branch (code only here, never pushed)
Worktree /root/worktree/zlink-v21-s033/pool, branch perf/1639-pool-exp at 168c679590 (= base). Make one local commit:
1. In completion_dispatcher_t, if env `ZLINK_PERF_COMPLETION_POOL` is a positive integer use it as the worker count, otherwise keep the current rule unchanged.
2. Name each worker thread `zl-cmpl-<n>` with pthread_setname_np so collectors can attribute them.
Nothing else. Configure/build with a copy of psched8/configure-branch.sh pointed at this worktree and a build dir `.pool-build` (Release -O3 -DNDEBUG, probes OFF, same vcpkg/Core 1.23.0 prefixes), through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock). Verify the bench binaries are newer than the source and contain the env string (strings | grep) before measuring.

## Wait before measuring
A separate job (bstage) is measuring and explaining why base app-worker µs/msg differed 2× between two sessions with the same build and rate (psmeas shell launch 22.1–22.5 vs psched8 Python supervisor launch 11.5–11.8). Build now, but do not start measuring until /root/jobs/fx/cx/v21/s033/bstage/report.md exists. Read it and use the launch method it shows to be correct; if it is inconclusive, use the psched8 launch and say so.

## Measure
Variants: base (psmeas-base build), pool8, pool4, pool2 (pool build with ZLINK_PERF_COMPLETION_POOL set for both source and target processes). pool8 checks that the patch itself is neutral. 3 rounds; within each round run every variant back to back, rotating the order per round.
Per variant and round:
- 1 KiB Framework send-saturation and request-backpressure throughput (binding cells not needed).
- PM Spot p50/p99 (30 s, /pm/request readiness as in psched8 run-B.sh).
- 22k paced Framework request-backpressure cell (source CPU 0-7, target CPU 8-15): source-side `zl-cmpl-*` threads summed µs/msg and voluntary wakes/msg (base threads have no names: attribute base pool threads by the same thread set rule and explain the rule, or report base as the process total minus I/O and named roles and say so), source and target I/O sendto/msg, target app-worker and receive µs/msg, process-total µs/msg for both processes, target/source effective GHz.
Report each variant against base's own 3-round range in the same session. Never compare with numbers from earlier sessions.

## Rules
- Every measurement under `flock -o /root/jobs/fx/cx/measure.lock`, one at a time. Builds take the build slot only.
- No test or spec changes, no push, no GitHub, no PRs, no sub-agents or other AI tools. Do not delete raw outputs. Remove the .pool-build dir only after the report is written and only if nothing else references it.
- Open the raw files before writing any number. Progress to progress.md; write report.md once at the end.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/pool/report.md (Korean, short tables with raw paths)
