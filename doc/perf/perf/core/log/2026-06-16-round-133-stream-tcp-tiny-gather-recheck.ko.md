# Round 133: STREAM/TCP tiny gather recheck

## 목표

- round96에서 tcp 단독 +1.75% 신호가 있었지만 전체 전송 실패로 폐기한
  `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD=128` 방향을 재검토한다.
- 전역 env가 아니라 STREAM/TCP에만 제한하면 다른 전송을 건드리지 않을 수 있는지 확인한다.

## 후보

- `core/src/runtime/engine/asio/asio_engine.cpp`
  - `prepare_gather_output()`에서 STREAM이고 transport가 tcp이며 env tiny gather threshold가 지정되지 않은
    경우에만 128B 이하 body를 gather-write 대상으로 삼는 후보를 임시 적용했다.
- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`
  - TCP 기본 tiny gather threshold 상수를 정책 모듈에 둔다.

## POSD 검토

- transport 판정은 이미 ASIO engine의 `is_tcp_transport()`에 모여 있으므로 새 transport 지식을 다른
  모듈로 퍼뜨리지 않는다.
- public API, wire format, STREAM packet parser, perf runner/client/server는 수정하지 않는다.
- 작은 payload 전용 분기를 output policy에만 둔다.

## 기능 검증

- `git diff --check`: 통과
- `cmake --build core/build --target libzlink -j$(nproc)`: 통과
- `ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly'`
  - 20/20 통과

## tcp 단독 후보 측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round133_stream_tcp_tiny_gather_tcp_only_candidate`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_013853_round133_stream_tcp_tiny_gather_tcp_only_candidate.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,1.04 2.63 2.94`
- result:
  `STREAM/tcp/64B = 308423.6 ops/s`
- round132 current 대비:
  `+0.76%`

## all-transport 후보 측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round133_stream_tcp_tiny_gather_all_transport_candidate`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_013936_round133_stream_tcp_tiny_gather_all_transport_candidate.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,0.77 2.35 2.83`
- status: complete, fail 0

| transport | candidate kops |
|---|---:|
| tcp | 322962.8 |
| tls | 223700.2 |
| ws | 277687.6 |
| wss | 188424.4 |

## removed A/B

- candidate를 원복한 뒤 `git diff --check && cmake --build core/build --target libzlink -j$(nproc)` 실행.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round133_stream_tcp_tiny_gather_removed_ab_all_transport`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_014123_round133_stream_tcp_tiny_gather_removed_ab_all_transport.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,1.65 2.41 2.81`
- status: complete, fail 0

| transport | candidate | removed | delta |
|---|---:|---:|---:|
| tcp | 322962.8 | 315996.0 | +2.20% |
| tls | 223700.2 | 222424.8 | +0.57% |
| ws | 277687.6 | 267079.0 | +3.97% |
| wss | 188424.4 | 196815.8 | -4.26% |

## 최종 판단

- 미채택.
- tcp/ws는 좋아졌지만 wss 하락 항목이 남는다.
- 코드상 후보는 TCP에만 적용되지만, 채택 기준은 결과 기준이므로 "하락 항목 없이 플러스"를 만족하지 못한다.
- source 변경은 되돌렸고, 원복 상태로 `core/build` runtime을 다시 빌드했다.
