2026-09-05 KST START: detached main worktree confirmed; preserving untracked core/build and core/build-dev symlinks, reviewing Core test/build/spec constraints.
2026-09-05 08:24 KST: added public-C-API handover distribution repro (tcp/inproc x reconnect 10/100/1000 x 20) and configured isolated RelWithDebInfo build-d086; initial build running at JOBS=3.
2026-09-05 08:29 KST: baseline repro confirmed: inproc 3/3 at 0 ms; tcp timed out 3/3 at 6000 ms. Existing ROUTER/pipe logs show repeated TCP pipe termination/reconnect before duplicate route adoption, so investigating accepted transport-pair admission owner.
2026-09-05 08:33 KST: temporary trace confirmed accepted pair-ID reuse for same RID causes count-1 Application slot collision and rejects both pipes before ROUTER handover; removed trace and changed count-1 accepted connections to allocate fresh pair IDs (count-2 lane pairing unchanged).
2026-09-05 08:37 KST: fixed RelWithDebInfo distribution is tcp 4/5/6/6 ms (min/p50/p95/max) for all 10/100/1000 ms cells and inproc 0/0/0/0; related tests green and new test 5/5 green. Full rebuild/gate starting.
2026-09-05 08:41 KST: full isolated build completed; full ctest -j2 running (143 tests, integration/serial lane included), first 26 tests green.
2026-09-05 08:47 KST: RelWithDebInfo full ctest finished 142/143; only hotpath_gate failed because O2/no-LTO measured 1.25-1.32x Release/LTO reference. Reconfiguring same build-d086 as Release+LTO release-gate for authoritative full gate.
2026-09-05 08:42 KST: Release+LTO rebuild in progress at 64% with JOBS=3 (timestamp correction: the two preceding estimated timestamps are sequence markers and ran before this line).
2026-09-05 08:45 KST: Release+LTO rebuild reached 75%; no compile/link failures, memory-constrained JOBS=3 maintained.
2026-09-05 08:49 KST: Release+LTO rebuild reached 85%; continuing final integration target links at JOBS=3.
2026-09-05 09:00 KST: final Release+LTO gates green: full ctest 143/143 (198.39s), new test 5/5, single-lane 29/29 x2, hotpath ratios 0.9952/0.9962/1.0005/0.9988, diff-check clean; writing final summary.
EXIT:0
