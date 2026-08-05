# Round 85: STREAM current pipe 조회 중복 제거 재검토

## 목적

이전 round49에서 버렸던 `STREAM/tcp/64B` current-send hot path 후보를 새 기준으로 다시 확인했다.
과거 기준에서는 10% 이상 반복 개선이 아니면 버렸지만, 지금은 작은 개선이라도 하락 항목이 없고
POSD를 해치지 않으면 채택할 수 있는지 재검토한다.

## POSD 점검

- 새 캐시, 새 상태, 새 public API를 추가하지 않는다.
- 내부 helper가 이미 읽은 dispatch pipe를 다시 TLS에서 읽지 않도록 인자로 받는 변경이다.
- current-send 경로 내부의 중복 조회를 줄이는 변경이라 설계 방향은 단순화에 가깝다.
- 다만 hot path 계약을 건드리는 내부 변경이므로, 성능 신호가 불안정하면 유지하지 않는다.

## 적용한 변경

- 파일:
  - `core/src/runtime/sockets/stream/stream_dispatch_send_policy_internal.hpp`
  - `core/src/runtime/sockets/stream/stream_dispatch_send.cpp`
- `stream_dispatch_send_current_msg_from_io()`에서 이미 읽은 `dispatch_pipe`를
  `resolve_current_dispatch_output_pipe()`에 넘겨 TLS current pipe 조회를 한 번 줄였다.

## 검증

### build

```bash
cmake --build core/build -j$(nproc)
```

- 결과: 통과

### test

```bash
ctest --test-dir core/build --output-on-failure -R 'test_stream|test_multi_stream_server_reassembly|test_transport_matrix|test_zmp_request_reply'
```

- 결과: 23/23 통과

```bash
git diff --check
```

- 결과: 통과

### perf: STREAM/tcp 단독

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round85_stream_current_pipe_recheck_tcp
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_143134_round85_stream_current_pipe_recheck_tcp.txt`
- load_avg: `11.28 12.08 11.22`
- `STREAM/tcp/64B`: `333,690.0 ops/s`
- round70 `332,250.4 ops/s` 대비 `+0.43%`

### perf: STREAM 전체 전송 1차

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round85_stream_current_pipe_recheck_all
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_143145_round85_stream_current_pipe_recheck_all.txt`
- load_avg: `9.84 11.75 11.12`
- `STREAM/tcp/64B`: `332,143.0 ops/s`
- `STREAM/tls/64B`: 실패, `non_zero_exit_2_size_64`
- 판정: partial 실패라 채택 근거로 쓰지 않는다. TLS 단독 재실행으로 실패 재현 여부를 확인했다.

### perf: STREAM/tls 단독

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tls --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round85_stream_current_pipe_recheck_tls_standalone
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_143214_round85_stream_current_pipe_recheck_tls_standalone.txt`
- load_avg: `6.81 10.79 10.83`
- `STREAM/tls/64B`: `222,174.6 ops/s`
- 결과: 성공

### perf: STREAM 전체 전송 2차

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round85_stream_current_pipe_recheck_all_retry
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_143236_round85_stream_current_pipe_recheck_all_retry.txt`
- load_avg: `5.54 10.25 10.65`
- 결과: success 4, fail 0

| case | May26 full | round70 current | round85 retry | vs May26 full | vs round70 |
|------|------------|-----------------|---------------|---------------|------------|
| STREAM/tcp/64B | 305,177.4 | 332,250.4 | 329,448.4 | +7.95% | -0.84% |
| STREAM/tls/64B | 214,574.6 | 217,262.2 | 226,815.6 | +5.70% | +4.40% |
| STREAM/ws/64B | 251,311.4 | 282,312.6 | 284,869.4 | +13.35% | +0.91% |
| STREAM/wss/64B | 184,722.2 | 177,889.2 | 187,706.0 | +1.62% | +5.52% |

## 판정

- 단독 `STREAM/tcp`에서는 `+0.43%`였지만, 전체 전송 재시도에서는 `STREAM/tcp`가 round70보다
  `-0.84%` 낮았다.
- TLS 단독 재실행은 성공했으므로 1차 실패는 sequence 흔들림일 가능성이 있다.
- 그러나 핵심 목표 항목인 `STREAM/tcp/64B` 개선이 반복되지 않았다.
- 이 후보는 POSD 관점에서는 깨끗하지만, "하락 항목 없이 +"라는 채택 조건을 만족한다고 보기 어렵다.
- 따라서 source 변경은 되돌렸다.

## 현재 상태

- source diff 없음.
- 작은 개선 후보라도 현재값 대비 반복 개선이 없거나 핵심 항목이 내려가면 유지하지 않는다.
