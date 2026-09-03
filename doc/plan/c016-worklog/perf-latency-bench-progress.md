# C perf one-way latency correction progress

- 2026-09-03: Confirmed `main`, preserved unrelated untracked `doc/principal/dev/hotpath.ko.md`, and found no pre-existing `bindings/c/perf/**` changes in either worktree.
- 2026-09-03: Chose a separate one-second latency phase with an in-flight cap of one. The existing saturated duration remains the throughput source; only the paced phase supplies mean/p95/p99.
- 2026-09-03: Scoped implementation to one-way single benchmarks only. REQREP, report fields, regression-gate keys, Core, docs, frameworks, and other bindings remain unchanged.
- 2026-09-03: Implemented the shared two-phase runner and routed DEALER_ROUTER/ROUTER_ROUTER through it. Main and baseline copies of all three changed source files are byte-identical.
- 2026-09-03: Built PAIR, PUBSUB, DEALER_DEALER, DEALER_ROUTER, and ROUTER_ROUTER in both worktrees. Ran `test_perf_regression_gate.py` (6/6 passed).
- 2026-09-03: After an empty `ps -eo comm | grep -E '^(perf_|python3)$'` check before every runner invocation, measured 64B PAIR inproc/tcp and DEALER_DEALER inproc in both worktrees with `--duration 1 --runs 1`. All mean/p95/p99 values were sub-ms; mean differences were 0.0%, +5.3%, and 0.0% respectively.
- 2026-09-03: Unrelated concurrent Core edits/build activity appeared after the first validation. Preserved those files, waited for LTO/compile processes to finish, confirmed CPU idle, and repeated only the main short cells. Final mean differences against the retained baseline reports are 0.0% for all three cells; all reported latency percentiles remain sub-ms.
