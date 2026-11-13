# Job: does TCP write batching follow submit speed? (0.33.0 #1639) — measurement only

Model/level: gpt-6-luna high. No sub-agents or other models (no Astra). Do not run
scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers.

## Background
/root/jobs/fx/cx/v21/s033/coreio/report.md measured C++ request-backpressure 1K messages per successful
TCP send: Framework 1.18 vs binding 10.53 (request direction), 1.22 vs 6.57 (reply). TCP send CPU per
message: Framework 15.8 vs binding 1.5 µs. It left open whether a faster Framework submitter would give
binding-sized batches by itself.

## Questions
1. Current state: on /root/worktree/zlink-3867r (HEAD 247b16c4be, PR #3919), measure messages per successful
   send and per receive, both directions, for C++ request-backpressure 1K and send-saturation 1K, Framework
   and binding. Use the coreio counter method and scripts (copy them into this directory).
2. Counterfactual: run the binding benchmark rate-limited to the Framework's throughput (~52K msg/s request,
   ~176K send) with the same window/concurrency. Report its messages per send and its I/O-thread CPU per
   message. If rate-limited binding also writes ~1 message per send, batching follows submit rate; if it
   still batches ~10, something in the Framework path flushes per message. If a rate limit needs a code
   change, make it only in a copy of the benchmark under this directory.
3. If the Framework path flushes per message, name the file:function that causes each separate send
   (Framework or binding call sequence), with evidence.

## Measurement rules
Condition B (source CPUs 0-7, target 8-15), 30 s warm-up, 3 runs, medians. Every benchmark and tracer under
`flock -o /root/jobs/fx/cx/measure.lock`, one at a time. Builds (only if needed) under
`flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 ...`.
MemAvailable >= 12 GiB. Stop processes with SIGTERM/SIGINT only. Tracer perturbation must be stated;
use counters, not strace, for batch numbers.

## Write limits and forbidden
The worktree is read-only for you. Write only under /root/jobs/fx/cx/v21/s033/cbatch/. Forbidden: commit,
push, branch, stash, reset, repository edits, spec edits, GitHub operations.

## Report
/root/jobs/fx/cx/v21/s033/cbatch/report.md:
- the batch table;
- the rate-limited binding result;
- the answer to question 3;
- raw paths.
End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
