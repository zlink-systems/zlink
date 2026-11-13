# Job (read-only measurement): .NET Framework request path — per-request counts and their sources, 0.33.0 #1639

Model/level: gpt-6-luna medium — light tier: measurement and collection, no product change.
Worktree /root/worktree/zlink-1639d (detached at origin/main fb254a964c), read-only. No sub-agents or other models (no Astra, no Fable).
Do not run scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers. No GitHub.
No commit, push, branch, stash, reset, rebase, spec edits. Do not edit the worktree. Write only under
/root/jobs/fx/cx/v21/s033/dcount/. Report: /root/jobs/fx/cx/v21/s033/dcount/report.md.

## Background
0.33.0 target: Framework ÷ binding >= 0.6 for request-backpressure, request-serial and send-saturation (1024 and 4096 B).
.NET request-backpressure 1024 is about 0.50. Earlier tuning of single spots gave a few percent each
(see /root/jobs/fx/cx/v21/s033/dfix/report.md). The C++ study found the cost is spread over many small per-request operations
(about 96 lock acquisitions and 36 heap allocations per request; /root/jobs/fx/cx/v21/s033/chand/report.md §2).
We need the same picture for .NET before choosing a design.

## Deliver
1. Fresh baseline: all six cells (3 patterns × 1024/4096), Framework and binding, condition B (source CPUs 0-7,
   target 8-15), 30 s warmup / 5 s active, 3 rounds, medians and ratios. Reuse the runner used in /root/jobs/fx/cx/v21/s033/dfix/.
2. For request-backpressure 1024, per completed request on source and target: per-thread CPU µs, allocations
   (bytes and count, from the runtime's own allocation counters/event tracing), lock/monitor acquisitions or
   contention events, thread hops (work items posted/continuations scheduled), syscalls. Same for binding.
3. Top call sites (function, file:line) per thread role that cover >= 80% of the Framework-only difference,
   with one line each on which Framework decision they serve. Mark Framework-only vs shared binding sites.
Facts only; no design proposals beyond that one line per site.

## Limits
Measurements under `flock -o /root/jobs/fx/cx/measure.lock`; builds under `flock -s /root/jobs/fx/cx/measure.lock`.
MemAvailable >= 12 GiB. SIGTERM/SIGINT only; never use &. Keep logs in files and read only summaries.
End the report with "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".
