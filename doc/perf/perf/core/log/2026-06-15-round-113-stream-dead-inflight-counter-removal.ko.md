# Round 113: STREAM dead inflight counter removal

## 이번 라운드 목표

- STREAM dispatch callback마다 실행되는 `_dispatch_inflight` atomic add/sub 비용을 제거한다.
- 현재 counter는 증가/감소와 internal virtual getter만 있고, close/detach/admission 판단에 쓰이지 않는다.
- 완료 기준:
  - focused STREAM lifecycle/threadsafe tests 통과.
  - `STREAM` 64B tcp/tls/wss focused perf에서 실패 0개.
  - round112 guard 대비 하락 항목이 있으면 source를 되돌린다.

## 기준 report

- round112 guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_173323_round112_pubsub_stream_guard_rerun.txt`

## 기준 수치

| Pattern | Transport | round112 guard |
|---------|-----------|---------------:|
| STREAM | tcp | 324782.4 ops/s |
| STREAM | tls | 213598.6 ops/s |
| STREAM | wss | 182649.6 ops/s |

## 병목 가설

1. STREAM packet callback hot path에서 `_dispatch_inflight.fetch_add()`와 `fetch_sub()`가 매 메시지 실행된다.
2. 현재 counter 값은 검색 기준으로 실제 판단 경로에서 읽히지 않는다.
3. callback 중 self close/detach 차단은 TLS 기반 `stream_dispatch_in_callback()`이 담당한다.

## POSD 검토

- 죽은 상태를 제거해 모듈 내부 상태를 줄인다.
- public API, wire format, handler signature, perf runner는 바꾸지 않는다.
- lifecycle 의미는 기존 테스트가 검증하는 `stream_dispatch_in_callback()` 기반 EBUSY 정책을 유지한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 영역:
  - STREAM dispatch callback accounting.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - maxmsgsize, decoder/message/send guard 정책을 수정하지 않는다.
  - mtrie, port parsing, IPC unlink 정책을 수정하지 않는다.
  - perf runner/client/server를 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly'
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round113_stream_dead_inflight_counter_removal
```

## 실행 결과

### Build

```bash
cmake --build core/build -j$(nproc)
```

- 결과: 성공.
- runtime: `core/build/lib/libzlink.so.6.0.4`.

### Focused tests

```bash
ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly'
```

- 결과: 20/20 passed.

### STREAM focused perf

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round113_stream_dead_inflight_counter_removal
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_174025_round113_stream_dead_inflight_counter_removal.txt`
- status: complete, fail 0.
- load_avg: `20.66 12.71 8.57`.

| Pattern | Transport | round112 guard | round113 focused | Delta |
|---------|-----------|---------------:|-----------------:|------:|
| STREAM | tcp | 324782.4 | 286879.2 | -11.7% |
| STREAM | tls | 213598.6 | 204416.4 | -4.3% |
| STREAM | wss | 182649.6 | 152661.6 | -16.4% |

## 최종 판단

- 미채택.
- 모든 STREAM transport가 round112 guard보다 하락했다.
- focused run의 load가 높았지만 tcp/wss 하락폭이 크고, 완료 기준이 하락 항목 없음이므로 source를 되돌렸다.
- 최종 source diff: 없음.
