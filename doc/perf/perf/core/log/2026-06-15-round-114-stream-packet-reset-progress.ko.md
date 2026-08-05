# Round 114: STREAM packet reset progress only after completed frame

## 이번 라운드 목표

- STREAM packet parser가 완성된 frame을 callback으로 넘긴 뒤 불필요하게 `msg_t` storage를
  close/init하지 않도록 한다.
- 오류/stop 경로는 기존 `reset()`을 유지하고, 완료된 frame을 `move()`한 뒤에는 parser progress만
  초기화하는 `reset_progress()`를 사용한다.
- 완료 기준:
  - focused STREAM tests 통과.
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

1. `msg_t::move()`는 source message를 빈 `init()` 상태로 되돌린다.
2. packet parser는 `state.header`와 `state.body`를 지역 message로 move한 직후 `state.reset()`을 호출한다.
3. 이 reset은 이미 빈 상태인 header/body를 다시 close/init하므로 완료 frame마다 불필요한 작업을 한다.

## POSD 검토

- packet state 객체 안에 `reset_progress()`를 추가해 상태 초기화 책임을 상태 객체에 둔다.
- 오류/stop처럼 보유 중인 message storage를 정리해야 하는 경로는 기존 `reset()`을 그대로 쓴다.
- public API, wire format, callback 계약, perf runner는 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 영역:
  - STREAM packet dispatch parser state reset.
- 보안 의미를 유지한 근거:
  - maxmsgsize 초과 시 disconnect/terminate 처리는 기존 `reset()` 경로를 유지한다.
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - decoder/message/send guard, mtrie, port parsing, IPC unlink 정책을 수정하지 않는다.
  - perf runner/client/server를 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly'
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round114_stream_packet_reset_progress
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
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round114_stream_packet_reset_progress
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_174730_round114_stream_packet_reset_progress.txt`
- status: complete, fail 0.
- load_avg: `24.06 20.18 13.33`.

| Pattern | Transport | round112 guard | round114 focused | Delta |
|---------|-----------|---------------:|-----------------:|------:|
| STREAM | tcp | 324782.4 | 310157.0 | -4.5% |
| STREAM | tls | 213598.6 | 207618.4 | -2.8% |
| STREAM | wss | 182649.6 | 176015.6 | -3.6% |

## 최종 판단

- 미채택.
- fail은 없었지만 round112 대비 모든 STREAM transport가 하락했다.
- load가 높았더라도 개선 항목이 없고 완료 기준을 만족하지 못하므로 source를 되돌렸다.
- 최종 source diff: 없음.
