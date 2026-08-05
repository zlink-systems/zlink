# Round 121: STREAM current 재측정과 batch policy probe

## 이번 라운드 목표

- 사용자가 기준으로 정정한 May26 report에 맞춰 `MULTI_STREAM 64B` current를 다시 잰다.
- 특히 `tcp 64B`가 실제로 하락 중인지 확인한다.
- source 변경 없이 `ZLINK_ASIO_STREAM_BATCH_SIZE` 환경변수 probe로 batch floor 방향성만 확인한다.
- POSD 기준:
  - perf runner/client/server는 수정하지 않는다.
  - STREAM socket public 계약이나 packet dispatch 소유권을 건드리지 않는다.
  - transport별 실패가 있는 후보는 채택하지 않는다.

## 기준 report

- corrected smoke baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음.
- `cmake --build core/build --target libzlink -j$(nproc)` 통과.
- perf runner가 사용한 runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`

## STREAM current same-window

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round121_stream_current_same_window`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222403_round121_stream_current_same_window.txt`
- status: complete
- load_avg: `1.15 2.52 2.68`

| transport | current kops |
|-----------|--------------|
| tcp | 308.833 |
| tls | 193.585 |
| ws | 234.818 |
| wss | 175.009 |

### corrected May26 full 대비

| transport | May26 full kops | round121 current kops | delta |
|-----------|-----------------|-----------------------|-------|
| tcp | 305.177 | 308.833 | +1.20% |
| tls | 214.575 | 193.585 | -9.78% |
| ws | 251.311 | 234.818 | -6.56% |
| wss | 184.722 | 175.009 | -5.26% |

### corrected May26 smoke 대비

| transport | May26 smoke kops | round121 current kops | delta |
|-----------|------------------|-----------------------|-------|
| tcp | 325.470 | 308.833 | -5.11% |
| tls | 229.781 | 193.585 | -15.75% |
| ws | 263.180 | 234.818 | -10.78% |
| wss | 200.642 | 175.009 | -12.78% |

## batch policy probe

### `ZLINK_ASIO_STREAM_BATCH_SIZE=2048`

- command:
  `ZLINK_ASIO_STREAM_BATCH_SIZE=2048 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round121_stream_batch2048_probe`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222541_round121_stream_batch2048_probe.txt`
- status: partial
- tcp: `303.546 kops`
- failure:
  `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`
- 판단:
  tcp도 기본값 current보다 낮고 tls 실패가 있어 배제한다.

### `ZLINK_ASIO_STREAM_BATCH_SIZE=8192`

- command:
  `ZLINK_ASIO_STREAM_BATCH_SIZE=8192 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round121_stream_batch8192_probe`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222611_round121_stream_batch8192_probe.txt`
- status: partial
- tcp: `323.220 kops`
- failure:
  `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`
- 판단:
  tcp는 좋아 보이지만 tls 실패가 있으므로 기본값 후보로 채택하지 않는다.

### 기본값 tls-only rerun

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round121_stream_tls_current_rerun`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222646_round121_stream_tls_current_rerun.txt`
- status: partial
- failure:
  `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`

## 코드 triage

- `core/src/runtime/sockets/stream/stream.cpp`
  - routing id가 있는 단일 frame send는 shard lookup 후
    `write_single_message_and_flush_no_recursive_hwm_check`로 바로 내려가는 fast path가 있다.
  - dispatch callback에서 같은 pipe로 되돌려 보내는 경로도 direct output pipe를 우선 사용한다.
  - packet dispatch는 frame prefix/header/body 상태와 `maxmsgsize` 검사를 포함하므로 성능만 보고 단순화하지 않는다.
- `core/src/runtime/sockets/stream/stream_batch_policy.hpp`
  - STREAM batch floor는 `ZLINK_ASIO_STREAM_BATCH_SIZE` 환경변수로 이미 한 곳에 모여 있다.
  - 기본값을 바꾸는 후보는 모든 transport 성공과 하락 없음이 확인되어야 한다.
- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`
  - STREAM read/write target, speculative write, gather threshold 정책이 한 모듈에 모여 있다.
  - transport별 정책을 임의로 분산시키는 변경은 POSD 정보 은닉을 해친다.

## 판단

- corrected May26 full 기준에서 `STREAM tcp 64B`는 current가 +1.20%다.
  따라서 사용자가 지적한 tcp 64B 하락은 현재 same-window 단독 측정에서는 재현되지 않았다.
- corrected May26 smoke 기준으로 보면 tcp는 -5.11%라 약한 하락이지만, full 기준과 충돌한다.
  사용자가 최근에 정정한 비교 기준에서는 full/smoke 모두 참고하되, full을 우선 기준으로 본다.
- TLS/WS/WSS는 full 기준에서도 -5% 이상 낮다.
  다만 같은 바이너리에서 이후 tls-only가 실패했고, 별도 Node sample 프로세스가 CPU를 많이 쓰고 있어
  현재 창의 TLS 실패를 source 회귀로 단정하지 않는다.
- batch size 2048/8192 probe는 둘 다 tls 실패가 있어 미채택이다.
- source 변경을 남기지 않는다.

## 다음 후보

- 환경 부하가 낮은 상태에서 `STREAM tls,ws,wss 64B`를 다시 분리 측정한다.
- 실패가 반복되면 perf runner 변경 없이 core TLS/WS stream transport 경로의 에러 원인을 먼저 찾는다.
- tcp 400kops 목표는 아직 미달이지만, 현재 증거상 May26 full 대비 tcp 하락 복구보다 새로운 tcp 개선 후보가 필요하다.
