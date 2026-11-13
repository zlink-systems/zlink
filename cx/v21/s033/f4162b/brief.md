Model gpt-6-luna, effort high. Earlier briefs are history; read /root/jobs/fx/cx/v21/s033/f4162/report.md for context.

# Task: #4162 follow-up — bench gate rules decided by 실장님 (2026-11-11 03:00Z)

Worktree /root/worktree/zlink-v21-s033/f4162b, branch perf/4162-bench-gate-rules, based on perf/4162-bench-latency-stats (histogram change 2be354603a + main merge). Do not push, fetch or pull, no PR, do not run scripts/dev/internal-docs.sh. Do not change the histogram itself.

## Decisions to implement
A) A/A reproducibility ±5% is judged on **p50 latency**, not the mean.
B) PUBSUB latency window starts only after the receiver has caught up with the target send rate; a run that never catches up by the end is recorded as **invalid** (not a number), with the reason.

## Do
1. Find every place that states or applies the A/A ±5% rule and the PUBSUB latency window: doc/perf/PERF_POLICY.md (judgment clauses), single/multi policy docs, harness/aggregator scripts (e.g. bindings/c/perf/**, scripts that print RESULT lines, any A/A comparator). List them with file:line before changing. If the ±5% rule lives only in issue text and in no file, say so and add it to PERF_POLICY.md in the judgment section.
2. B in the harness: use the existing pacing diagnostics (perf_multi_measurement.hpp:71-79 — target_sender_rate, paced_received_rate) as the single owner of "caught up". Define caught-up concretely (e.g. paced received rate over a fixed sub-window ≥ target rate × (1 − tolerance) and backlog not growing); write the exact definition and tolerance into PERF_POLICY.md. Latency samples before catch-up are not added to the histogram. If never caught up: RESULT latency lines say invalid with reason; throughput lines unchanged. No second rule elsewhere — decision sites before/after table.
3. Tests: unit test for the caught-up decision (never / late / immediate) and for invalid output; extend test_perf_multi_metrics.cpp. A comparator test for p50 ±5% if a comparator exists.
4. Run: build via /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock; keep waiting if the gate is closed), Core prefix /root/jobs/fx/cx/v21/s033/core124/prefix-1.24 as f4162 did. Then under `flock -o /root/jobs/fx/cx/measure.lock` with taskset -c 8-15: PUBSUB tcp 256B × 5 runs; record per run caught-up time, valid/invalid, latency mean/p50/p95, throughput. Single A/A 20 runs (as f4162 observe.py) judged by p50 ±5%: report pass/fail and the table. Keep all raw files (never delete raw outputs) under /root/jobs/fx/cx/v21/s033/f4162b/raw.
5. git diff --check, commit locally. Policy text in Korean (and English if the clause has an English pair).

## Rules
No GitHub, no sub-agents or other AI tools. Every claim file:line. Report in Korean, short, with "리팩토링 점검" line.

## Output: /root/jobs/fx/cx/v21/s033/f4162b/report.md
