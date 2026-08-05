# Round 142: PUBSUB tls recheck

## 목표

- round141 reduced full에서 남은 최저 항목인 `MULTI_PUBSUB/tls 64B`를 source 변경 전 다시 확인한다.
- 긴 reduced full의 run-order/load 변동과 core hot path 회귀를 분리한다.
- 완료 기준:
  - source 변경 전 targeted `PUBSUB tls 64B` 재측정으로 반복 하락 여부를 확인한다.
  - 반복 하락이 확인될 때만 core-only 최소 후보를 적용한다.
  - 후보가 `PUBSUB tcp,tls,ws,wss`에서 하락 없이 개선될 때만 유지한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round134 retained SPOT reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_014400_round134_current_retained_spot_reduced_full.txt`
- round135 PUBSUB targeted current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_021837_round135_pubsub_worst_targeted_current.txt`
- round141 final retained SPOT reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034903_round141_final_retained_spot_reduced_full.txt`

## 시작 상태

- 유지 source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.

## 병목 가설

1. `PUBSUB/tls 64B` 하락은 `xpub_t::xsend()`의 matching send-all 경로보다 TLS transport 출력 경로의
   긴 실행 순서 변동일 수 있다.
2. `dist_t::check_hwm()`은 matching pipe 전체를 매번 훑는다. 단일 subscriber 또는 모든 pipe가 active인
   steady-state에서 이 비용이 64B one-way에 보일 수 있다.
3. `dist_t::distribute()`의 multi-pipe fanout은 VSM refcount/copy와 pipe write/flush 비용이 지배적일 수
   있다. 다만 기존 단일 pipe fast path나 dist write-more cache 후보는 하락 항목이 있어 재채택하지 않는다.

## 먼저 검증할 가설

- source 변경 없이 `PUBSUB tls 64B` targeted를 낮은 부하에서 다시 실행한다.
- round141의 `PUBSUB/tls -2.10% vs round134`가 반복되지 않으면 source 변경을 하지 않는다.

## source 변경 전 targeted 측정

명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB --transports tls \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round142_pubsub_tls_targeted_current
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_042452_round142_pubsub_tls_targeted_current.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.59 1.66 2.32`
- 완료: success 1, fail 0
- `MULTI_PUBSUB/tls 64B`: `2414972.4`

비교:

| 기준 | delta |
|------|------:|
| May26 full | -7.93% |
| round134 | +1.36% |
| round135 targeted | +0.69% |
| round141 | +3.54% |

판단:

- round141 reduced full의 `PUBSUB/tls -2.10% vs round134`는 targeted 재측정에서 반복되지 않았다.
- May26 full 대비 gap은 아직 남지만, 현재 retained source diff의 직접 회귀로 단정할 근거는 약하다.
- 이 결과만으로 `PUBSUB/tls`에 새 source 변경을 적용하지 않는다.

## 코드 triage

- `xpub_t::xsend()`:
  - `_send_all_data && !_manual && !options.invert_matching`이면 `_dist.check_hwm()` 뒤
    `_dist.send_to_all()`로 간다.
  - 일반 matching 경로는 `_subscriptions.match()` 뒤 `_dist.send_to_matching()`으로 간다.
- `dist_t`:
  - 단일 matching pipe steady-state fast path가 이미 있다.
  - VSM 분기와 LMSG refcount fanout 분기가 이미 나뉘어 있다.
  - matching pipe HWM cache가 이미 있다.
- `pipe_t`:
  - `write_and_flush_no_recursive_hwm_check()`와
    `write_single_message_and_flush_no_recursive_hwm_check()` 모두 내부 HWM 확인을 유지한다.

재검토한 후보:

- `dist_t`/`lb_t` final single-message helper 확장은 round63에서 평균 `+0.9%`, 중앙값 `+0.4%`라 원복됐다.
- PUBSUB empty-subscription active pipe 상태는 round67 재분류에서 `ws -5.19%` 하락 때문에 배제됐다.
- XPUB repeated-topic matching cache는 topic/topology cache invalidation 상태를 추가하지만 full/current 기준 개선이
  명확하지 않아 원복됐다.
