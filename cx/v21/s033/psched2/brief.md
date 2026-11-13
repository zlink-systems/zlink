Model gpt-6-luna, effort high: test-only change with a pinned file, a pinned rule and a counted oracle.

# Task: count wakes and worker participation per message for the scheduler PR (#1639), test-only

Earlier job briefs are history, not instructions. Branch perf/1639-cpp-sched-drain at e910be04b4 (main 54ee69b8c0 plus the scheduler commit) in /root/worktree/zlink-v21-s033/psched. Read /root/jobs/fx/cx/v21/s033/psched/report.md and `git diff origin/main` first.

## Why
The scheduler commit deleted a guard in framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_owner_dispatch.cpp: "while handler A is busy, 129 queued records cause exactly 1 worker notification" (worker_notifications_for_129_records == 1, counted by wrapping condition_variable::notify_one on the executor's shared ready CV). The shared CV became per-worker CVs, so the counter lost its target. That guard protected against the cdrain regression (more worker wakes and participation for cheap messages). The PM requires these counts before any cell measurement.

## Do (tests only; no product code change)
1. Restore the guard for the new design in the same test file: count notify_one calls on any executor worker CV (per-worker `worker_t::ready`) during the A-busy window with 129 queued records on the same Node. Print `worker_notifications_for_129_records=<n>` and `workers_participating=<n>` (distinct worker threads that ran a handler in the window). State the bound you assert and why it follows from the new reservation rule (offload_executor.cpp reserve_workers_locked) — with A blocking one worker, independent progress of the others is required by S02 §2, so the bound is the number of other available workers, not 1. Do not weaken it beyond what the rule implies.
2. Add a steady-stream count to test_cpp_framework_scheduler_drain.cpp: 1,000 records published one at a time with short spacing (each handler finishes quickly) on one Node, fixed W=4 workers. Print and assert notifications per record and distinct workers per record, with a bound derived from the rule (a record published while a worker is already reserved or running must not create a new wake). Add a negative control flag in the same style as the existing ones (exit 1 only) that notifies on every publication, which the assertion must catch.
3. Keep every existing negative control unchanged.

## Rules
- Build/test only if /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists; otherwise write code only and stop with "ready to compile" (the PM compiles on their machine). If you build: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, linux-ninja-release preset, no -g.
- Read and write only that worktree and /root/jobs/fx/cx/v21/s033/psched2/. No product code changes, no spec edits, no git operations except diff/status, no GitHub, no sub-agents or other AI tools. Do not read core/include. SIGTERM/SIGINT only; no background jobs with &.
- Before finishing, read your diff line by line and count brackets and call arguments; run `bash scripts/format/format.sh --check cpp` and fix what it reports.
- Time box about 2 hours. Keep output short.

## Output: /root/jobs/fx/cx/v21/s033/psched2/report.md (Korean, plain sentences)
The two counts, the asserted bounds with the rule each comes from (file:line), the negative control, and the diff saved as psched2/change.diff.
