# Task: cut .NET Framework request-path cost — deadline expiry and reply decode (issue #3869)

Worktree /root/worktree/zlink-3869, branch perf/3869-dotnet-request (from origin/main 910d1fc41f). Commit there; do not push.
Read first: the profile /root/jobs/fx/cx/v21/s033/dprof/report.md (and its raw/ and profile-summary.json), and the issue (`gh issue view 3869 --repo zlink-systems/zlink`).
D = framework/languages/dotnet/src/Zlink.Framework/.

## Baseline (ratio040, main ee11390257)
request-backpressure 1024: Framework 29,571 vs binding 58,990 (0.501); 4096: 0.484. request-serial 1024: 0.515; 4096: 0.491. Target: >= 0.6 in all four.

## Findings to act on
1. Deadline expiry per message, backpressure. On the hottest managed Framework worker, `ZLinkManagedMeshNode.ExpireOperationsUnderOwner` (D/.../ZLinkManagedMeshNode.cs ~12616) costs 4.4 µs/message self (6.9 inclusive) and `ZLinkDeadlineIndex<T>.Visit` (ZLinkRequestDeadlineIndex.cs ~66) 1.6 µs/message, plus DeadlineBudgetSpent/DeadlineNow. Spec 01-execution/01-submit-and-completion §10.1 says management visits happen at the time the index chooses (not per request), visit only slots whose candidates fall in the absolute window, completed entries are cleaned on the next visit, and management turns must not scale with request count. A cost proportional to messages suggests visits too frequent or scanning more entries than the window requires (for example walking all pending entries, or entries with remaining rounds). Find the actual cause from the source and a focused measurement (count visits per second, entries examined per visit, at request-backpressure 1024), then fix it within §10.1. Do not change timeout semantics: no timeout may be confirmed before its deadline, and reply vs timeout still races only on the terminal claim.
2. Reply decode, serial. `ZLinkEnvelopeReplyDecoder.Decode<TReply>` (D/Runtime/Messaging/ZLinkClientCallCodec.cs ~105) shows 12.5 µs/message self on the busiest source worker; the sampled self time may include inlined header decode and typed deserialization. Break this down (a microbenchmark of Decode on the benchmark reply shape, or a perf/dotnet-trace capture with inlining disabled for that method) and remove avoidable work (copies, intermediate lists/strings, repeated validation, codec lookups) while keeping envelope validation and typed decode behaviour. Compare with the C++ decoder for the same envelope (framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp) and Java.
3. Also look at `ZLinkBackendRouteReceived.Dispose` (2.9 µs) and `ZLinkMeshCompletionTable.ReleaseReservation` (2.1 µs) on the same serial worker; fix if the cause is clear.
4. No Framework thread is saturated in either pattern, so throughput is set by the chain, not one thread. After 1–3, re-measure; if still below 0.6, report the remaining per-message chain (which threads a request crosses and where it waits) rather than guessing.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No new flags, counters, modes or fallback paths; keep one implementation per rule. Report decision locations before/after.
- Cross-language: if a fix reflects a rule that Java/Node/C++ implement differently, say how they do it; do not change other languages in this job.
- Build and test .NET with the repo's normal commands (see framework/languages/dotnet README / scripts and CONTRIBUTING). Run builds and tests under `flock -s /root/jobs/fx/cx/measure.lock`. Run the .NET unit and contract tests that cover the changed files, then the full .NET unit test project; add tests for the deadline visit count (management visits must not scale with request count) and for decode equivalence (byte-identical inputs decode to equal results; malformed inputs rejected as before).
- Format: `bash scripts/format/format.sh --check dotnet` from the worktree root, and fix.
- Measure: build the .NET bench in this worktree (framework/bench/grpc/dotnet). Apply /root/jobs/fx/cx/ratio034/pin-condition-b.patch to the dotnet run_local.sh only while measuring (revert before committing; if the patch does not apply to dotnet, pin the same way the ratio040 worktree's dotnet run_local.sh does: target CPUs 8-15, source 0-7). Alternate this build and /root/worktree/zlink-ratio040 (prebuilt main) for request-backpressure and request-serial at 1024 and 4096, Framework only, 4 rounds, WARMUP_SECONDS=30 DURATION_SECONDS=5, each run in `flock -o /root/jobs/fx/cx/measure.lock`. Report each round, medians and the ratio against the ratio040 binding numbers.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/dfix/report.md (exact path)
Cause found for each finding (with the measurement that shows it), changes and why, decision count, tests, format, A/B table, commit SHA, remaining chain if below 0.6, anything not finished.
