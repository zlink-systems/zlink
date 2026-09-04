2026-09-04 START branch=main HEAD=70a9998998138cc3db8258711cbf986e4ced113c scope=bindings/go/**; unrelated worktree changes preserved; core/build read-only link target.
2026-09-04 REVIEW found runtime completion no-event spin and WRITABLE terminal result collapse; perf runners rejected direct ZLINK_GO_NATIVE_DIR, patched direct prebuilt core/build/lib support.
2026-09-04 FIX lazy-started runtime drain only after a nonzero SEND wait token, changed no-event loop to blocking completion poll, mapped TERMINAL ENOENT to SubmitNotFound and ESHUTDOWN/ETERM to SubmitTerminated.
2026-09-04 TEST targeted public/internal regressions count=5 green, including exact packet retry, immediate missing-route failure, and route-removal terminal wake.
2026-09-04 PERF DEALER_ROUTER tcp 1024B duration=3 runs=1: before 81,519 msg/s mean 0.620ms p95 2.727ms p99 7.859ms; after 190,141 msg/s mean 0.340ms p95 1.792ms p99 5.421ms.
2026-09-04 SMOKE tests/vet/guards green and samples 7/7; single perf PAIR,DEALER_ROUTER,PUBSUB tcp,inproc all 6/6 nonzero.
2026-09-04 MULTI first run exposed DEALER_ROUTER_SENDSEND tls/1024 post-window hang (300s); root cause sender waited forever on final WRITABLE after receive drain stopped, patched measurement-end socket lifecycle wake.
2026-09-04 MULTI targeted tls/1024 fixed run green in 3s at 13,308.5 ops/s; final 3-pattern x 2-size x 4-transport run green 24/24, 120/120 result rows, all throughput nonzero.
2026-09-04 FINAL targeted regressions count=5 green, race green, full tests/vet/guards green, samples 7/7, diff-check/gofmt/bash-n green; summary written; BLOCKERS none.
EXIT:0
