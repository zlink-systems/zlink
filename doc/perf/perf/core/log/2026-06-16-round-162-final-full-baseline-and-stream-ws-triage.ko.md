# Round 162: final full baseline 저장 및 STREAM ws 실패 정리

## goal

- 요청대로 C single, C multi full 테스트 결과를 `bindings/c/perf/baseline/`에 저장한다.
- multi full이 complete가 아니라 partial로 끝난 이유를 별도 재현으로 확인한다.
- 이번 라운드에서는 core, perf runner, perf client/server 코드를 수정하지 않는다.

## baseline 저장 결과

single full:

- baseline:
  `bindings/c/perf/baseline/perf_c_single_linux_20260616_104952_final_retained_spot_single_full_baseline_20260616.txt`
- source report:
  `bindings/c/perf/results/single/report/perf_c_single_linux_20260616_104952_final_retained_spot_single_full_baseline_20260616.txt`
- status: `complete`
- expected/result lines: `720/720`

multi full:

- baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260616_081700_final_retained_spot_multi_full_baseline_20260616.txt`
- source report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_081700_final_retained_spot_multi_full_baseline_20260616.txt`
- status: `partial`
- success/fail: `180/12`
- expected/result lines: `960/900`
- 실패 항목: `MULTI_STREAM current ws 64B..131072B`

## 실행 기준

multi full:

```bash
PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag final_retained_spot_multi_full_baseline_20260616
```

single full:

```bash
PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks.sh --reuse-build \
  --pattern ALL --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 \
  --results-tag final_retained_spot_single_full_baseline_20260616
```

## 주요 수치

single SPOT 64B:

| item | throughput |
|------|-----------:|
| `SPOT/tcp` | 350032.400 |
| `SPOT/tls` | 377400.400 |
| `SPOT/ws` | 356661.600 |
| `SPOT/wss` | 399567.800 |

multi STREAM 64B, partial report에서 결과가 나온 transport:

| item | throughput |
|------|-----------:|
| `MULTI_STREAM/tcp` | 240002.200 |
| `MULTI_STREAM/tls` | 186733.800 |

`MULTI_STREAM/ws`와 `MULTI_STREAM/wss`는 full report에서 결과가 누락됐다.

## STREAM ws 재현

standalone STREAM ws:

```bash
PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports ws \
  --duration 5 --runs 1 \
  --connect-ready-timeout-ms 5000 \
  --results-tag stream_ws_failure_repro_20260616
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_115733_stream_ws_failure_repro_20260616.txt`
- status: `complete`
- `MULTI_STREAM/ws/64B`: `234341.800`

STREAM all transports:

```bash
PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp,tls,ws,wss \
  --duration 5 --runs 1 \
  --connect-ready-timeout-ms 5000 \
  --results-tag stream_all_transport_repro_20260616
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_115830_stream_all_transport_repro_20260616.txt`
- status: `complete`
- 64B: tcp `282349.800`, ws `239419.200`, wss `174604.400`

SPOT_SENDSEND 뒤 STREAM:

```bash
PERF_DEBUG=1 PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss \
  --duration 5 --runs 1 \
  --connect-ready-timeout-ms 5000 \
  --results-tag spotsendsend_stream_debug_20260616
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_120653_spotsendsend_stream_debug_20260616.txt`
- status: `partial`
- failure detail:
  `case_failed size=1024 connect_ok=0 connect_fail=10000 send_error=0 recv_error=10000 timeout_error=0`

## 판단

- baseline 저장 요청은 완료했다.
- single full은 complete다.
- multi full은 결과 파일을 baseline에 저장했지만 complete가 아니라 partial이다.
- standalone STREAM ws/wss가 통과하므로 단순히 ws stream 자체가 항상 실패하는 상태는 아니다.
- SPOT_SENDSEND 뒤 STREAM 재현에서 새 size 단계의 10000개 연결이 모두 실패했다.
  따라서 처리량 계산 문제가 아니라 반복 실행 뒤 ws STREAM 서버/accept/연결 수명주기 또는
  로컬 자원 회수 문제로 보는 것이 맞다.

## POSD/security 상태

- 이번 라운드는 코드 변경 없음.
- public API, wire format, 보안 하드닝 항목 변경 없음.
- perf runner/client/server도 수정하지 않았다.
