# Round 156: retained 상태 64B full refresh

## 범위

- 목적: 현재 retained source 상태가 May26 smoke/full 기준과 어느 정도 맞는지
  전체 64B 축으로 확인한다.
- 제외: perf runner, perf client/server, benchmark 조건 변경.
- 현재 retained source diff:
  - `zlink_spot_send_spot_part()` 단일 FINAL fast path.

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round156_retained_spot_final_fastpath_full64_refresh
```

- runtime: `core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064431_round156_retained_spot_final_fastpath_full64_refresh.txt`
- status: success `32`, fail `0`
- elapsed: `43m 21s`

## May26 full 대비 주요 결과

- STREAM:
  - `MULTI_STREAM/tcp/64`: `305,177.4 -> 345,614.8`, `+13.25%`
  - `MULTI_STREAM/tls/64`: `214,574.6 -> 232,880.8`, `+8.53%`
  - `MULTI_STREAM/ws/64`: `251,311.4 -> 304,258.6`, `+21.07%`
  - `MULTI_STREAM/wss/64`: `184,722.2 -> 191,627.6`, `+3.74%`
- SPOT_SENDSEND:
  - `tcp`: `271,206.0 -> 267,789.4`, `-1.26%`
  - `tls`: `254,009.6 -> 275,596.4`, `+8.50%`
  - `ws`: `240,791.0 -> 263,326.4`, `+9.36%`
  - `wss`: `252,557.8 -> 272,499.0`, `+7.90%`
- worst full 대비 하락:
  - `MULTI_SPOT/wss/64`: `-9.32%`
  - `MULTI_PUBSUB/tls/64`: `-7.81%`
  - `MULTI_PUBSUB/wss/64`: `-3.14%`
  - `MULTI_SPOT_SENDSEND/tcp/64`: `-1.26%`

## May26 smoke 대비 주요 하락

- `MULTI_SPOT/tcp/64`: `-13.88%`
- `MULTI_PUBSUB/tls/64`: `-4.71%`
- `MULTI_STREAM/wss/64`: `-4.49%`
- `MULTI_SPOT_SENDSEND/wss/64`: `-1.70%`

## 판정

- 사용자 기준이 된 STREAM/tcp 64B는 May26 smoke/full보다 높다.
- 그러나 "하락 항목 없이 +면 채택"이라는 보수 기준으로 보면 full 기준 하락 항목이 남아 있다.
- retained SPOT_SENDSEND fast path 자체는 round131 A/B에서 제거본 대비 모든 SPOT_SENDSEND transport가
  상승했으므로 후보 자체의 직접 하락으로 단정하지 않는다.
- 다음 단계:
  - May26 full 대비 하락한 `MULTI_SPOT/wss`, `MULTI_PUBSUB/tls`, `MULTI_PUBSUB/wss`는
    retained 변경과 독립적인 현재 편차인지 별도 focused 재측정한다.
  - 새 source 후보는 하락 항목을 만들지 않는 경우에만 유지한다.
