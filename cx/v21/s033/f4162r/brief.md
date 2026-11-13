Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4162 — record reference latency for cells that stay invalid (PM decision 2026-11-11, "안 1")

Worktree /root/worktree/zlink-v21-s033/f4162r (branch perf/4162-latency-reference, based on perf/4162-bench-latency-stats). Commit locally; do not push.
Read first: /root/jobs/fx/cx/v21/s033/f4162f/report.md (diagnosis: first-seq difference 0, real receive lag 1.0–1.8 ms), raw/final-pubsub-01.log:115-121 there (current output: latency = invalid:receiver_not_caught_up_by_deadline), doc/perf/PERF_POLICY.md:84-93, bindings/c/perf/multi/common/perf_multi_measurement.hpp, bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp.

## PM decision (do not change it)
- Catch-up rule, 1 ms bound, 0.5 s hold and deadline stay exactly as they are. Judged latency (latency, latency_p95, latency_p99) stays `invalid:<reason>` when catch-up is not reached.
- In that case ALSO emit the observed latency as non-judged reference values: p50, p99 of the latency samples observed during the run, and the in-flight floor (minimum over progress ticks of the minimum-subscriber in-flight, and that floor ÷ target_rate in ms). Name them so no consumer can mistake them for judged values (e.g. `latency_ref_p50`, `latency_ref_p99`, `inflight_floor`, `inflight_floor_ms`), and print them only when judged latency is invalid.
- A run whose late_sends_gt_1ms > 0 must be marked in the result (e.g. a RESULT line `sender_late,<count>`), so a report can single it out.

## Do
1. Find where the invalid latency RESULT lines are produced and where samples are collected; add the reference lines in one place (one decision site for "invalid → print reference"). No new flags/options; if you add a counter or field, remove or reuse an existing one where possible and list before/after counts of decision sites.
2. Update every consumer that parses these RESULT lines (grep the repo: perf aggregators, report scripts, tests) so unknown/reference keys are not judged; the reference values must never enter a pass/fail verdict.
3. Document the reference fields in doc/perf/PERF_POLICY.md next to lines 84-93 (one short paragraph: "판정 외 참고값").
4. Test: a unit/metrics test showing (a) caught-up run prints judged latency and no reference lines; (b) not-caught-up run prints invalid judged latency plus reference p50/p99/in-flight floor; (c) late sends > 0 is marked. Show it fails before your product change and passes after (paste both outputs).
5. One short real run: PUBSUB tcp 256 B, same command as f4162f final runs (see its run-pubsub-five.sh), 1 run, inside `flock -x /root/jobs/fx/cx/measure.lock` with cpu0-15 governor performance and powersave restored by trap. Paste the RESULT block.

## Rules
- Compile C/C++ only through /root/jobs/fx/cx/cpp-build-slot.sh. Use the Core prefix matching bindings/cpp/VERSION (/root/.cache/zlink/core-release/<ver>/linux-x64), binding from source (USE_BINDINGS_SOURCE=ON) where applicable.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools.
- Never delete raw outputs. Raw to /root/jobs/fx/cx/v21/s033/f4162r/raw.
- Korean, short.

## Output
/root/jobs/fx/cx/v21/s033/f4162r/report.md: changed files with reason, decision-site counts before/after, fail-before/pass-after outputs, the real-run RESULT block with raw path.
