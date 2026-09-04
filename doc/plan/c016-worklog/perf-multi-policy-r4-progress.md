START detached a9eb6c5a778c; scope bindings/c/perf/**; policy/contract inspection started
INSPECT policy §1.2/§5.1 and Core Contract B confirmed; worktree only has expected core/build symlinks
MEASURE before build started; bindings-only CMake, ZLINK_BUILD_JOBS=3/JOBS=3, Core build untouched
MEASURE before complete: 15/15 cells, log perf-multi-policy-r4-before.log; Core 0.17.0
EDIT single active measurement restored for echo/DD/PUBSUB/REQREP; Contract B retained payload retry and relay drain updated
BUILD all affected multi targets and perf_multi_metrics_test passed; warnings pre-existing size_t diagnostics only
TEST perf_multi_metrics_test ctest passed 1/1 (binary assertion suite)
TEST requested CI multi smoke passed 6/6 throughput cells; 1024/65536 all nonzero, no hang
TEST REQREP smoke started for DEALER_ROUTER_REQREP and ROUTER_ROUTER_REQREP
TEST REQREP first smoke: 1024B passed; 65536B bounded drain timeout with 7773/10254 completions; raised drain to existing 5000ms policy knob, no workload cap
TEST REQREP follow-up passed: DEALER_ROUTER_REQREP and ROUTER_ROUTER_REQREP at 65536B, both nonzero
MEASURE after started: same CCU=100 DUR=5 tcp 64/256/1024/4096/65536 and 3 patterns; no parallel heavy work
MEASURE after first pass 14/15; DEALER_ROUTER_SENDSEND 64B exited 1, targeted debug rerun started
DEBUG DEALER_ROUTER_SENDSEND 64B confirmed teardown backlog exceeded fixed 5s; bounded echo drain now scales to 3x active duration without limiting submissions
FIX echo submit scheduling restored to one DONTWAIT attempt per writable client per round; no in-flight cap, inbound polling remains live, 64B retry passed with 5/5 metrics
MEASURE after final complete: 15/15 cells under identical CCU=100 DUR=5 tcp matrix; log perf-multi-policy-r4-after-final.log
TEST final requested CI multi smoke passed 6/6 cells and 30/30 result metrics with Core 0.17.0; no hang
TEST final REQREP smoke passed 4/4 cells and 20/20 result metrics at 1024/65536B
VERIFY full bindings/c build passed with jobs=3; perf_multi_metrics_test 5/5 plus ctest 1/1; shell/python syntax and git diff checks passed
SUMMARY perf-multi-policy-r4-summary.md written; tracked changes confined to bindings/c/perf/**; Core symlinks untouched
EXIT:0
