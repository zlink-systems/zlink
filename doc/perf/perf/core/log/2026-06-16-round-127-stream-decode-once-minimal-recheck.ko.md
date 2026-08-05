# Round 127: STREAM routing-id decode-once 최소 재검토

## 목표

- 이전 round102-104에서 반려했던 STREAM routing-id decode-once 후보를 더 좁은 형태로 재검토한다.
- 사용자가 제안한 기준에 맞춰, 작은 개선이라도 하락 항목이 없으면 채택할 수 있는지 확인한다.
- POSD 기준상 내부 중복 제거만 허용하고, 공개 API나 dispatch 내부 인터페이스는 넓히지 않는다.

## 후보

- `send_stream_message()`에서 callback fast path 비교용 byte 비교를 없애고,
  한 번 파싱한 `uint32_t routing_id`를 현재 callback routing id와 비교한다.
- 대안 B는 stream dispatch 내부 API가 `uint32_t routing_id`를 받게 하는 방식이지만,
  내부 인터페이스가 넓어져 이번 라운드에서는 적용하지 않았다.

## 검증

- `git diff --check`: pass
- `cmake --build core/build -j$(nproc)`: pass
- STREAM 관련 CTest:
  `ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly|test_transport_matrix'`
  - 21/21 pass

## Perf

### 오염 실행

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round127_stream_decode_once_minimal_recheck`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_002310_round127_stream_decode_once_minimal_recheck.txt`
- status:
  complete, fail 0
- 시작 부하:
  `load_avg,43.63 15.05 7.51`
- 판단:
  CTest와 겹쳐 시작 부하가 높으므로 채택 판단에서 제외한다.

### Candidate low-load tcp

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round127_stream_decode_once_minimal_tcp_lowload`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_002704_round127_stream_decode_once_minimal_tcp_lowload.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,1.64 7.76 6.24`
- result:
  `MULTI_STREAM/tcp/64 = 287453.2`

### Removed A/B tcp

- 후보를 원복하고 `cmake --build core/build --target libzlink -j$(nproc)` 수행.
- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round127_stream_decode_once_minimal_removed_ab_tcp`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_002754_round127_stream_decode_once_minimal_removed_ab_tcp.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,3.33 7.23 6.14`
- result:
  `MULTI_STREAM/tcp/64 = 301417.2`

## 판단

- 같은 시간대 A/B에서 후보는 제거 상태보다 낮다.
- 후보는 POSD 관점에서 가능한 작은 정리였지만, 하락 없는 개선 조건을 만족하지 못했다.
- source 변경은 되돌렸다.
- 남은 source diff는 round125 `zlink_spot_send_spot_part()` FINAL-only fast path뿐이다.
