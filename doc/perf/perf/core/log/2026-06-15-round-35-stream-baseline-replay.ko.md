# Round 35: STREAM baseline replay

- 목표: `MULTI_STREAM/tcp/64` 기준값 `400124.6`이 현재 머신/현재 부하 조건에서도 재현되는지 분리한다.
- 기준 baseline report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 기준 commit: `cb605c6c1`
- 현재 clean recheck: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_013449_round34_stream_tcp64_clean_recheck.txt`

## Why this round exists

- Current source has no retained core change from rounds 31-34.
- `MULTI_STREAM/tcp/64` current clean recheck is `335068.0`, while corrected baseline is `400124.6`.
- Baseline commit `cb605c6c1` used a different STREAM perf server path: it did not take `send_mutex` around echo sends.
- The active task does not allow perf harness changes as performance improvements, so this round only replays the baseline commit in a separate worktree and records whether the 400k number is reproducible now.

## Plan

1. Create a detached worktree for `cb605c6c1` outside the active checkout.
2. Build the baseline core/perf artifacts in that worktree.
3. Run only `MULTI_STREAM/tcp/64` with the same focused command shape.
4. Do not modify current workspace source or perf runner.

## Execution

Worktree:

```bash
git worktree add --detach /tmp/zlink-cb605c6c1 cb605c6c1
```

Build:

```bash
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j$(nproc)
```

Perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round35_baseline_cb605_stream_tcp64_replay
```

Report:

- `/tmp/zlink-cb605c6c1/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_014634_round35_baseline_cb605_stream_tcp64_replay.txt`

Result:

- `META,commit,cb605c6c1`
- runtime: `/tmp/zlink-cb605c6c1/core/build/lib/libzlink.so.6.0.0`
- load_avg: `22.87 26.84 18.27`
- `MULTI_STREAM/tcp/64`: `381021.6`
- failure: `0`

Comparison:

- Original baseline report: `400124.6`
- Baseline replay now: `381021.6`, `-4.77%` vs original baseline report.
- Current clean recheck: `335068.0`, `-12.06%` vs baseline replay.
- Current no-send-mutex diagnostic from round30: `383141.6`, close to baseline replay `381021.6`.

Interpretation:

- The original 400k is not fully reproduced under current load, but the baseline commit still lands around `381k`.
- The current perf harness with `send_mutex` lands around `335k`; removing that mutex diagnostically landed around `383k`.
- This supports the earlier conclusion that most of the remaining STREAM/tcp/64 gap is benchmark-side serialization, not an obvious core runtime regression.
- Because the active task forbids counting perf-only changes as core improvements, no perf source change is retained.
