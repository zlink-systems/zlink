# Round 161: ROUTER/DEALER one-way 재분석

## goal

- 현재 retained 상태에서 전체 64B 중앙값과 one-way 평균 목표를 누르는 항목을 다시 정렬한다.
- 이미 반복 배제한 PUBSUB/SPOT transport 후보 대신 ROUTER/DEALER 공통 hot path에
  POSD-safe 후보가 남아 있는지 확인한다.
- 완료 기준: source 후보가 있으면 build/test/focused perf를 통과하고,
  없으면 source 변경 없이 근거를 남긴다.

## 기준

- 과거 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 사용자 보정 기준:
  - May26 smoke:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - May26 full:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 문제 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained full64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064431_round156_retained_spot_final_fastpath_full64_refresh.txt`

## 시작 상태

- core source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()` 단일 FINAL fast path.
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.
- `git diff --check -- core/src core/include core/tests`: 통과.

## 병목 가설

1. `DEALER_ROUTER`와 `ROUTER_ROUTER`의 낮은 개선폭은 router routing-id lookup,
   out-pipe active/weight bookkeeping, multipart envelope 처리 비용에서 온다.
2. `DEALER_DEALER`과 `DEALER_ROUTER`는 `lb_t`/`fq_t` 공통 pipe hot path 영향을 받지만,
   기존 단일 pipe fast path 이후에는 HWM/fairness 계약 때문에 남은 안전 후보가 작다.
3. `PUBSUB/tls`와 `SPOT/wss`는 여전히 낮지만 기존 후보가 하락 항목을 만들었으므로,
   같은 transport policy 변경을 반복하지 않는다.

## 먼저 검증할 가설

- 문제 report 대비 round156에서 개선폭이 낮은 one-way 항목을 다시 정렬하고,
  ROUTER/DEALER 계열이 실제로 중앙값을 누르는지 확인한다.
- 그 다음 router/dealer send/recv hot path에서 public 계약과 fairness 상태를 늘리지 않는 후보만 검토한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 아래 후보는 `fq_t` 내부 수신 경로만 임시로 바꿨다가 되돌렸다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending-copy 제거, mtrie 비재귀화, port parsing, IPC unlink 순서,
    decoder/message/send guard, `maxmsgsize` 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - `dealer|router|stream|pubsub|multi_socket_contract_regressions|backpressure_(oneway_)?matrix`
    focused CTest 48/48 통과.

## 현재 낮은 개선 항목 재정렬

문제 report 대비 round156 retained full64에서 낮은 항목:

| item | delta |
|------|------:|
| `MULTI_PUBSUB/tls` | -1.17% |
| `MULTI_PUBSUB/wss` | -0.23% |
| `MULTI_PUBSUB/ws` | -0.08% |
| `MULTI_DEALER_DEALER/ws` | +0.61% |
| `MULTI_DEALER_DEALER/tls` | +1.14% |
| `MULTI_DEALER_DEALER/tcp` | +1.49% |
| `MULTI_DEALER_DEALER/wss` | +1.83% |
| `MULTI_DEALER_ROUTER/wss` | +2.61% |
| `MULTI_ROUTER_ROUTER/tls` | +3.17% |
| `MULTI_PUBSUB/tcp` | +3.68% |
| `MULTI_ROUTER_ROUTER/ws` | +4.21% |

현재 공통 64B는 평균 `+8.46%`, 중앙값 `+5.17%`다.

## 코드 확인

읽은 파일:

- `core/src/runtime/sockets/internal/fq.cpp`
- `core/src/runtime/sockets/internal/fq.hpp`
- `core/src/runtime/sockets/internal/lb.cpp`
- `core/src/runtime/sockets/dealer/dealer.cpp`
- `core/src/runtime/sockets/router/router_recv_path.cpp`
- `core/src/runtime/sockets/router/router_send_path.cpp`

확인 내용:

