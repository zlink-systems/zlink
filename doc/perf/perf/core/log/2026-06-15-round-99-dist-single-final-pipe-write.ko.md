# Round 99: dist single final pipe write 후보

## goal

PUBSUB one-way 64B fanout에서 `dist_t`가 final non-routing payload를 각 pipe에 쓸 때 반복되는
message flag/routing-id 확인을 줄인다.

완료 기준:

- core build 통과
- PUB/SUB와 backpressure 관련 focused CTest 통과
- `PUBSUB tcp,tls,wss 64B` targeted perf에서 하락 항목 없이 개선 신호 확인
- 개선이 없거나 하락하면 source 변경을 되돌린다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round90 current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_145417_round90_current_regression_recalibration.txt`
- round97 retained guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153913_round97_retained_spot_fastpath_reduced_guard.txt`

## 병목 가설

1. `PUBSUB` 64B one-way는 `xpub_t::xsend()` -> `dist_t::send_to_matching()` ->
   `dist_t::distribute()` -> `pipe_t` write 경로를 100 subscriber pipe에 반복한다.
2. final 단일 payload인 경우 `dist_t`는 message가 `more=false`이고 routing id가 아님을 한 번 확인할 수
   있다. 그런데 현재는 각 pipe write에서 `msg_->flags()`와 `msg_->is_routing_id()`를 다시 본다.
3. `pipe_t::write_single_message_and_flush_no_recursive_hwm_check()`는 이미 STREAM/ROUTER fast path에서
   쓰는 기존 helper이며, final non-routing payload 의미와 맞는다.

먼저 검증할 가설:

- `dist_t` 내부에서 final non-routing payload일 때 기존 single-message pipe helper를 호출하면
  PUBSUB fanout에서 작은 반복 비용이 줄어든다.

## 변경 계획

- `core/src/runtime/sockets/internal/dist.cpp`
  - final non-routing payload일 때만 `write_single_message_and_flush_no_recursive_hwm_check()`를 사용한다.
  - multipart, routing-id, HWM, deactivate semantics는 기존 경로를 유지한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, port parsing, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행할 회귀 테스트:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|multi_socket_contract_regressions|backpressure_oneway_matrix|backpressure_matrix|transport_matrix|spot_pubsub_scenario)$'`

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure \
  -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|multi_socket_contract_regressions|backpressure_oneway_matrix|backpressure_matrix|transport_matrix|spot_pubsub_scenario)$'
```

- build: pass
- focused CTest: 6/6 pass
- 첫 build에서 clock skew 경고가 있어 `sleep 2 && cmake --build core/build -j$(nproc)`를 재실행했고,
  재실행은 경고 없이 pass했다.

## perf 1

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round99_dist_single_final_pipe_write_pubsub
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155125_round99_dist_single_final_pipe_write_pubsub.txt`
- status: complete
- success: 3
- fail: 0
- start load_avg: `27.05 13.17 8.92`

| case | round97 retained guard | round99 candidate | delta |
|---|---:|---:|---:|
| PUBSUB/tcp/64B | 2,395,925.0 | 2,462,993.6 | +2.80% |
| PUBSUB/tls/64B | 2,253,781.4 | 2,191,708.4 | -2.75% |
| PUBSUB/wss/64B | 2,486,570.4 | 2,453,509.2 | -1.33% |

## perf 2

첫 perf는 시작 load가 높아 낮은 load에서 재실행했다.

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round99_dist_single_final_pipe_write_pubsub_retry
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155353_round99_dist_single_final_pipe_write_pubsub_retry.txt`
- status: complete
- success: 3
- fail: 0
- start load_avg: `3.88 8.54 7.82`

| case | round97 retained guard | round99 retry | delta |
|---|---:|---:|---:|
| PUBSUB/tcp/64B | 2,395,925.0 | 2,376,180.8 | -0.82% |
| PUBSUB/tls/64B | 2,253,781.4 | 2,206,231.4 | -2.11% |
| PUBSUB/wss/64B | 2,486,570.4 | 2,464,771.6 | -0.88% |

## 판정

- focused tests는 통과했다.
- 하지만 재실행에서도 `tcp/tls/wss` 모두 round97 retained guard보다 낮았다.
- 기존 pipe helper를 재사용하는 구조는 POSD 관점에서 작고 명확하지만, 성능 목표에 기여하지 못한다.
- 후보는 폐기했고 `dist.cpp` source 변경은 되돌렸다.
- 되돌린 뒤 `cmake --build core/build -j$(nproc)`를 다시 실행해 `core/build` runtime을 현재 source와
  맞췄다.

## 현재 상태

- `dist.cpp` diff 없음.
- retained source diff는 round92 `SPOT_SENDSEND` 단일 FINAL fast path뿐이다.
