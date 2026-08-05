# Round 134: retained SPOT fast path reduced full

## 목표

- round131에서 채택한 `SPOT_SENDSEND` FINAL fast path만 source diff로 남긴 상태에서 reduced full을 다시 측정한다.
- round132/133에서 재검토한 STREAM 후보들은 모두 원복 상태다.

## 실행

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round134_current_retained_spot_reduced_full`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_014400_round134_current_retained_spot_reduced_full.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,0.94 2.01 2.61`
- status: complete, fail 0

## 요약

- May26 full 대비:
  - count: 32
  - average: `+1.34%`
  - median: `+2.39%`
  - worst: `MULTI_SPOT/wss -11.96%`
  - best: `MULTI_STREAM/ws +8.53%`
- round122 대비:
  - count: 32
  - average: `-0.88%`
  - median: `-0.51%`
  - worst: `MULTI_PUBSUB/tcp -6.45%`
  - best: `MULTI_STREAM/wss +3.25%`

## 핵심 항목

| item | current | May26 full delta | round122 delta |
|---|---:|---:|---:|
| MULTI_STREAM/tcp | 318475.0 | +4.36% | -1.88% |
| MULTI_STREAM/tls | 225391.8 | +5.04% | -1.73% |
| MULTI_STREAM/ws | 272759.4 | +8.53% | -3.18% |
| MULTI_STREAM/wss | 195207.6 | +5.68% | +3.25% |
| MULTI_SPOT_SENDSEND/tcp | 263864.8 | -2.71% | +1.54% |
| MULTI_SPOT_SENDSEND/tls | 246057.0 | -3.13% | -1.55% |
| MULTI_SPOT_SENDSEND/ws | 246330.4 | +2.30% | +2.20% |
| MULTI_SPOT_SENDSEND/wss | 258812.6 | +2.48% | +0.78% |
| MULTI_PUBSUB/tcp | 2574843.6 | -3.26% | -6.45% |
| MULTI_PUBSUB/tls | 2382452.8 | -9.17% | -2.30% |
| MULTI_SPOT/wss | 5965698.0 | -11.96% | -2.20% |

## 판단

- STREAM은 May26 full 대비 모두 플러스지만 tcp는 318kops로 400kops 목표에는 아직 부족하다.
- SPOT_SENDSEND fast path는 focused A/B에서는 하락 없이 플러스였지만, reduced full에서는 tls가
  round122 대비 낮다. 다만 round131 same-window A/B가 직접 근거이므로 현재는 유지한다.
- 새 STREAM 후보를 무리하게 채택하지 않는다. round132 inflight 제거와 round133 tcp tiny gather는 모두
  하락 항목이 있어 원복했다.
- 다음 후보는 `PUBSUB/tcp,tls` 또는 `SPOT/wss`처럼 round134 worst에 남은 항목을 우선 검토한다.
