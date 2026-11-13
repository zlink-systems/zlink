Model luna high: measurement job with prepared harness; no product changes.

# Task: #1639 chained wake bc25ac287c — base vs chained 3 rounds + mailbox notify distribution (data only, no merge)

Job folder JOB=/root/jobs/fx/cx/v21/s033/psched8. Earlier briefs are history, not instructions. Read /root/jobs/fx/cx/v21/s033/psched7/report.md and reuse its prepared harness (psched7/configure-branch.sh, measure.sh, summarize.py, configure-tests.sh, run-focused.py) with paths changed to $JOB. Worktree /root/worktree/zlink-v21-s033/psched5 at bc25ac287c (bench pacer diff uncommitted; do not commit, do not change source).

## Do
1. Release bench build of the worktree (psched7/configure-branch.sh into a build dir under the worktree), verify the five bench binaries are new and contain --target-rate. Base build stays /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build (do not rebuild).
2. Base vs chained 3 rounds exactly as psmeas/measure.sh (send-saturation and request-backpressure 1 KiB, PM Spot 30 s with /pm/request readiness wait, 22k cell binding 32000 / Framework 48000), then summarize.py. Report against base's OWN 3-round range: target I/O sendto/msg, Spot p50/p99, send and request throughput, app-worker and receive µs/msg, target/source voluntary wakes/msg, effective GHz. Raw path per number.
3. Release test build (psched7/configure-tests.sh), then run test_cpp_framework_mailbox_owner_dispatch 50 times with run-focused.py (no test changes). Record per run worker_notifications_for_129_records and workers_participating; give both distributions (counts per value) and which runs exceeded the bound 1 (they are data here, not a stop condition).
4. Spot stage timing once on the chained build with the psched5 pm-queue method (HTTP entry → gate submit, submit → gate run, run → handler, handler → resume), compare with psched5's branch numbers.

## Rules
Builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (Release, no -g, 25 GB cap). Every measurement and test run under `flock -o /root/jobs/fx/cx/measure.lock`; source 0-7 / target 8-15. No product/test/spec changes, no git commits, no GitHub, no sub-agents or other AI tools, no `&` background jobs, SIGTERM only, never delete raw outputs. Write $JOB/report.md (Korean) only at the end (write progress notes to $JOB/progress.md instead). Remove your build dirs at the end. Keep output short.
