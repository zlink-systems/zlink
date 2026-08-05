# Round 130: STREAM current dispatch pipe resolve

## 목표

- STREAM packet echo hot path의 current-dispatch send에서 중복 TLS 조회를 줄인다.
- 완료 기준:
  - core build 통과.
  - STREAM 관련 CTest 통과.
  - STREAM tcp/ws/wss targeted perf에서 하락 없이 개선 신호가 있어야 한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round122:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`
- round129:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_003229_round129_retained_spot_fastpath_lowload_all64_reduced_full.txt`

## 병목 가설

1. STREAM packet echo는 callback 안에서 바로 같은 routing-id로 응답을 보낸다.
2. `send_stream_message()`는 callback 중 같은 routing-id일 때 `stream_dispatch_send_current_msg_from_io()` fast path를 탄다.
3. 이 fast path는 `current_pipe()`를 읽은 뒤 `resolve_current_dispatch_output_pipe()`에서 같은 TLS pipe를 다시 읽는다.
4. helper가 감추는 정보가 거의 없고 호출자도 이미 current pipe 의미를 알고 있으므로, helper를 없애면 POSD의 얕은 모듈 위험도 줄어든다.

## 먼저 검증할 가설

- helper 제거와 단일 current pipe 조회가 STREAM/tcp/ws의 작은 하락을 줄이는지 확인한다.

## 보안 하드닝 보존 확인

- WS/WSS pending-copy 제거, mtrie 비재귀화, 포트 파싱, IPC unlink, decoder/message/send guard,
  maxmsgsize 정책은 수정하지 않는다.
- perf runner/client/server는 수정하지 않는다.

## 실행 결과

### Build

- `git diff --check`: pass
- `cmake --build core/build -j$(nproc)`: pass
- runtime:
  `core/build/lib/libzlink.so.6.0.4`

### STREAM 관련 CTest

- 명령:
  `ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly|test_transport_matrix'`
- 결과:
  21/21 pass

### Candidate focused perf

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,ws,wss --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round130_stream_current_pipe_resolve_tcp_ws_wss`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_010917_round130_stream_current_pipe_resolve_tcp_ws_wss.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,6.95 8.74 5.70`
- status:
  complete, fail 0

| transport | candidate |
|-----------|----------:|
| tcp | 323821.8 |
| ws | 264726.2 |
| wss | 176411.2 |

### Removed A/B

- 후보 제거 후 `cmake --build core/build --target libzlink -j$(nproc)` 수행.
- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,ws,wss --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round130_stream_current_pipe_resolve_removed_ab_tcp_ws_wss`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_011110_round130_stream_current_pipe_resolve_removed_ab_tcp_ws_wss.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,2.56 6.68 5.30`
- status:
  complete, fail 0

| transport | candidate | removed | delta |
|-----------|----------:|--------:|------:|
| tcp | 323821.8 | 313456.8 | +3.31% |
| ws | 264726.2 | 258448.0 | +2.43% |
| wss | 176411.2 | 194351.4 | -9.23% |

## 판단

- candidate는 tcp/ws에서 작은 개선 신호가 있지만 wss가 크게 낮다.
- 사용자가 제안한 "작은 개선도 하락 항목이 없으면 채택" 기준에 맞지 않는다.
- source 변경은 되돌렸다.
- 최종 source diff는 round125 SPOT FINAL-only fast path뿐이다.
