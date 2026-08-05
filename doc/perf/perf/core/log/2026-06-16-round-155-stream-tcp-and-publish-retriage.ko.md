# Round 155: STREAM/tcp 기준 재확인과 publish fast path 재검토

## 범위

- 대상: core runtime hot path.
- 제외: perf runner, perf client/server, benchmark 조건 변경.
- 현재 유지 중인 source diff: `zlink_spot_send_spot_part()` 단일 FINAL fast path.

## 사용자 기준

- May26 smoke 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `MULTI_STREAM/tcp/64/throughput`: `325,470.0 ops/s`
- May26 full 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - `MULTI_STREAM/tcp/64/throughput`: `305,177.4 ops/s`
- 판정 정책:
  - 5% 이상이면 명확한 개선으로 본다.
  - 1-2% 개선도 하락 항목이 없고 POSD-safe이면 누적 후보로 채택 가능하다.
  - 하락 항목이 있으면 원복 또는 추가 A/B 확인을 우선한다.

## 현재 재측정

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round155_stream_tcp_current_refresh
```

- runtime: `core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064056_round155_stream_tcp_current_refresh.txt`
- load_avg: `0.30 1.19 1.89`
- result:
  - `MULTI_STREAM/tcp/64/throughput`: `326,131.6 ops/s`
- May26 smoke 대비: `+0.20%`
- May26 full 대비: `+6.87%`

## publish fast path 재검토

- 확인한 과거 후보:
  - round 9: `zlink_publish_part()` single FINAL fast path는 tcp만 소폭 상승,
    tls/ws/wss 하락으로 원복.
  - round 28: `MULTI_PUBSUB`는 전반 상승했지만 `MULTI_SPOT/tcp`와
    `MULTI_SPOT/ws`가 크게 하락해 원복.
  - round 42: round29 current 대비 tcp/ws/wss 하락, tls만 `+0.86%`.
  - round 75: tls 단독 후보도 약 `-0.25%`.
- POSD 판단:
  - 단순히 `zlink_publish_part()`에서 `logical_multipart_publish()`로 위임하는
    변경은 이미 반복 검증에서 하락 항목이 있었다.
  - topic 프레임 전송 성공 후 payload 전송 실패가 발생하면 rollback과 caller
    part 소유권 처리가 정확히 한 번만 일어나야 한다. 이 지식을 API 계층에 다시
    펼치면 publish transaction의 실패 의미가 두 곳으로 새어 나온다.
- 결론:
  - 이번 라운드에서는 publish fast path를 다시 적용하지 않는다.
  - 다음 후보는 STREAM/tcp 64B의 실제 dispatch/send 경로에서, 기존 보안 하드닝과
    current-pipe 실패 이력을 건드리지 않는 작은 변경으로 제한한다.

## 후보 A: STREAM dispatch context 조회 결합

- 변경 방향:
  - `stream_dispatch_context_t::owns_socket()`와 `current_routing_id()`를 API
    송신 경로에서 따로 조회하지 않고, 내부 helper 하나에서 현재 socket과
    routing id를 함께 확인한다.
  - public API 변경 없음.
  - stream dispatch TLS 지식은 `stream_dispatch_internal`에 머물게 하므로,
    API 계층으로 새 상태나 정책을 노출하지 않는다.
- build:
  - `cmake --build core/build --target libzlink -j$(nproc)` 통과.
- tests:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(stream...|multi_stream...)'`
  - STREAM 관련 20/20 통과.
  - transport/multipart 주변 3/3 통과.
- candidate perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round155_stream_dispatch_context_combined_candidate
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064319_round155_stream_dispatch_context_combined_candidate.txt`
- result:
  - `MULTI_STREAM/tcp/64/throughput`: `338,614.2 ops/s`
- current refresh 대비: `+3.83%`

## 후보 A 제거 A/B

```bash
cmake --build core/build --target libzlink -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round155_stream_dispatch_context_combined_removed_ab
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064351_round155_stream_dispatch_context_combined_removed_ab.txt`
- result:
  - `MULTI_STREAM/tcp/64/throughput`: `345,241.8 ops/s`
- candidate 대비 제거본이 `+1.96%` 높다.

## 판정

- 후보 A는 하락 없는 개선으로 확인되지 않았다.
- 소스 변경은 제거 상태로 유지한다.
- 현재 `STREAM/tcp/64B`는 May26 smoke/full 기준과 같거나 높지만, 400kops
  목표에는 아직 부족하다.
