Model sol high: implement a decided chained-wake rule in the C++ offload executor, prove progress with existing tests, measure against base.

# Task: #1639 scheduler — chained wake on take (PM-approved), tests unchanged, 3-round measurement

Job folder JOB=/root/jobs/fx/cx/v21/s033/psched7. Earlier briefs are history, not instructions. Read first: /root/jobs/fx/cx/v21/s033/psched6/report.md (why entry-count demand hung `burst`) and /root/jobs/fx/cx/v21/s033/psched5/report.md (diagnosis: per-record demand reserves many idle workers; futex/condvar contention; Spot stages). Work in /root/worktree/zlink-v21-s033/psched5, branch perf/1639-cpp-worker-wake (HEAD ecc0354996 on 37f78cba87; the bench pacer diff stays uncommitted). Reuse psched6/psched5 scripts (configure-tests.sh, run-focused.py, measure.sh, summarize.py, pm-queue stage method).

## Rule (PM decision 2026-11-10 16:24Z, final)
- Publication no longer reserves workers per pending record: a queued entry is one unit of demand (as ecc0354996).
- On take: when a worker takes a record from a drain source and records remain in that source, and no worker is already reserved for it, reserve (or start) exactly ONE more worker. That worker takes the next record and applies the same rule, so a blocked handler never strands the records behind it, notifies stay at most one per take, and at most one reserved-not-yet-running worker exists per source.
- The wake decision stays in reserve_workers_locked (runtime/dispatch/offload_executor.cpp) — one place, no second rule site. Prefer expressing "already reserved for it" with the existing worker phase (e.g. any worker in `reserved` phase) rather than a new per-source flag; if a per-source marker is unavoidable, remove an existing input so the count does not grow, and say so. Update the comment "A take never wakes or starts another worker" to the new rule. Remove now-unused pending() plumbing only if it has no other caller.
- Report decision-point and input counts before/after.

## Completion conditions
1. test_cpp_framework_scheduler_drain (including burst :168-203 and independent :279-316) and test_cpp_framework_mailbox_owner_dispatch, plus all their negative-control registrations, pass 50x each with NO test changes (use run-focused.py watchdog; a hang is a failure, stop and report its stack). Then the full framework unit test list once.
2. Release bench build from this worktree (as psmeas configure-branch.sh), verify binaries are new, then base vs chained 3 rounds exactly like psmeas/measure.sh (send/request 1 KiB, PM Spot 30 s with readiness wait, 22k). Base build: /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build. Then Spot stage timing once on the chained build (psched5 pm-queue method).

## Merge conditions (PM, fixed; against base's own 3-round range)
target I/O sendto/msg ≤ base; Spot p99 within base range; send and request throughput within base range or better; app-worker µs/msg ≤ base; target voluntary wakes/msg ≤ base. Numbers and raw paths for each.

## Rules
Builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (Release, no -g, 25 GB cap). Every measurement under `flock -o /root/jobs/fx/cx/measure.lock`, source 0-7 / target 8-15. Commit locally on perf/1639-cpp-worker-wake only; no push/PR/GitHub/spec edits; no sub-agents or other AI tools; no `&` background jobs; SIGTERM only; never delete raw outputs; do not touch perf/1639-cpp-sched-drain or its worktree. Write $JOB/report.md (Korean). Remove your build dirs at the end; keep branch and worktree. Keep output short.