- pre-HWM 검사를 제거하는 변경은 실패 시 메시지를 소비하지 않는 기존 의미를 바꿀 수 있어 적용하지 않는다.

현재 판단:

- `PUBSUB/tls` 단독 targeted에서는 하락이 반복되지 않았으므로, 이 항목만 보고 새 상태나 예외 fast path를
  추가하지 않는다.
- 다음은 one-way 전체 targeted를 다시 측정해, 문제 report 대비 목표 미달이 reduced full 순서 문제인지
  실제 공통 one-way gap인지 분리한다.

## one-way targeted 재측정

명령:

```bash
sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,PUBSUB,SPOT \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 --connect-ready-timeout-ms 5000 \
  --results-tag round142_oneway_targeted_current
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_042836_round142_oneway_targeted_current.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `1.11 1.42 2.08`
- 완료: success 12, fail 0

문제 report 대비:

- count: 10
- average: `+6.46%`
- median: `+1.07%`
- worst: `MULTI_PUBSUB/tls -2.23%`
- best: `MULTI_SPOT/tls +60.75%`

May26 full 대비:

- count: 12
- average: `-0.40%`
- median: `+1.69%`
- worst: `MULTI_SPOT/wss -11.11%`
- best: `MULTI_DEALER_DEALER/tls +4.27%`

round141 대비:

- count: 12
- average: `-0.02%`
- median: `-0.02%`
- worst: `MULTI_SPOT/ws -3.43%`
- best: `MULTI_PUBSUB/ws +3.22%`

핵심 항목:

| item | current | vs problem | vs May26 full | vs round141 |
|------|--------:|-----------:|--------------:|------------:|
| MULTI_DEALER_DEALER/tcp | 3080658.8 | +1.15% | +3.38% | +0.02% |
| MULTI_DEALER_DEALER/tls | 3194117.2 | +0.99% | +4.27% | -0.98% |
| MULTI_DEALER_DEALER/ws | 3176448.2 | +0.62% | +3.12% | +0.35% |
| MULTI_DEALER_DEALER/wss | 3333643.2 | +2.09% | +3.61% | +0.28% |
| MULTI_PUBSUB/tcp | 2582328.0 | -1.74% | -2.98% | +1.81% |
| MULTI_PUBSUB/tls | 2392173.4 | -2.23% | -8.80% | +2.56% |
| MULTI_PUBSUB/ws | 2249374.4 | +1.31% | +2.18% | +3.22% |
| MULTI_PUBSUB/wss | 2663428.8 | -0.61% | -3.52% | -1.17% |
| MULTI_SPOT/tcp | 3983980.0 | +2.26% | +0.55% | -0.92% |
| MULTI_SPOT/tls | 6010403.8 | +60.75% | +1.19% | -0.06% |
| MULTI_SPOT/ws | 5981994.2 | n/a | +3.34% | -3.43% |
| MULTI_SPOT/wss | 6023719.8 | n/a | -11.11% | -1.86% |

판단:

- one-way targeted도 round141과 거의 같은 수준이다.
- 문제 report 대비 one-way 평균 `+10%`, 중앙값 `+10%` 목표에는 아직 미달이다.
- `SPOT/tls` outlier를 제외하면 one-way 개선은 대부분 `-2%~+4%` 범위다.
- 다음 후보는 PUBSUB 단독 matching 상태 추가가 아니라, `DEALER_DEALER`와 `PUBSUB`가 공유하는
  pipe enqueue/flush, mailbox/wakeup, poller 경로에서 찾아야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 시작 단계는 측정과 코드 triage만 수행한다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서, decoder/message/send guard,
    `maxmsgsize` 정책을 변경하지 않는다.
