# Round 164: SPOT routed destination peek

## goal

- SPOT routed remote send hot path에서 packed envelope full parse를 줄인다.
- 완료 기준:
  - `SPOT_SENDSEND`/`SPOT_REQREP` focused 64B targeted perf에서 하락 항목 없이 의미 있는 개선 후보인지 확인한다.
  - `cmake --build core/build -j$(nproc)` 통과.
  - 관련 core tests 통과.
  - 효과가 없으면 변경을 되돌린다.

## 기준 report

- 현재 문제:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260616_081700_final_retained_spot_multi_full_baseline_20260616.txt`

## 시작 상태

- 현재 HEAD: `aaf17b588`
- core source diff: 없음.
- perf runner/client/server diff: 없음.
- `STREAM ws/wss` full failure는 round 163에서 TIME_WAIT/ephemeral port 압력으로 분리했다.

## 현재 64B 하락 항목

문제 report 대비 retained full에서 낮은 항목:

| item | delta |
|------|------:|
| `MULTI_STREAM/tcp` | -19.84% |
| `MULTI_SPOT_SENDSEND/tcp` | -14.63% |
| `MULTI_PUBSUB/ws` | -14.59% |
| `MULTI_SPOT_SENDSEND/tls` | -14.52% |
| `MULTI_SPOT_REQREP/tls` | -14.13% |
| `MULTI_STREAM/tls` | -10.87% |
| `MULTI_PUBSUB/tls` | -9.70% |
| `MULTI_PUBSUB/tcp` | -6.82% |

이번 라운드는 stream failure와 PUBSUB fanout을 피하고 SPOT echo 계열 공통 send path를 본다.

## 병목 가설

1. `spot_send_spot_impl()`과 request/reply submit path는 destination node rid를 이미 알고 있는데,
   remote dispatch에서 packed envelope를 다시 full parse해 source/destination 문자열을 모두 만든다.
   64B에서는 이 문자열/parse 비용이 payload보다 상대적으로 크다.
2. `dispatch_external_router_delivery()`는 remote routing에 destination node rid만 있으면 충분하다.
   packed header에서 destination node rid만 검증해서 바로 `send_external_router_once()`로 보내면,
   public contract와 wire format을 유지하면서 hot path parse 비용을 줄일 수 있다.

## 먼저 검증할 가설

- packed envelope helper에 destination node rid만 읽는 내부 함수를 추가한다.
- `dispatch_external_router_delivery()`가 이 fast peek에 성공하면 full envelope parse 없이
  external router로 보낸다.
- 실패하거나 empty destination이면 기존 full parse 경로로 fallback한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 없음.
- 보안 의미를 유지한 근거:
  - packed header size 검증과 routing id 크기 검증을 유지한다.
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱 검증,
    IPC unlink 순서, decoder/message/send guard, `maxmsgsize` 정책은 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - 아직 없음.
