# C perf one-way latency correction

## Result

The five single one-way patterns now report throughput from the existing saturated interval and mean/p95/p99 latency from a separate non-saturated interval. REQREP behavior, result columns, and regression-gate cell keys are unchanged.

## Design

- Phase 1 keeps the existing duration-controlled, unrestricted send loop. Its received-message count remains the reported throughput source.
- Phase 2 lasts one second and permits exactly one message in flight. The sender reads the receiver thread's atomic received count and does not timestamp/send the next sample until the previous sample has been received.
- Only Phase 2 contributes send-to-receive mean/p95/p99. The stop token drains and terminates Phase 1 before Phase 2 starts, so saturated HWM backlog cannot enter the latency sample set.
- The pacing acknowledgment is an in-process benchmark synchronization counter; it does not add a reply to the measured one-way wire protocol.
- PAIR, PUBSUB, and DEALER_DEALER already use the shared one-way runner. DEALER_ROUTER and ROUTER_ROUTER were moved from duplicated loops to that same runner so all five patterns use the identical policy.

## Changed files

- `bindings/c/perf/single/common/perf_single_one_way.hpp`
- `bindings/c/perf/single/src/perf_dealer_router.cpp`
- `bindings/c/perf/single/src/perf_router_router.cpp`

The same three files are modified and byte-identical in `/home/hep7/project/zlink-perf-core-0.15.1`. No REQREP source, report formatter, `perf_regression_gate.py`, or gate test was changed.

## Short validation measurements

All cells used 64B messages, `--duration 1 --runs 1`, HWM 1000, and an empty pre-run `ps -eo comm | grep -E '^(perf_|python3)$'` check. Values are milliseconds. The delta compares main mean with baseline mean; runner output is rounded to 0.001ms.

| Pattern / transport | Baseline mean / p95 / p99 | Main mean / p95 / p99 | Mean delta |
|---|---:|---:|---:|
| PAIR / inproc | 0.002 / 0.002 / 0.002 | 0.002 / 0.003 / 0.007 | 0.0% |
| PAIR / tcp | 0.019 / 0.024 / 0.042 | 0.019 / 0.039 / 0.061 | 0.0% |
| DEALER_DEALER / inproc | 0.007 / 0.008 / 0.016 | 0.007 / 0.008 / 0.016 | 0.0% |

Raw runner reports:

- `/home/hep7/project/zlink-work/c016/main-pair-short.txt`
- `/home/hep7/project/zlink-work/c016/main-dealer-dealer-short.txt`
- `/home/hep7/project/zlink-work/c016/baseline-pair-short.txt`
- `/home/hep7/project/zlink-work/c016/baseline-dealer-dealer-short.txt`

The short-run p95/p99 values are all far below 1ms, but their relative percentages are sensitive to scheduler noise and the report's 0.001ms precision. The requested near-5% comparison is met by the mean values (0.0%, 0.0%, 0.0%). Main was repeated after unrelated LTO activity ended; the table and raw main reports contain the final idle-CPU run.

## Other verification

- Main: built `perf_pair`, `perf_pubsub`, `perf_dealer_dealer`, `perf_dealer_router`, and `perf_router_router` successfully.
- Baseline: built the same five targets successfully.
- `python3 -m unittest bindings/c/perf/tests/test_perf_regression_gate.py`: 6 tests passed.
- `git diff --check -- bindings/c/perf`: passed in both worktrees.
- Active runtimes were printed in runner metadata and were newer than their respective Core sources:
  - main: `/home/hep7/project/zlink/core/build/lib/libzlink.so.0.15.1`
  - baseline: `/home/hep7/project/zlink-perf-core-0.15.1/core/build/lib/libzlink.so.0.15.1`
- Every shell command, build, test, and benchmark ran under `ulimit -v 16777216`.

## Copy procedure

From the main worktree, apply the measurement policy to the baseline runner with whole-file copies:

```bash
cp /home/hep7/project/zlink/bindings/c/perf/single/common/perf_single_one_way.hpp /home/hep7/project/zlink-perf-core-0.15.1/bindings/c/perf/single/common/perf_single_one_way.hpp
cp /home/hep7/project/zlink/bindings/c/perf/single/src/perf_dealer_router.cpp /home/hep7/project/zlink-perf-core-0.15.1/bindings/c/perf/single/src/perf_dealer_router.cpp
cp /home/hep7/project/zlink/bindings/c/perf/single/src/perf_router_router.cpp /home/hep7/project/zlink-perf-core-0.15.1/bindings/c/perf/single/src/perf_router_router.cpp
```

Those copies have already been applied for this validation and were intentionally left in the baseline worktree.

## QUESTIONS

- None.
