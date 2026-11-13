# Task: write the service wire into the final native message on the C++ request/send path (issue #3867, candidate W)

Evidence: /root/jobs/fx/cx/v21/s033/cprof/report.md ("Re-ranked R / W / T / M / H", "Gap to 0.6 and first change"), and the W row in /root/jobs/fx/cx/ratio034/gap14-analysis.md. W's enclosing scope is 2.395 µs/message on the critical submitter thread (request-backpressure 1024). The source makes one shared multipart holder on moved input and two byte-vector holder sites for the two outgoing service-wire parts, then materializes native messages (`C/backend/raw_binding_adapter.hpp:materialize_binding_part`). The reply side has the same holder sites plus a const-list ownership copy.
C = framework/languages/cpp/framework/src/runtime/.

## Goal
Encode the service wire directly into the final native message buffer(s) and consume moved multipart input without a temporary shared wrapper, on the request, reply and send paths. Wire bytes, admission, completion, routing and lifetimes stay exactly as they are. Spec references: framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md §3 E1 (size first, one buffer) and 05-payload-ownership-and-codec.ko.md §2.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Worktree /root/worktree/zlink-3867w, branch perf/3867-wire-writer (from origin/main 3fb411b4ab). Commit there; do not push.
- No new flags, counters, modes or fallback paths. If the change keeps an old path alive beside the new one, explain why in report.md. Report rule/decision count before and after.
- Build C++ only via `/root/jobs/fx/cx/cpp-build-slot.sh <build-dir> --parallel 3` (read its usage), under `flock -s /root/jobs/fx/cx/measure.lock`. Every affected target must compile with -k 0 before you call the work done.
- Tests: add or extend a unit test that pins the exact wire bytes of an encoded request, reply and send before and after (byte equality), and a test that a moved multipart input is consumed without copying (observable through the existing test-access seams only). Run the C++ unit and contract labels via `/root/jobs/fx/cx/measure-shared.sh` (read its usage). Run `bash scripts/format/format.sh --check cpp` from the worktree root and fix.
- Measure: build the C++ bench in your worktree (framework/bench/grpc/cpp/build.sh, under the build slot), apply the pinning patch /root/jobs/fx/cx/ratio034/pin-condition-b.patch to the cpp run_local.sh only for measuring and revert it before committing. Alternate your build and /root/worktree/zlink-ratio040 (already built, main ee11390257) for request-backpressure 1024 and send-saturation 1024, Framework only, 4 rounds each, WARMUP_SECONDS=30 DURATION_SECONDS=5, each run in `flock -o /root/jobs/fx/cx/measure.lock`. Report each round and the medians, plus submitter CPU µs/message and heap calls/message (counter: /root/jobs/fx/cx/v21/s033/cprof/alloc_counter.so).
- Keep MemAvailable >= 12 GiB.

## Output: /root/jobs/fx/cx/v21/s033/wire/report.md (exact path)
Changed files and why, allocations removed per message per path (static and measured), the A/B table, test results, format result, commit SHA, and anything you could not finish.
