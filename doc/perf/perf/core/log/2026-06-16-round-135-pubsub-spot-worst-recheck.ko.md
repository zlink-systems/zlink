# Round 135: PUBSUB and SPOT worst recheck

## 목표

- round134 reduced full에서 worst로 남은 `PUBSUB/tcp,tls`와 `SPOT/wss`가 반복 병목인지 targeted perf로
  다시 확인한다.
- source 변경 전 수치 변동을 분리하고, 하락이 반복되는 항목에만 후보를 적용한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round122 low-load reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`
- round134 current retained SPOT reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_014400_round134_current_retained_spot_reduced_full.txt`

## 시작 상태

- 유지 source diff:
  `zlink_spot_send_spot_part()` 단일 FINAL fast path만 남아 있다.
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.

## 병목 가설

1. `PUBSUB` 64B는 `xpub_t::xsend()`의 send-all path에서 `dist_t::check_hwm()`과 LMSG refcount fanout 비용이
   지배적일 수 있다.
2. `SPOT/wss`는 data-plane fanout 자체보다 WS/WSS transport 변동 또는 spotnode 내부 socket set의 HWM/queue
   정책 영향이 클 수 있다.
3. round134의 `PUBSUB/tcp,tls` 하락은 source 회귀가 아니라 reduced full 긴 실행 창의 변동일 수 있다.

## 먼저 검증할 가설

- `PUBSUB tcp,tls,ws,wss 64B`와 `SPOT wss 64B`를 낮은 부하에서 targeted 재측정한다.
- 재측정에서도 하락이 반복되는 항목만 코드 후보를 적용한다.

## targeted 재측정

### PUBSUB

명령:

```bash
sleep 45; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round135_pubsub_worst_targeted_current
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_021837_round135_pubsub_worst_targeted_current.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.30 1.65 2.41`
- 완료: fail 0

64B throughput:

| transport | round135 | vs May26 full | vs round122 | vs round134 |
|-----------|----------|---------------|-------------|-------------|
| tcp | 2720419.2 | +2.21% | -1.16% | +5.65% |
| tls | 2398485.6 | -8.56% | -1.64% | +0.67% |
| ws | 2227330.0 | +1.18% | +1.37% | +1.92% |
| wss | 2668512.6 | -3.33% | -0.71% | -0.07% |

판단:

- round134의 `PUBSUB/tcp` 하락은 targeted 재측정에서 회복됐다. source 회귀로 보기 어렵다.
- `PUBSUB/tls`는 May26 full 대비 하락이 반복된다. 다만 round122 대비 -1.64%라 현재 변경의 직접 회귀라기보다
  현재 실행 창 또는 TLS 출력 경로 변동까지 섞였을 수 있다.
- `PUBSUB/ws,wss`는 round122 대비 각각 +1.37%, -0.71%로 작은 변동 범위다.

### SPOT/wss

명령:

```bash
sleep 30; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern SPOT --transports wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round135_spot_wss_targeted_current
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_022344_round135_spot_wss_targeted_current.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `2.25 2.33 2.49`
- 완료: fail 0
- 64B throughput: `6100341.2`
- vs May26 full: -9.98%
- vs round122: +0.00%
- vs round134: +2.26%

판단:

- `SPOT/wss`는 May26 full 대비 낮지만 round122와 거의 같다.
- 예전 독립 측정에서 May26 full을 넘긴 적이 있으므로, source 회귀로 단정하지 않는다.
- 이 항목은 바로 수정하기보다 same-window A/B가 가능한 구체 후보가 있을 때만 건드린다.

## 다음 판단

- 먼저 반복 신호가 남은 `PUBSUB/tls`를 기준으로 후보를 고른다.
- 이전에 시도했던 `PUBSUB` gather/HWM 계열은 일부 transport 하락이 있었으므로 그대로 재채택하지 않는다.
- 작은 개선도 하락이 없으면 채택할 수 있지만, POSD 기준상 호출자 계약이나 runner 정책을 늘리는 변경은 제외한다.
