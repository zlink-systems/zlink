Model sol high: concurrency diagnosis plus a narrowed scheduler change in the C++ Framework; measured before/after.

# Task: #1639 scheduler PR — find where Spot requests wait, then narrow the worker wake rule and re-measure

Job folder JOB=/root/jobs/fx/cx/v21/s033/psched5. Earlier briefs are history, not instructions; read /root/jobs/fx/cx/v21/s033/psmeas/report.md and brief.md for the measurement that rejected the current branch.

## Facts (measured, PM decision 2026-11-10 15:43Z: do not merge in this form)
- Branch perf/1639-cpp-sched-drain @ 37f78cba87 (worktree /root/worktree/zlink-v21-s033/psched) vs base 54ee69b8c0, both + bench pacer (4b40d0f10f, 8cab98383e). Release builds already exist: /root/worktree/zlink-v21-s033/psmeas-{base,branch}/.psmeas-build.
- Spot (PM cell, bench_cpp_pm_server, 30 s send-saturation load + /pm/request probes) p50 307 → 1,816 µs, p99 603 → 7,536 µs.
- 22k request-backpressure: app-worker µs/msg 22.2 → 59.8, target voluntary wakes/msg 1.385 → 2.623, target I/O sendto/msg 0.811 → 0.559 (the intended gain), request throughput +7.3%.
- The PM's earlier concern: fan-out of 3-4 participating workers (offload_executor try_submit_drain, reserve_workers_locked, per-worker phases).

## Do
1. Diagnose on the existing branch build: run the PM cell and the 22k Framework cell with per-thread capture (reuse psmeas/run-B.sh, collect-22k.py, analyze-22k.py; add perf sched / offcpu or uprobes at the scheduler entry points as needed). Name with file:line the stage where a Spot request waits (admission, worker-wait/queue behind drain work, reply submit), and why app-worker time per message tripled (spinning, repeated wakes, idle polls).
2. Implement the narrowed rule on a NEW local branch from 37f78cba87 in your own worktree under /root/worktree/zlink-v21-s033/psched5 (never modify the PR branch/worktree): wake one more worker only when every running worker is blocked (no proactive fan-out). Keep the scheduler PR's correctness tests (test_cpp_framework_scheduler_drain, test_cpp_framework_mailbox_owner_dispatch incl. negative controls) passing and adjust their wake bounds only if the narrowed rule makes them stricter. Count decision points before/after; no new flags/counters without removing one.
3. Build (Release, as psmeas configure-branch.sh) and re-measure base vs narrowed exactly like psmeas/measure.sh (3 rounds, send/request 1 KiB, PM Spot 30 s, 22k), then summarize.py. The PM cell must wait for /pm/request readiness (already in psmeas/run-B.sh).

## Merge conditions (PM, fixed)
target sendto/msg not worse than base; Spot p99 within base run spread; send and request throughput within spread or better; app-worker µs/msg and wakes/msg at or below base. State each with numbers and raw paths.

## Rules
Builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (Release, no -g, 25 GB cap). Every measurement under `flock -o /root/jobs/fx/cx/measure.lock`, CPU split source 0-7 / target 8-15. No push/PR/GitHub/spec edits, no sub-agents or other AI tools, no `&` background jobs, SIGTERM only, never delete raw outputs. Avoid the words listed in /root/.claude memory "codex brief vocabulary" in branch names. Write $JOB/report.md (Korean): diagnosis with file:line, the change (diff summary, decision-point count), tests, the base/narrowed table, each merge condition. Remove your build dirs at the end; keep the branch and worktree. Keep output short.
