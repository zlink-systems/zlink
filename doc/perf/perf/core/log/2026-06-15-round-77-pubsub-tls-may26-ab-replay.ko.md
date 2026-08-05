# Round 77 - PUBSUB/tls May26 A/B replay

## 목적

- `MULTI_PUBSUB/tls/64B` 하락이 과거 report의 단일 run 편차인지, 현재 checkout 쪽 차이인지 분리한다.
- May26 기준 commit `1b60c0159`를 별도 worktree에서 같은 머신에 빌드해 재측정한다.

## May26 replay

```bash
git worktree add --detach /tmp/zlink-1b60-pubsub-tls 1b60c0159
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release -DZLINK_BUILD_TESTS=ON
cmake --build core/build -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern PUBSUB --transports tls --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round77_may26_commit_pubsub_tls_replay
```

- report: `/tmp/zlink-1b60-pubsub-tls/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_133611_round77_may26_commit_pubsub_tls_replay.txt`
- runtime: `/tmp/zlink-1b60-pubsub-tls/core/build/lib/libzlink.so.6.0.3`
- load_avg: `17.85 20.48 14.91`
- result: `MULTI_PUBSUB/tls/64B = 2,460,474.8 ops/s`

## Current replay

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round77_current_pubsub_tls_ab_recheck
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_133649_round77_current_pubsub_tls_ab_recheck.txt`
- runtime: `core/build/lib/libzlink.so.6.0.4`
- load_avg: `9.62 18.04 14.32`
- result: `MULTI_PUBSUB/tls/64B = 2,271,125.6 ops/s`

## 결과

- 같은 시간대 재측정 기준 현재는 May26 commit보다 약 `-7.70%`.
- 과거 full report의 `2,623,065.0 ops/s`와 smoke report의 `2,537,614.0 ops/s`보다는 May26 replay 자체가 낮다.
- 따라서 환경 편차와 현재 checkout 차이가 섞여 있지만, 현재 checkout 쪽 하락도 실제로 남아 있다.

## 추가 확인

- May26 perf source를 현재 `core/build`에 링크해 harness 차이를 분리하려 했지만 실패했다.

```bash
cmake -S bindings/c -B bindings/c/build_currentcore -DCMAKE_BUILD_TYPE=Release -DZLINK_CORE_DIR=/home/hep7/project/kairos/zlink/core -DZLINK_C_CORE_BUILD_DIR=/home/hep7/project/kairos/zlink/core/build -DZLINK_C_BUILD_BENCHMARKS=ON -DZLINK_C_BUILD_SAMPLES=OFF
cmake --build bindings/c/build_currentcore --target comp_src_pubsub_server comp_src_pubsub_client -j$(nproc)
```

- 실패 이유: May26 perf source가 현재 core C API의 `zlink_monitor_snapshot`, `zlink_spot_node_internal_sockets_snapshot` 심볼과 맞지 않는다.

## 다음

- May26 이후 PUBSUB/tls 주변 core commit 후보는 `fb4855bc6 core: clamp command body sizes`가 사실상 유일하다.
- 해당 변경은 steady-state publish path가 아니라 subscription command parsing 쪽이라 hot path 후보 가능성은 낮다.
- 다음 라운드에서는 현재 HEAD에서 `fb4855bc6`의 xpub/msg command-body clamp만 임시로 되돌려 측정할지, 아니면 core가 아닌 perf harness 변경의 기준 차이를 별도 문서화할지 판단한다.
