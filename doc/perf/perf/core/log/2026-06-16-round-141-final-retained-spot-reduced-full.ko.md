# Round 141: final retained SPOT reduced full

## 목표

- TLS gather 후보를 원복하고, 유지 diff를 `zlink_spot_send_spot_part()` 단일 FINAL fast path 하나로
  제한한 상태에서 reduced full 64B를 다시 측정한다.
- 사용자가 정정한 May26 full 기준과 직전 retained 상태인 round134를 함께 비교한다.

## 시작 상태

- 유지 source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`에서 send sequence가 시작되지 않은 단일 `ZLINK_PART_FINAL`을
    `spot_send_spot_impl(..., part_count=1)`로 직접 보낸다.
- TLS gather 후보와 STREAM 후보는 모두 원복 상태다.
- perf runner/client/server는 수정하지 않았다.
- 보안 하드닝 항목은 수정하지 않았다.

## 실행

명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 --connect-ready-timeout-ms 5000 \
  --results-tag round141_final_retained_spot_reduced_full
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034903_round141_final_retained_spot_reduced_full.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.66 0.99 1.74`
- 완료: success 32, fail 0
- `git diff --check -- core/src core/include core/tests doc/plan/perf/core/log/2026-06-16-round-140-stream-tcp-hotpath-rebaseline.ko.md`
  통과

## 요약

May26 full 대비:

- count: 32
- average: `+3.62%`
- median: `+3.34%`
- worst: `MULTI_PUBSUB/tls -11.08%`
- best: `MULTI_STREAM/ws +16.45%`

round134 대비:

- count: 32
- average: `+2.23%`
- median: `+0.60%`
- worst: `MULTI_PUBSUB/tls -2.10%`
- best: `MULTI_SPOT_SENDSEND/tls +11.63%`

## 핵심 항목

| item | current | vs May26 full | vs round134 |
|------|--------:|---------------:|------------:|
| MULTI_STREAM/tcp | 339453.0 | +11.23% | +6.59% |
| MULTI_STREAM/tls | 235238.6 | +9.63% | +4.37% |
| MULTI_STREAM/ws | 292661.2 | +16.45% | +7.30% |
| MULTI_STREAM/wss | 202534.2 | +9.64% | +3.75% |
| MULTI_SPOT_SENDSEND/tcp | 276611.2 | +1.99% | +4.83% |
| MULTI_SPOT_SENDSEND/tls | 274685.4 | +8.14% | +11.63% |
| MULTI_SPOT_SENDSEND/ws | 269240.6 | +11.82% | +9.30% |
| MULTI_SPOT_SENDSEND/wss | 273255.8 | +8.20% | +5.58% |
| MULTI_PUBSUB/tcp | 2536476.6 | -4.70% | -1.49% |
| MULTI_PUBSUB/tls | 2332426.0 | -11.08% | -2.10% |
| MULTI_SPOT/wss | 6138139.0 | -9.42% | +2.89% |

## 판단

- corrected May26 full 기준으로 `STREAM/tcp 64B`는 `339.5kops`이며 `+11.23%`다. 400kops 목표에는
  아직 도달하지 못했다.
- round134 대비로는 STREAM 전송 전체와 SPOT_SENDSEND 전송 전체가 상승했다.
- May26 full 대비 `PUBSUB/tls`, `SPOT/wss`, `PUBSUB/tcp`, `ROUTER_ROUTER/ws`, `PUBSUB/wss`는
  여전히 `-2%`보다 낮다.
- round134 대비 하락은 `PUBSUB/tls -2.10%` 하나이며, round135 targeted 재측정에서 같은 retained 상태의
  `PUBSUB/tls`가 round134 대비 `+0.67%`였기 때문에 source diff의 직접 회귀로 단정하지 않는다.
- 현재 retained SPOT fast path는 작은 개선이라도 하락 없이 반복되는지 보는 기준에서는 유지 가능하다.
  다만 전체 perf 목표는 아직 완료가 아니다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 유지 diff는 SPOT send-part fast path 내부의 submit 경로 단축뿐이다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서, decoder/message/send guard,
    `maxmsgsize` 정책을 변경하지 않았다.
  - STREAM/TLS gather 후보는 guard가 부족해 원복했다.
- 추가로 실행한 회귀 테스트:
  - `cmake --build core/build --target libzlink -j$(nproc)`
  - reduced full perf success 32, fail 0
