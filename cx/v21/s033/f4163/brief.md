Model sol high: root cause of a Core latency shift and a fix proposal; concurrency/admission reasoning.

# Task: #4163 — why #4135 keeps the ROUTER_ROUTER inproc 1024B queue deep at saturation, and the fix

Job folder JOB=/root/jobs/fx/cx/v21/s033/f4163. Earlier briefs are history, not instructions. Work in your own detached worktree $JOB/src at 0005a4d6c0 (`git -C /root/zlink worktree add --detach`).

## Facts (measured, do not re-derive)
- single ROUTER_ROUTER inproc 1024B, C perf harness (bindings/c/perf/run_benchmarks.sh --pattern ROUTER_ROUTER --transports inproc --msg-sizes 1024 --runs 5), CPU 8-15.
- Mean latency: 1.23 (79567d3791) median 0.15-0.29 ms; 9dc2246dd0 (has #3930, not #4135) median 0.088 ms, 0/10 above the 1.23 max; 1.24 candidate 0005a4d6c0 (#4135) median 1.09-1.11 ms, 7-9/10 runs at 0.9-1.15 ms. Throughput equal (~1.8 Mmsg/s). The latency distribution is bimodal in every version; #4135 moves most runs to the high mode.
- Raw: /root/jobs/fx/cx/v21/s033/c4163/report.md and runs/, /root/jobs/fx/cx/v21/s033/core124b/confirm10/analysis.txt.
- #4135 (git show 0005a4d6c0): complete-record writers pass pipe_write_complete_record so the empty-queue HWM exception covers a record's MORE frames (spec 06-auto-hwm :476). The ROUTER send path (router_send_path.cpp, router.hpp) changed too.

## Do
1. Read the #4135 diff in full, the spec it cites (core spec 06-auto-hwm around :460-480), and the benchmark's ROUTER_ROUTER single sender/receiver loop (bindings/c/perf). Explain, with file:line, which admission or wake decision changed for this benchmark's messages (ROUTER routing-id frame + 1024B body): e.g. does the routing-id frame now take a path that admits more records per pipe, changes when the writer is told the pipe is full, or changes the reader-activation/flush cadence. Mean latency at equal throughput is queue depth ÷ rate, so name the queue that is deeper and which stage drains it later.
2. Confirm with a measurement, not only reading: e.g. count queued records or HWM refusals/write-activation edges per message at 1.23-mid (9dc2246dd0) vs candidate with a temporary counter or perf probe in your own builds (never in the shared prefixes). Build Core via the recipe in /root/jobs/fx/cx/v21/s033/core124/build-prefix-and-harness.sh into $JOB (prefix dirs of your own), builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, Release, no -g. Each benchmark run under `flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 ...`. Reuse /root/jobs/fx/cx/v21/s033/c4163/ prefixes and harness setup notes (provenance manifest workaround in its job log) rather than rediscovering them.
3. Propose the smallest fix in the Core module that owns the decision, keeping #4118's guarantee (a complete multipart record on an empty queue must not be refused forever for a small HWM; tests test_router_complete_record_hwm.cpp, unittest_router_pipe_contract.cpp must still pass). Implement it as a commit on a local branch `fix/4163-router-inproc-queue-depth` in your worktree (do not push), build, run those two tests plus the Core unit/contract tests the #4135 PR touched, then measure single ROUTER_ROUTER inproc 256,1024 runs-10 (two rounds of --runs 5, alternating with the candidate) and compare with the 1.23 / mid numbers above.

## Output
$JOB/report.md (Korean): mechanism with file:line, the measurement that confirms it (raw paths), the fix diff summary with a before/after count of decision points (no new flags or counters without removing one), test results, runs-10 table. Keep output short.

## Rules
No push, PR, GitHub or spec edits; no sub-agents or other AI tools; no `&` background jobs; SIGTERM/SIGINT only; never delete raw outputs; 25 GB disk cap; MemAvailable >= 12 GiB before builds and runs. Remove build directories at the end, keep the worktree and branch.
