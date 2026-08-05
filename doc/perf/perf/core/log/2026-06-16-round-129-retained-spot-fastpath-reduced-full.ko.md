# Round 129: retained SPOT fast path reduced full

## 목표

- round125에서 유지 후보로 남긴 `zlink_spot_send_spot_part()` FINAL-only fast path 상태를 reduced full 64B로 다시 확인한다.
- STREAM/tcp 64B 목표와 전체 하락 항목을 May26 기준, round122, round125와 비교한다.

## 시작 상태

- source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`의 단일 FINAL part fast path.
- round127 STREAM routing-id decode-once 최소 후보는 반려 후 원복했다.
- round128 ASIO target cap / auto-HWM throughput profile env probe는 source 변경 없이 반려했다.
- `git diff --check`: pass

## 실행

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round129_retained_spot_fastpath_lowload_all64_reduced_full`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_003229_round129_retained_spot_fastpath_lowload_all64_reduced_full.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,1.21 3.55 4.85`
- status:
  complete, fail 0

## 주요 결과

### STREAM

| transport | May26 full | May26 smoke | round122 | round125 | round129 |
|-----------|-----------:|------------:|---------:|---------:|---------:|
| tcp | 305177.4 | 325470.0 | 324585.4 | 324716.0 | 303030.8 |
| tls | 214574.6 | 229781.0 | 229366.2 | 219617.6 | 220377.2 |
| ws | 251311.4 | 263180.0 | 281722.8 | 251775.0 | 251128.2 |
| wss | 184722.2 | 200642.0 | 189070.4 | 160400.6 | 198625.6 |

- May26 full 대비:
  - tcp `-0.70%`
  - tls `+2.70%`
  - ws `-0.07%`
  - wss `+7.53%`
- round122 대비:
  - tcp `-6.64%`
  - tls `-3.92%`
  - ws `-10.86%`
  - wss `+5.05%`
- STREAM/tcp 64B는 303.0kops로, 400kops 목표에는 아직 도달하지 못했다.

### SPOT_SENDSEND

| transport | May26 full | May26 smoke | round122 | round125 | round129 |
|-----------|-----------:|------------:|---------:|---------:|---------:|
| tcp | 271206.0 | 264042.0 | 259856.6 | 261465.2 | 264909.2 |
| tls | 254009.6 | 247003.0 | 249922.4 | 248891.8 | 248411.2 |
| ws | 240791.0 | 242507.0 | 241035.8 | 242320.8 | 236483.0 |
| wss | 252557.8 | 277203.0 | 256813.8 | 252525.0 | 243380.8 |

- round122 대비:
  - tcp `+1.94%`
  - tls `-0.60%`
  - ws `-1.89%`
  - wss `-5.23%`
- round125 same-window A/B에서는 fast path가 제거 상태보다 좋아 보였지만,
  reduced full에서는 SPOT_SENDSEND 전체가 명확한 개선으로 고정되지 않았다.

## 전체 비교

### vs May26 full

- common: 32
- average: `+1.31%`
- median: `+1.91%`
- worst:
  - `MULTI_SPOT/wss`: `-9.37%`
  - `MULTI_PUBSUB/tls`: `-9.30%`
  - `MULTI_SPOT_SENDSEND/wss`: `-3.63%`
  - `MULTI_PUBSUB/wss`: `-3.02%`
  - `MULTI_PUBSUB/ws`: `-2.52%`
- best:
  - `MULTI_SPOT_REQREP/tcp`: `+8.94%`
  - `MULTI_STREAM/wss`: `+7.53%`
  - `MULTI_DEALER_ROUTER/ws`: `+7.18%`
  - `MULTI_SPOT/ws`: `+5.86%`
  - `MULTI_DEALER_DEALER/tls`: `+5.14%`

### vs round122

- common: 32
- average: `-0.87%`
- median: `-0.48%`
- worst:
  - `MULTI_STREAM/ws`: `-10.86%`
  - `MULTI_STREAM/tcp`: `-6.64%`
  - `MULTI_SPOT_SENDSEND/wss`: `-5.23%`
  - `MULTI_STREAM/tls`: `-3.92%`
  - `MULTI_PUBSUB/tcp`: `-2.76%`
- best:
  - `MULTI_STREAM/wss`: `+5.05%`
  - `MULTI_SPOT_REQREP/tcp`: `+4.69%`
  - `MULTI_SPOT_SENDSEND/tcp`: `+1.94%`

### vs round125

- common: 32
- average: `+0.70%`
- median: `-0.07%`
- worst:
  - `MULTI_STREAM/tcp`: `-6.68%`
  - `MULTI_ROUTER_ROUTER/wss`: `-5.68%`
  - `MULTI_ROUTER_ROUTER/tls`: `-3.77%`
  - `MULTI_SPOT_SENDSEND/wss`: `-3.62%`
- best:
  - `MULTI_STREAM/wss`: `+23.83%`
  - `MULTI_PUBSUB/tcp`: `+7.54%`
  - `MULTI_SPOT/ws`: `+6.55%`
  - `MULTI_SPOT_REQREP/ws`: `+5.98%`

## 판단

- fail 0은 만족했다.
- May26 full 기준 전체 평균/중앙값은 양수지만, 사용자가 말한 STREAM/tcp 400kops 목표에는 도달하지 못했다.
- round122 대비로는 STREAM/ws가 `-10%`를 넘게 낮고, STREAM/tcp도 낮다.
- round125 SPOT FINAL-only fast path는 같은 창 A/B에서는 긍정적이었지만 reduced full에서는 효과가 작고 변동성에 묻힌다.
- 따라서 현재 상태를 최종 성공으로 보지 않는다. 다음 후보는 STREAM/tcp/ws 또는 PUBSUB/tls/ws를 하락 없이 끌어올릴 수 있는 core-only 변경이어야 한다.
