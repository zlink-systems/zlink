# Task: Java Framework request path — remove the double envelope-header scan and source header encode cost (issues #3870, #3871)

Worktree /root/worktree/zlink-3870, branch perf/3870-java-header (from origin/main). Commit there; do not push. Kotlin uses this Java runtime.
Read first: /root/jobs/fx/cx/v21/s033/jprof/report.md (profile on main ee11390257) and `gh issue view 3870 --repo zlink-systems/zlink`, `gh issue view 3871 ...`.

## Situation (ratio040)
send-saturation 1024 0.366, request-backpressure 1024 0.460, request-serial 1024 0.582 and 4096 0.579. 4K send/backpressure pass.
Profile findings to act on:
1. request-backpressure: `ZLinkChannelEnvelope.decodeDispatchHeader` (~line 558) calls into `decodeHeader` (~line 493) so the same envelope header is scanned twice on the busy Java request path — 3.54 µs/message inclusive. Make it one scan that yields both the dispatch view and the full header, keeping strict validation and the existing cache behaviour.
2. request-backpressure source: `ZLinkChannelEnvelope.jsonString` (~line 270) costs 0.96 µs/message self in header encoding. Remove avoidable work (repeated escaping/allocation of constant fields, StringBuilder growth, re-encoding constant parts). Correlation and deadline fields are dynamic; the wire bytes must stay identical.
3. request-serial: the target Java pump spends 5.25 µs/message inclusive in `decodeHeader`/`decodeDispatchHeader`; fix 1 should also cover it. Completion hand-off `ZLinkServiceCompletionDispatcher.post` (~53) / `ZLinkJavaRawServicePort.deliverTerminal` (~491) shows 2.1 µs/message; look, but change it only with clear evidence and within spec 01-execution/01-submit-and-completion §11 (new completion turn) and 08-messaging-hot-path §4.4.
Send-saturation is limited by the source Core IO thread, not Java code — a separate cross-language job investigates that; do not work on it here.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No new flags, counters, modes or fallback paths; wire bytes identical. Report decision locations before/after.
- Build and test with the repo's normal Java commands (gradle; see framework/languages/java README and CONTRIBUTING), under `flock -s /root/jobs/fx/cx/measure.lock`. Run the Java unit tests and the integrationTest task that covers the envelope/channel code (the project requires Java integrationTest before PR), and add tests: golden header bytes encode, one-scan decode equals the previous two-scan results, malformed headers rejected exactly as before.
- Format: `bash scripts/format/format.sh --check java` from the worktree root, and fix.
- Measure: build the Java bench in this worktree (framework/bench/grpc/java). Pin like the ratio040 worktree's java run_local.sh (copy its pinning diff while measuring, revert before committing). Alternate this build and /root/worktree/zlink-ratio040 (prebuilt main) for request-backpressure 1024 and request-serial 1024 and 4096, Framework only, 4 rounds, WARMUP_SECONDS=30 DURATION_SECONDS=5, each run in `flock -o /root/jobs/fx/cx/measure.lock`, plus 2 rounds of the binding for same-day ratios. Report each round, medians and ratios.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/jfix/report.md (exact path)
Changes and why, measured effect, decision count, tests, format, A/B table, commit SHA, anything not finished.
