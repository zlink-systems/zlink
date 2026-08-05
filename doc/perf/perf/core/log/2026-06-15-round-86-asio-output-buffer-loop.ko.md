# Round 86: ASIO output buffer loop 중복 제거 후보

## 목표

ASIO output hot path에서 새 상태를 추가하지 않고 중복 구현을 줄일 수 있는지 확인한다.
완료 기준은 focused ASIO/STREAM/PUBSUB 테스트 통과와 targeted 64B perf에서 하락이 없는지 확인하는 것이다.

## 기준 report

- May26 full 보정 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 최신 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
- 최근 재검토:
  - round84 native send 후보: 미채택
  - round85 STREAM current pipe 후보: 미채택

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음
- 기존 로그 변경:
  - `doc/plan/perf/core/log/2026-06-15-round-81-pubsub-tls-commit-isolation.ko.md`
  - round82~round85 로그 파일

## 병목 가설

1. `asio_engine_t::process_output()`와 `prepare_output_buffer()`가 같은 encoder fill loop를
   별도로 유지한다. 중복 구현을 하나로 합치면 hot path instruction/cache footprint가 아주 작게 줄고,
   이후 batching 정책 변경도 한 곳에서만 다루게 된다.
2. 실제 병목은 encoder fill loop 중복이 아니라 pipe wakeup, TLS write completion, 또는 perf sequence
   variance다. 이 경우 구조는 단순해져도 측정 개선은 없거나 잡음권이다.

먼저 가설 1을 검증한다. 변경은 `process_output()`이 이미 존재하는 `prepare_output_buffer()`를 호출하게
하는 최소 refactor로 제한한다.

## POSD 점검

- 새 상태, 새 캐시, 새 public API를 추가하지 않는다.
- 같은 의미의 encoder fill loop를 한 구현으로 모아 정보 중복을 줄인다.
- ASIO transport 정책, 보안 guard, perf 조건은 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 사본 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
    decoder/message/send guard, maxmsgsize 정책을 변경하지 않는다.
  - ASIO encoder output fill 구현의 중복만 줄인다.
- 추가로 실행한 회귀 테스트:
  - `ctest --test-dir core/build --output-on-failure -R 'test_stream|test_pubsub|test_pubsub_filter_xpub|test_xpub_nodrop|test_transport_matrix|test_multi_socket_contract_regressions|test_zmp_request_reply'`

## 변경

- 파일: `core/src/runtime/engine/asio/asio_engine.cpp`
- 내용:
  - `process_output()` 안에 복제되어 있던 encoder fill loop를 제거하고,
    기존 `prepare_output_buffer()`를 호출하도록 임시 변경했다.

## 검증

### build

```bash
cmake --build core/build -j$(nproc)
```

- 결과: 통과

### test

```bash
ctest --test-dir core/build --output-on-failure -R 'test_stream|test_pubsub|test_pubsub_filter_xpub|test_xpub_nodrop|test_transport_matrix|test_multi_socket_contract_regressions|test_zmp_request_reply'
```

- 결과: 26/26 통과

```bash
git diff --check
```

- 결과: 통과

### perf

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,STREAM --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round86_asio_output_buffer_loop_pubsub_stream
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_143823_round86_asio_output_buffer_loop_pubsub_stream.txt`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- load_avg: `13.92 12.38 10.90`
- 결과:

| case | round70 current | round86 candidate | delta |
|------|-----------------|-------------------|-------|
| PUBSUB/tcp/64B | 2,468,643.4 | 2,405,328.6 | -2.57% |
| PUBSUB/tls/64B | 2,264,552.0 | 2,279,190.4 | +0.65% |
| STREAM/tcp/64B | 332,250.4 | 331,664.2 | -0.18% |
| STREAM/tls/64B | 217,262.2 | 213,001.6 | -1.96% |

## 판정

- focused tests는 통과했다.
- 그러나 성능은 `PUBSUB/tls`만 +0.65%이고, `PUBSUB/tcp`, `STREAM/tcp`, `STREAM/tls`는 모두 낮다.
- 구조 단순화 자체는 POSD 방향과 맞지만, 현재 목표는 core runtime 성능 개선이므로 이 변경을 남기지 않는다.
- source 변경은 원복했다.

## 현재 상태

- source diff 없음.
- 다음 후보는 ASIO 중복 제거보다 실제 one-way data path 비용을 줄일 수 있는 쪽에서 다시 찾는다.
