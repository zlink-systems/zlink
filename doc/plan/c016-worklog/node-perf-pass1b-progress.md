2026-09-05T14:55:39+09:00 START: pass1b analysis; preserve existing changes; measurement gate pending
2026-09-05T14:57:05+09:00 ANALYSIS: single TS drain exists; raw OPT_FD watcher bypasses POLLCOMPLETION registration; checking Core readiness ownership and N-API callback boundary; benchmark gate absent
2026-09-05T14:59:32+09:00 ANALYSIS: Core hash matches pass1; baseline build underway JOBS=3; runtime/public already share drain; no measurement permitted yet
2026-09-05T15:01:35+09:00 PREPARED: sampled submit/admission/server-recv/reply/pull/Promise trace outside package; not executed while measurement gate pending
2026-09-05T15:02:54+09:00 UNIT: no-other-event-source consecutive REQUEST test passes on baseline; 50ms timer and mandatory unrelated wake not established; await measurement gate
2026-09-05T15:04:05+09:00 UNIT: runtime/public transfer regression prepared; no timer/uv_async root cause asserted; benchmark/profiling gate still absent
2026-09-05T15:09:34+09:00 IMPLEMENT: shared env Core POLLCOMPLETION wait and one libuv delivery FD; socket registries and TS drain unchanged; candidate build running, JOBS=3; no benchmarks
2026-09-05T15:11:41+09:00 DEBUG: first pump regression exposed invalid N-API callback receiver; fixed receiver and init cleanup; retrying only progress test, not benchmarks
2026-09-05T15:13:15+09:00 GATE: admission 5/5 and boundary/operation/layout 19/19 PASS; adding multi-Context, public wait-only settlement, close regressions; measurement gate pending
2026-09-05T15:16:39+09:00 FIX: full unit suite exposed cross-Context termination poisoning; pump lifetime moved to Context (one poller/worker/channel per Context), no API/d.ts changes; targeted regression rerun
2026-09-05T15:18:58+09:00 GATE: npm test raw unit suite 130/130 across27 files PASS; all87 d.ts byte-identical; 5x related tests running; samples/measurement deferred until sentinel/load gate
2026-09-05T15:20:49+09:00 PREPARED: native public-API admission/reply/pull and TCP recv timestamp observer compiled outside repo; not executed pending gate
2026-09-05T15:21:52+09:00 GATE: 28 related tests x5 PASS; no production instrumentation; external JS/native observers ready; waiting for verdict_chain.done and load<=3
2026-09-05T15:23:50+09:00 SUMMARY: draft with design comparison, gates, baseline ratios and explicit pending measurement blockers written; no performance run started
2026-09-05T15:25:20+09:00 MEASURE-GATE PASS: verdict_chain.done exists;  15:25:20 up 21:41,  1 user,  load average: 0.37, 2.97, 3.99; baseline DR/tcp/64B/100clients/1s diagnostic begins
2026-09-05T15:27:10+09:00 PROFILE: DR/64/100clients/5s baseline with97-stride sampling and public router recv timestamp;  15:27:10 up 21:43,  1 user,  load average: 0.14, 2.08, 3.54
2026-09-05T15:27:59+09:00 PROFILE: DR/64/100clients/5s candidate same observer;  15:27:59 up 21:44,  1 user,  load average: 0.45, 1.89, 3.40
2026-09-05T15:30:34+09:00 PROFILE: previous5s server was runner-terminated without exit buffers; observer now flushes on SIGTERM; baseline DR5s repeat for missing server boundary;  15:30:34 up 21:47,  1 user,  load average: 0.31, 1.32, 2.96
2026-09-05T15:31:26+09:00 PROFILE: candidate DR5s same preserved observer;  15:31:26 up 21:47,  1 user,  load average: 0.43, 1.20, 2.83
2026-09-05T15:33:16+09:00 OFFICIAL-AFTER START: exact requested20cells, no profiling;  15:33:16 up 21:49,  1 user,  load average: 0.27, 0.94, 2.55
2026-09-05T15:35:34+09:00 OFFICIAL: DD5cells complete; DR underway. Successful trace complete1310/1641 requests: serverTCP->publicRecv132.675/128.323ms, clientTCP->CorePull1.043/2.439ms; goal not yet met
2026-09-05T15:35:45+09:00 OFFICIAL correction: latest log already shows DR complete and RR through65536B; previous progress line understated completed cells
2026-09-05T15:36:08+09:00 OFFICIAL-AFTER EXIT:0
2026-09-05T15:38:36+09:00 REJECT: Context pump official20/20 completed but DR/RR64KiB throughput collapsed to807.6/584.8 ops/s; candidate source/binary/report preserved; reverted only own pump edits, retaining pass1; evaluating N-API callback scope fix
2026-09-05T15:42:51+09:00 CALLBACK FIX: shared pump rejected and removed; baseline ALS context regression fails(undefined), fixed native async resource/MakeCallback passes5progress tests; no new thread/timer/spin/pool/registry
2026-09-05T15:46:04+09:00 LOAD-BLOCK: final paired trace did not start; guard rejected load24.09 then host load150.84; external209 c++/189 lto processes observed; waiting <=3
2026-09-05T15:47:42+09:00 CORE-CHANGE: shared Core library now543e1089430176bf861f9ef8b7974941e3d785dee8d93bb8d4a39d62e1d08538,mtime15:15:18; start hash a2069049d5c35e74b56ba3bc95183302f9ff1950d590340ccacb1e834256800e. No Core writes by this task. Historical pass1 comparison is confounded; do not attribute64KiB gap solely to candidate. Load still152, no measurements running
2026-09-05T15:49:58+09:00 FINAL UNIT GATE: native callback scope candidate; npm test+samples (not benchmark/profiling), JOBS3;  15:49:58 up 22:06,  1 user,  load average: 156.47, 106.16, 50.81
2026-09-05T15:53:09+09:00 WAIT: final unit gate remains active; measurements blocked by load>3; final callback code adds only N-API async resource lifetime, preserving existing single drain and raw FD watcher
2026-09-05T15:54:28+09:00 GATE/WAIT: final progress5 tests PASS inside full suite; remaining legacy tests running under external LTO load; no new perf run
2026-09-05T15:55:48+09:00 FINAL-GATE FAIL: concurrent TCP routed SENDSEND test exceeded existing contract deadline under load126; stopped failed file to avoid continuing broad gate; no timeout/assertion changes; isolate when load recovers
2026-09-05T16:00:00.688680+09:00 WAIT external build load=74.2626953125; no benchmark/profiling active
2026-09-05T16:02:00.703304+09:00 WAIT external build load=17.9111328125; no benchmark/profiling active
2026-09-05T16:04:00.711088+09:00 MEASUREMENT GATE PASS load=2.97607421875
2026-09-05T16:04:00+09:00 FINAL PROFILE before DR64/100clients/5s
2026-09-05T16:04:12.920735+09:00 MEASUREMENT GATE PASS load=2.88671875
2026-09-05T16:04:12+09:00 FINAL PROFILE after DR64/100clients/5s
2026-09-05T16:04:19.998362+09:00 WAIT external build load=3.08349609375; no benchmark/profiling active
2026-09-05T16:04:49.999058+09:00 MEASUREMENT GATE PASS load=1.92041015625
2026-09-05T16:04:50+09:00 DIAGNOSTIC baseline on current Core, DR/RR64KiB/100clients/5s; isolates historical Core confound
2026-09-05T16:05:06+09:00 FINAL PAIRED DIAGNOSTICS DONE
2026-09-05T16:06:54+09:00 LOAD RECOVERED: isolate previously timed-out file before final full gate;  16:06:54 up 22:23,  1 user,  load average: 0.46, 28.83, 52.85
2026-09-05T16:07:02+09:00 TARGETED FAILURE RECHECK PASS; final npm test+samples begins
2026-09-05T16:07:25+09:00 FINAL FUNCTIONAL GATES PASS: npm test+samples and related5x
2026-09-05T16:07:40+09:00 DIAGNOSTICS: same Core baseline64KiB DR16435/RR15240.8ops/s reproduces historical rate; pump gap is not Core-only. Final callback scope pull->Promise0.588->0.012ms; total RTT129.827->130.204ms, primary queue delay persists
2026-09-05T16:08:36+09:00 FINAL OFFICIAL AFTER START: callback scope fix, exact requested20cells, no instrumentation;  16:08:36 up 22:25,  1 user,  load average: 1.26, 21.18, 47.66
2026-09-05T16:11:30+09:00 FINAL-AFTER RUNNING: no code changes or tests during measurement; final gates131tests+7samples and29x5 passed
2026-09-05T16:11:33+09:00 FINAL OFFICIAL AFTER EXIT:0; Core hash unchanged
2026-09-05T16:15:43+09:00 DIAGNOSTIC final DR64KiB isolated: investigate official7938.6 vs same-Core baseline16435 regression; official cell remains unchanged;  16:15:43 up 22:32,  1 user,  load average: 0.34, 6.57, 31.10
2026-09-05T16:17:49+09:00 FINAL-CHECK:20/20 final report copied; Core unchanged;87d.ts identical;diff-check clean; performance goal NOT met and regressions retained for reporting
2026-09-05T16:25:05+09:00 Final: native async callback fix retained; full gate 131 tests + 7 samples PASS; related 29x5 PASS; d.ts87 unchanged; diff-check PASS; official20/20 report copied. Performance goal/shared completion pump/empty wakes unresolved; regressions retained in summary.
EXIT:2
