# Task: give the Node Framework a safe margin above 0.6 — request-serial and send-saturation 4K (issue #3872)

Worktree /root/worktree/zlink-3872, branch perf/3872-node-request (from origin/main). Commit there; do not push.
Read first: the profile /root/jobs/fx/cx/v21/s033/nprof/report.md (with its raw data in the same directory) and `gh issue view 3872 --repo zlink-systems/zlink`.
N = framework/languages/node/ (find the exact src paths for the files named below).

## Situation
ratio040 (main ee11390257): send-saturation 4096 0.597 (fails), request-serial 4096 0.601, request-serial 1024 0.605, request-backpressure 1024 0.620. The profile's single runs: send-saturation 4096 0.645, request-serial 4096 0.596. These cells sit at the noise edge. Target: every Node cell >= 0.65 on the 4-round median.
- send-saturation 4096: the Framework target main event loop is saturated (17.45 µs/message vs binding 12.10); the source loop is close (16.98 vs 11.82). Hot target methods: `ZLinkEncodedPayload.ts:data` (1.375 µs self, returns a copy), `channel-envelope.ts:tryParseCanonicalChannelHeader` (0.86 inclusive), the dispatch chain `node-raw-binding-port.ts:receiveRecord` → `mesh-dispatch-pump.ts:dispatchClaimRecords` → `channel-dispatchers.ts:dispatchMesh/dispatchMeshCommand` → `channel-dispatch-pipeline.ts:dispatchOneWay/invoke` (≈6.7 µs inclusive), plus `receivedAtClock` read twice (service-mailbox.ts and channel-dispatchers.ts).
- request-serial 4096: latency-bound (round trip 193.5 µs vs binding 114.6). Source: `request-result.ts:createZLinkRequest` 29.8 µs inclusive, `raw-service-mesh-runtime.ts:requestToTarget` 13.7, `channel-envelope.ts:encodePayload` 2.1 self / 6.9 inclusive, `encodeChannelEnvelopeParts` 8.4 inclusive, `decodeChannelReply` 7.0, `deliverCompletion` 8.8, `ZLinkEncodedPayload.ts:data` 2.5 self, `isValidChannelCorrelationId` and `validateChannelHeader` 0.47/0.39.

## Goal
Remove avoidable per-message work on these paths while keeping every contract: the E3/E4 single pending-operation registration and E5 new completion turn (spec framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion §9–11, 08-messaging-hot-path §3–4), envelope validation, and decoded-value ownership (`data()` returns an independent copy to the application — keep that public contract; internal paths that only read may use a view if the bytes cannot escape). Typical candidates: redundant Promise/admission/result wrappers and continuation hops on `requestToNode`/`createZLinkRequest`; repeated validation of values validated once already on the same path; repeated clock reads per record; intermediate arrays/buffers in envelope encode/decode; per-record closures in the dispatch chain. Measure each change; keep only those that help.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No new flags, counters, modes or fallback paths. Report decision locations before/after.
- Build and test with the repo's normal Node commands (see framework/languages/node README / package.json and CONTRIBUTING), under `flock -s /root/jobs/fx/cx/measure.lock`. Run the Node unit and contract tests that cover changed files, then the full Node unit suite. Add tests that pin the contracts you touch (single pending entry, completion on a new turn, `data()` copy independence, validation rejections).
- Format: `bash scripts/format/format.sh --check node` from the worktree root, and fix.
- Measure: build the Node bench in this worktree (framework/bench/grpc/node). Pin like the ratio040 worktree's node run_local.sh (target CPUs 8-15, source 0-7; copy its pinning diff while measuring, revert before committing). Alternate this build and /root/worktree/zlink-ratio040 (prebuilt main) for send-saturation 4096, request-serial 4096, request-serial 1024 and request-backpressure 1024, Framework only, 4 rounds, WARMUP_SECONDS=30 DURATION_SECONDS=5, each run in `flock -o /root/jobs/fx/cx/measure.lock`. Also run the binding for the same four cells for 2 rounds so the ratio uses same-day binding numbers. Report each round, medians and ratios.
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/nfix/report.md (exact path)
Changes and why (with the measured effect of each), decision count, tests, format, A/B table, commit SHA, anything not finished.
