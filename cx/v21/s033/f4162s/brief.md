Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4162 PR #4323 — address PM review (4 items)

Worktree /root/worktree/zlink-v21-s033/f4162r (branch perf/4162-latency-reference, HEAD 9b073028f2). Commit locally; do not push.
Read the review verbatim: /root/jobs/fx/cx/v21/s033/f4162s/review.md. Also /root/jobs/fx/cx/v21/s033/f4162r/report.md.

## Do (decisions are fixed; implement them)
1. **Reference values leave the RESULT channel.** The C engine (perf_multi_metrics.hpp / pubsub client+server, wherever latency_ref_p50, latency_ref_p99, inflight_floor, inflight_floor_ms, sender_late are printed) prints them as `REFERENCE,<same fields>` lines, not `RESULT,`. Remove EVERY exclusion branch for these keys from all parsers/gates (the 11 sites in the review: c/bench/with_zmq multi+single, c/perf run_comparison (2) + single, cpp/perf run_comparison + single, dotnet run_emit, python perf_metrics, perf_regression_gate, plus the python runner REQUIRED_METRICS branches). Exactly one place — the runner that re-shows child output — knows the `REFERENCE,` prefix and passes those lines through to the log/report as display only. `rg -n "latency_ref|inflight_floor|sender_late"` over bindings/ must show only: the C engine, that one display site, tests, docs. Report before/after site count.
2. **Gate: invalid latency cell does not make the whole report incomparable.** Decision: the invalid cell is excluded from judgement and listed explicitly (e.g. "invalid: PUBSUB tcp 256 latency — receiver_not_caught_up_by_deadline"); every other cell (incl. the same row's throughput) is judged normally. Implement in perf_regression_gate.py (do not route the invalid value into result.errors), add one line to doc/perf/PERF_POLICY.md "PUBSUB multi latency" stating this, and add a gate test: report with one invalid PUBSUB latency + valid throughput regression → throughput regression still fails the gate and the invalid cell is listed.
3. **Keep the engine's invalid reason.** bindings/c/perf/run_comparison.py (~:4291) must store the reason string with the cell and print it as received, not a fixed string. Test with a non-default reason.
4. **One invalid rule.** multi and single parsers accept `invalid:<reason>` under the same rule (pick: any metric, carried as an invalid cell; or latency metrics only — choose the one that needs fewer branches and apply it to both via one shared helper if a shared module already exists; do not add a new module just for this). Test that both parsers behave the same for the same line.

## Verification
- Behavioural fail-before for 2, 3, 4: run the new tests against the current HEAD product files (only revert product .py hunks, keep tests) → FAIL; after → pass. Paste outputs.
- Python tests via unittest from each test file's directory (pytest is not installed): bindings/c/perf/single/tests/test_multi_run_comparison_policy.py, bindings/c/perf/tests/test_perf_regression_gate.py, bindings/python/tests/test_perf_runner.py, and any with_zmq/cpp/dotnet parser tests that exist.
- C++: rebuild and run perf_multi_metrics_test (update its expected output for REFERENCE,). Then one short real run PUBSUB tcp 256 under `flock /root/jobs/fx/cx/measure.lock` (exclusive), source cores 0-7 / target 8-15, showing RESULT lines, REFERENCE lines, and the runner's report.
- `bash scripts/format/format.sh --check cpp` and the python format/lint the repo uses for these files.

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, -j3, Core prefix /root/.cache/zlink/core-release/1.25.0/linux-x64, build dir under the worktree named build-f4162s, 25 GB cap; remove at end.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Raw logs in /root/jobs/fx/cx/v21/s033/f4162s/logs; never delete.
- Net rule count must go down: no new flags/counters. Korean, short.

## Output
/root/jobs/fx/cx/v21/s033/f4162s/report.md: commits, decision-site table before/after per item, changed files with reason, fail-before/after outputs, real-run lines, remaining problems.
