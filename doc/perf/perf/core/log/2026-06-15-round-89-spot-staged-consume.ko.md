# Round 89: SPOT staged publish consume 후보

## 목표

SPOT publish data-plane의 backpressure/staging 경로에서 이미 소유한 message parts를 다시 copy하지 않고
staged queue로 이동할 수 있는지 검증한다.

완료 기준:
- SPOT/PUBSUB 관련 focused CTest 통과
- targeted SPOT 64B perf에서 하락 없이 개선 신호가 있는지 확인
- 효과가 없으면 source 변경 원복

## 기준 report

- May26 full 보정 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 최신 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음
- 최근 미채택 후보:
  - round86: ASIO output loop 중복 제거
  - round87: mailbox/wakeup triage
  - round88: message/pipe triage

## 병목 가설

1. `drain_pub_ingress_socket()`는 recv한 `frames`를 이미 소유한다. EAGAIN으로 staged queue에 넣을 때
   `stage_message()`가 다시 copy한 뒤 호출부가 원본을 clear하므로, backpressure/staging 경로에서 불필요한
   LMSG refcount/copy 비용이 생긴다.
2. SPOT 64B steady-state perf는 대부분 immediate fanout으로 끝나며 staged path를 거의 타지 않는다. 이 경우
   consume 변경은 구조적으로는 맞지만 targeted throughput 개선은 없을 수 있다.

먼저 가설 1을 최소 변경으로 검증한다.

## POSD 점검

- 새 상태나 캐시를 추가하지 않는다.
- caller가 이미 소유한 `spot_owned_msg_parts_t`를 staged queue로 이동하는 명시적 consume API를 추가한다.
- 기존 copy API는 유지해서 다른 caller의 ownership 의미를 바꾸지 않는다.
- perf 조건, runner, client/server는 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 사본 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
    decoder/message/send guard, maxmsgsize 정책을 변경하지 않는다.
  - SPOT 내부 staged queue ownership 이동만 다룬다.
- 추가로 실행한 회귀 테스트:
  - `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane|test_spot_pubsub_scenario|test_spot_poller|test_spot_runtime_activation|test_spot_dispatch_event|test_spot_router_channel_peer|test_multi_socket_contract_regressions|test_transport_matrix|test_pubsub'`

## 변경

- 파일:
  - `core/src/runtime/services/spot/data_plane/spot_data_plane_internal.hpp`
  - `core/src/runtime/services/spot/data_plane/spot_data_plane_pending.cpp`
  - `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- 내용:
  - `stage_publish_message_consume()`와 `stage_message_consume()`을 임시 추가했다.
  - `drain_pub_ingress_socket()`에서 `frames`를 staged queue에 넣는 EAGAIN 경로가 copy 대신 move를 쓰도록
    임시 변경했다.

## 검증

### build

```bash
cmake --build core/build -j$(nproc)
```

- 결과: 통과

### test

```bash
ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane|test_spot_pubsub_scenario|test_spot_poller|test_spot_runtime_activation|test_spot_dispatch_event|test_spot_router_channel_peer|test_multi_socket_contract_regressions|test_transport_matrix|test_pubsub'
```

- 결과: 19/19 통과

```bash
git diff --check
```

- 결과: 통과

### perf

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round89_spot_staged_consume_tcp_tls
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_144600_round89_spot_staged_consume_tcp_tls.txt`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- load_avg: `25.30 12.17 10.04`
- 결과:

| case | May26 full | round70 current | round89 candidate | vs May26 full | vs round70 |
|------|------------|-----------------|-------------------|---------------|------------|
| SPOT/tcp/64B | 3,962,360.0 | 3,974,620.0 | 3,984,660.0 | +0.56% | +0.25% |
| SPOT/tls/64B | 5,939,903.4 | 6,906,405.8 | 5,610,939.2 | -5.54% | -18.76% |

## 판정

- focused tests는 통과했다.
- `SPOT/tcp`는 round70 대비 `+0.25%`로 오차 범위다.
- `SPOT/tls`는 round70 대비 크게 낮다. 후보가 TLS를 직접 건드리지는 않지만, 하락 없는 개선 후보로
  유지할 근거가 없다.
- 이 consume 변경은 backpressure/staging 경로에서만 의미가 있어 steady-state SPOT 64B 목표에도 직접
  기여하지 못했다.
- source 변경은 원복했다.

## 현재 상태

- source diff 없음.
- SPOT staged ownership 후보는 보류하지 않고 폐기한다.
