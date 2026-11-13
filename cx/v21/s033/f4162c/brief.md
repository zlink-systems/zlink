Model gpt-6-luna, effort high. Earlier briefs are history; read /root/jobs/fx/cx/v21/s033/f4162b/report.md and brief.md for context.

# Task: #4162 — PUBSUB catch-up must mean the backlog is drained

Worktree /root/worktree/zlink-v21-s033/f4162b, branch perf/4162-bench-gate-rules, HEAD 7a65e1f013. Do not push, fetch or pull, no PR, do not run scripts/dev/internal-docs.sh. Histogram unchanged.

## Supervisor finding
The decision (실장님): PUBSUB latency is measured only after the receiver has caught up with the sender; runs that never catch up are invalid. f4162b defined caught-up as "received rate over 0.5 s ≥ 95% of target and backlog not increasing" (perf_multi_measurement.hpp:13-50, PERF_POLICY.md:84-93). That is "keeping pace", not "caught up": run 1 shows paced_received_rate 9194.4 = target 9194.24 and caught_up at 1.01 s, yet latency p50 352 ms (raw/pubsub-01.log:54-55). The backlog left from the throughput phase stays constant, so the latency window measures the old backlog. 3 of 4 "valid" runs are 333–454 ms, one is 2.5 ms.

## Fix
- caught-up = the backlog is drained: in-flight count (sent sequence − received count, already tracked for the diag) ≤ a small bound, held for one sub-window. Pick the bound from the target rate (e.g. ≤ target_rate × 1 ms, minimum 1) and write the exact rule into PERF_POLICY.md (replace the 95%/non-increasing text). Keep the single owner catch_up_tracker_t; no second rule.
- Latency samples start at the catch-up point as now; deadline miss → invalid as now.
- Tests: unit cases for drained / keeping pace with a constant backlog (must NOT count as caught up) / never.
- Re-run PUBSUB tcp 256B × 5 under `flock -o /root/jobs/fx/cx/measure.lock` with taskset -c 8-15 as before; table per run: catch-up seconds, valid/invalid, latency mean/p50/p95, throughput, max in-flight at catch-up. Keep all raw files under /root/jobs/fx/cx/v21/s033/f4162c/raw (never delete raw outputs).
- Build via /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock; keep waiting if the gate is closed), Core prefix /root/jobs/fx/cx/v21/s033/core124/prefix-1.24. Aggregator tests, metrics self-test, git diff --check. Commit locally on top of 7a65e1f013.

## Rules
No GitHub, no sub-agents or other AI tools. Every claim file:line. Report in Korean, short, with decision sites before/after and a "리팩토링 점검" line.

## Output: /root/jobs/fx/cx/v21/s033/f4162c/report.md
