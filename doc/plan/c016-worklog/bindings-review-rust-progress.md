2026-09-04T19:39:42+09:00 START review bindings/rust (commit 85eb9425a1, core 50d77800f2)
2026-09-04T19:42:00+09:00 baseline perf DEALER_ROUTER tcp 1024: thr=301034 msg/s lat=0.077ms p95=0.183 p99=0.479 (report perf_rust_single_linux_20260904_194006_review_before.txt)
2026-09-04T19:56:28+09:00 fixes applied: reactor thread replaces executor-turn spin, lazy SEND registration + parked WRITABLE replay, per-part Core shared copies, RwLock submit gate, REQUEST paths restored; new tests 5/5 green (routed_async 12/12)
2026-09-04T19:59:19+09:00 run_tests.sh 14/14 PASS; perf after DEALER_ROUTER tcp 1024: 130124 msg/s (baseline 301034 came from the executor spin loop); single smoke 6 cells complete; POLLOUT-mask probe made no difference (120807) -> reverted; multi smoke rerun started
2026-09-04T20:06:37+09:00 pre-port baseline (70a9998998 worktree) DEALER_ROUTER tcp 1024: 171888 msg/s lat 4.0ms; perf single sender now drives SEND futures via its own public Poller wait: 258606 msg/s lat 2.06ms (smoke2 sample 281791); single smoke2 6/6 complete; multi smoke complete exit 0; fmt/diff-check OK
EXIT:0