- `lb_t::sendpipe()`에는 단일 active pipe fast path가 이미 있다.
- `fq_t::recvpipe()`와 `fq_t::has_in()`은 단일 active pipe에서도 일반 fair-queue loop를 탄다.
- DEALER/ROUTER 수신 경로는 `_fq.recvpipe()`에 의존하므로 단일 pipe 경로를 분리하면
  one-way와 routed echo에 영향을 줄 수 있다.

## 후보: `fq_t` 단일 active pipe fast path

적용한 임시 후보:

- `fq_t::recvpipe()`에서 `_active == 1 && _current == 0`이면 일반 round-robin loop를 건너뛰고
  `_pipes[0]->read()`를 바로 호출한다.
- `fq_t::has_in()`에서도 같은 단일 pipe 조건에서 `check_read()`를 바로 호출한다.

POSD 검토:

- public API, socket contract, wire format은 바꾸지 않는다.
- 단일 pipe에서는 fairness 선택지가 없으므로 fair-queue 정책을 약화하지 않는다.
- 구현은 `fq_t` 내부에 머물며 호출자 조건을 늘리지 않는다.

기능 검증:

```bash
cmake --build core/build --target libzlink -j$(nproc)
ctest --test-dir core/build --output-on-failure \
  -R 'dealer|router|stream|pubsub|multi_socket_contract_regressions|backpressure_(oneway_)?matrix'
```

- build: 통과.
- CTest: 48/48 통과.

targeted perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round161_fq_single_pipe_candidate_router_dealer
```

- runtime: `core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_075114_round161_fq_single_pipe_candidate_router_dealer.txt`
- start load: `0.45 0.87 1.79`
- status: success `12`, fail `0`

후보 vs round156 retained full64:

| item | candidate | round156 | delta |
|------|----------:|---------:|------:|
| `MULTI_DEALER_DEALER/tcp` | 3092683.4 | 3091049.6 | +0.05% |
| `MULTI_DEALER_DEALER/tls` | 3208573.4 | 3198925.2 | +0.30% |
| `MULTI_DEALER_DEALER/ws` | 3155934.4 | 3175943.0 | -0.63% |
| `MULTI_DEALER_DEALER/wss` | 3316736.2 | 3325069.2 | -0.25% |
| `MULTI_DEALER_ROUTER/tcp` | 424997.0 | 438076.6 | -2.99% |
| `MULTI_DEALER_ROUTER/tls` | 385163.2 | 398925.2 | -3.45% |
| `MULTI_DEALER_ROUTER/ws` | 371723.6 | 419431.8 | -11.37% |
| `MULTI_DEALER_ROUTER/wss` | 346892.0 | 381074.8 | -8.97% |
| `MULTI_ROUTER_ROUTER/tcp` | 372988.6 | 424599.8 | -12.16% |
| `MULTI_ROUTER_ROUTER/tls` | 349161.6 | 385316.6 | -9.38% |
| `MULTI_ROUTER_ROUTER/ws` | 371932.8 | 408391.6 | -8.93% |
| `MULTI_ROUTER_ROUTER/wss` | 339172.8 | 367180.6 | -7.63% |

## 후보 판정

- 기각하고 되돌렸다.
- `DEALER_DEALER`에서는 보합이지만 `DEALER_ROUTER`, `ROUTER_ROUTER`에서 큰 하락이 반복됐다.
- 단일 pipe fast path가 겉보기에는 loop 비용을 줄이지만, active pipe 재활성화와 routed echo 경로의
  흐름에는 유리하지 않았다.
- 사용자 기준인 "하락 항목 없이 플러스면 채택"을 만족하지 못한다.

## 최종 상태

- `fq_t` 후보는 원복했다.
- 원복 뒤 `cmake --build core/build --target libzlink -j$(nproc)` 통과.
- 최종 core source diff는 기존 `zlink_spot_send_spot_part()` 단일 FINAL fast path 하나만 남아 있다.
- 이번 라운드는 새 source 변경 없음.
