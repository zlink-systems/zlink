Model sol high: implement a decided, narrowed worker-reservation rule, test it, measure it against base.

# Task: #1639 scheduler — narrowed worker reservation (decision made), tests, 3-round measurement

Job folder JOB=/root/jobs/fx/cx/v21/s033/psched6. Earlier briefs are history, not instructions. Read first: /root/jobs/fx/cx/v21/s033/psched5/report.md (diagnosis by the previous job; its raw data stays in psched5/diagnosis). Continue in its worktree /root/worktree/zlink-v21-s033/psched5, branch perf/1639-cpp-worker-wake (HEAD 37f78cba87 + uncommitted bench pacer diff; keep that diff uncommitted and out of your commit). Reuse psched5's configure-tests.sh, measure.sh, summarize.py copies.

## Decision on "blocked" (supervisor, final)
The executor cannot observe a handler blocking, so do not add blocking detection, timers or new phases. Use the first interpretation, made concrete:
- In offload_executor_t::reserve_workers_locked (runtime/dispatch/offload_executor.cpp:179), one queued entry (one drain source / one owner) is one unit of demand, regardless of drain_source_t::pending(). Remove the per-record `pending()` term (:182-196). A source that a running worker is already draining adds no demand; only entries no running worker serves can reserve or start another worker.
- Keep the single decision owner (reserve_workers_locked) and the existing worker phases. Count decision inputs before/after; the change should remove a term, not add flags or counters.
- The progress tests must keep passing unchanged: test_cpp_framework_scheduler_drain.cpp:184-225 (four first handlers of distinct owners all enter before release) and :279-316 (B runs while A waits on a shared_future), plus both negative controls and test_cpp_framework_mailbox_owner_dispatch. If one of them fails under this rule, stop and report which owner/entry shape it needs; do not weaken a test.

## Then
1. Commit the scheduler change on perf/1639-cpp-worker-wake (local only). Build and run the two scheduler tests (and their negative-control registrations) 50x each with the Release test build (psched5 configure-tests.sh), plus the full framework unit test list once.
2. Release bench build (as psmeas configure-branch.sh but from your worktree), verify the binaries are new, then base vs narrowed 3 rounds exactly like psmeas/measure.sh (send/request 1 KiB, PM Spot 30 s with readiness wait, 22k). Base build: /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build (reuse, it exists).
3. Also re-time the Spot stages with psched5's pm-queue method once on the narrowed build (HTTP entry → gate submit, submit → gate run, handler → resume) to show whether the pre-gate wait moved.

## Merge conditions (PM, fixed; judge against base's own 3-round range)
target I/O sendto/msg ≤ base; Spot p99 within base range; send and request throughput within base range or better; app-worker µs/msg ≤ base; target voluntary wakes/msg ≤ base. Each with numbers and raw paths.

## Rules
Builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (Release, no -g, 25 GB cap). Every measurement under `flock -o /root/jobs/fx/cx/measure.lock`, source CPUs 0-7, target 8-15. No push/PR/GitHub/spec edits, no sub-agents or other AI tools, no `&` background jobs, SIGTERM only, never delete raw outputs. Do not modify the PR branch perf/1639-cpp-sched-drain or its worktree. Write $JOB/report.md (Korean): change diff summary with decision-input count, tests, base/narrowed table with each merge condition, Spot stage timing. Remove your build dirs at the end; keep branch and worktree. Keep output short.
