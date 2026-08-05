# Round 112: ASIO process_output helper unification

## 이번 라운드 목표

- ASIO output hot path에서 중복된 encoder-fill 루프를 하나로 모은다.
- `process_output()`이 `prepare_output_buffer()`를 사용하게 해서 STREAM async output 경로도 같은
  encoder target resize/growth 정책을 적용한다.
- 완료 기준:
  - focused STREAM/ASIO tests 통과.
  - `STREAM` 64B tcp/tls/wss focused perf에서 실패 0개.
  - 하락 항목이 있으면 source를 되돌린다.
  - 개선폭이 5% 미만이라도 하락이 없고 복잡성이 줄면 보수적으로 재검토한다.

## 기준 report

- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round110 current guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_171900_round110_lowload_current_guard.txt`

## 기준 수치

- May26 smoke STREAM 64B: tcp 325470.0, tls 229781.0, ws 263180.0, wss 200642.0 ops/s.
- May26 full STREAM 64B: tcp 305177.4, tls 214574.6, ws 251311.4, wss 184722.2 ops/s.
- round110 STREAM 64B: tcp 252199.0, tls 184091.4, wss 157238.8 ops/s.

## 병목 가설

1. `prepare_output_buffer()`와 `process_output()`가 같은 encoder-fill 루프를 따로 가진다.
2. `prepare_output_buffer()`에는 STREAM encoder target resize/growth 적용이 있지만,
   `process_output()`에는 그 정책이 빠져 있다.
3. speculative write가 꺼진 tls/ws/wss 같은 async output 경로에서는 `process_output()`가 더 자주
   쓰이므로, 같은 정책을 쓰게 하면 transport별 output batching 편차를 줄일 수 있다.

## POSD 검토

- 중복된 output-buffer 지식을 한 helper로 모은다. 정보 은닉과 깊은 모듈 원칙에 맞다.
- public API, wire format, transport API, perf runner는 바꾸지 않는다.
- 별도 특수 fast path를 추가하지 않고 기존 helper의 책임을 재사용한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 영역:
  - ASIO output buffer 준비 경로.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거를 되돌리지 않는다.
  - maxmsgsize, decoder/message/send guard 정책을 수정하지 않는다.
  - mtrie, port parsing, IPC unlink 정책을 수정하지 않는다.
  - perf runner/client/server를 수정하지 않는다.

## 실행 예정

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_multi_stream_server_reassembly|test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)'
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round112_asio_process_output_helper_unification
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
ctest --test-dir core/build --output-on-failure -R 'test_multi_stream_server_reassembly|test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)'
```

- 결과: 20/20 passed.

### STREAM focused perf

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round112_asio_process_output_helper_unification
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_172858_round112_asio_process_output_helper_unification.txt`
- status: complete, fail 0.
- load_avg: `22.36 11.18 8.05`.

| Pattern | Transport | 64B throughput |
|---------|-----------|----------------|
| STREAM | tcp | 312309.8 ops/s |
| STREAM | tls | 220770.6 ops/s |
| STREAM | wss | 178294.0 ops/s |

### Guard perf 1

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round112_pubsub_stream_guard
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_172949_round112_pubsub_stream_guard.txt`
- status: partial, fail 1.
- failure: `MULTI_STREAM current wss 64B: non_zero_exit_2_size_64`.
- 같은 source에서 STREAM-only wss는 직전 focused perf에서 성공했으므로 재현성 확인을 위해 wss 단독과 guard 재실행을 추가했다.

### STREAM wss recheck

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round112_stream_wss_recheck
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_173301_round112_stream_wss_recheck.txt`
- status: complete, fail 0.
- STREAM wss 64B: 183139.2 ops/s.

### Guard perf 2

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,STREAM --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round112_pubsub_stream_guard_rerun
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_173323_round112_pubsub_stream_guard_rerun.txt`
- status: complete, fail 0.
- load_avg: `3.40 6.09 6.67`.

| Pattern | Transport | round110 | round112 guard 2 | Delta |
|---------|-----------|---------:|-----------------:|------:|
| PUBSUB | tcp | 2357759.4 | 2433742.6 | +3.2% |
| PUBSUB | tls | 2153328.0 | 2230630.8 | +3.6% |
| PUBSUB | wss | 2445626.8 | 2470511.8 | +1.0% |
| STREAM | tcp | 252199.0 | 324782.4 | +28.8% |
| STREAM | tls | 184091.4 | 213598.6 | +16.0% |
| STREAM | wss | 157238.8 | 182649.6 | +16.2% |

## 최종 판단

- 채택 후보.
- STREAM tcp 64B는 May26 smoke 기준 325470.0 ops/s와 거의 같은 수준까지 회복했다.
- STREAM tls 64B는 May26 full 기준 214574.6 ops/s에 근접했다.
- STREAM wss 64B는 May26 full 기준 184722.2 ops/s에 근접했다.
- PUBSUB guard에서도 하락 항목이 없었다.
- `process_output()`의 중복 구현을 제거하고 `prepare_output_buffer()`의 단일 정책으로 합쳤으므로 POSD 관점에서도 복잡성이 줄었다.
