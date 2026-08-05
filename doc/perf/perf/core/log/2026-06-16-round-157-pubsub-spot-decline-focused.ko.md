# Round 157: PUBSUB/SPOT 하락 항목 focused 재측정

## 목적

- round156 full64 refresh에서 May26 full 대비 낮게 나온 항목이 일회성인지 확인한다.
- 새 source 변경 없이 현재 retained 상태를 그대로 재측정한다.

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB,SPOT --transports tls,wss \
  --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round157_pubsub_spot_decline_focused_refresh
```

- runtime: `core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_072907_round157_pubsub_spot_decline_focused_refresh.txt`
- status: success `4`, fail `0`

## May26 full 대비

- `MULTI_PUBSUB/tls/64`: `2,623,065.0 -> 2,393,807.2`, `-8.74%`
- `MULTI_PUBSUB/wss/64`: `2,760,571.0 -> 2,680,231.0`, `-2.91%`
- `MULTI_SPOT/tls/64`: `5,939,903.4 -> 6,055,830.8`, `+1.95%`
- `MULTI_SPOT/wss/64`: `6,776,300.6 -> 6,100,255.0`, `-9.98%`

## 판정

- `PUBSUB/tls`와 `SPOT/wss` 하락은 focused 재측정에서도 반복됐다.
- `PUBSUB/wss`는 작은 하락이지만 May26 full 대비 낮은 상태가 유지됐다.
- `SPOT/tls`는 focused 재측정에서 상승하므로 문제 항목에서 제외한다.
- retained `zlink_spot_send_spot_part()` fast path의 직접 대상은 `SPOT_SENDSEND`이고,
  이 focused 항목들과 직접 경로가 다르다. 따라서 이 하락을 retained fast path의
  직접 회귀로 단정하지 않는다.

## 다음 후보

- `PUBSUB/tls,wss`: publish helper fast path는 과거 round9/42/75에서 하락 또는 무효로
  원복됐으므로 반복하지 않는다.
- `SPOT/wss`: fanout reserve 후보는 round154에서 SPOT 전 transport 하락으로 원복했다.
- 다음 조사는 WS/WSS 또는 TLS transport 공통 경로 중 이미 보안 하드닝으로 고정된
  pending-copy removal, maxmsgsize, send guard를 건드리지 않는 범위에서만 진행한다.
