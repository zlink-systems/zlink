# Round 100: STREAM transition stability recheck

## goal

full/reduced multi perf 실패 0개 목표를 위해, round97에서 나온 `STREAM/tls,wss` 실패가 반복되는지
확인한다.

완료 기준:

- `core/build` runtime 사용 확인
- reduced 64B set에서 실패 0개 또는 실패 조건 분리
- source 변경이 필요하면 core runtime 쪽에서 원인을 확인한 뒤 최소 변경
- source 변경이 없으면 이 round는 안정성 재현 로그로만 남긴다.

## 기준

- round97 reduced partial:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153913_round97_retained_spot_fastpath_reduced_guard.txt`
- round98 STREAM-only complete:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154759_round98_stream_transport_transition_failure_recheck.txt`

## git 상태

- retained core diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
- unrelated dirty files:
  - framework docs/node sample 변경이 worktree에 섞여 있으나 이 round에서 건드리지 않는다.

## 병목/실패 가설

1. round97 실패는 STREAM 자체의 `tcp -> tls -> wss` 전환 문제가 아니라, 앞선 PUBSUB/SPOT pattern 이후
   자원 상태와 결합된 실패다.
2. 실패가 재현되지 않으면 STREAM 실패는 현 시점에서 불안정한 외부/순서 문제로 보고, 다음 성능 후보는
   공통 hot path로 넘긴다.
3. 실패가 재현되면 perf runner를 수정하지 않고 core transport teardown, timer/callback, pending write/read
   상태를 우선 확인한다.

먼저 검증할 가설:

- round97과 같은 reduced set을 다시 실행해 실패가 반복되는지 본다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 없음. 먼저 재현만 수행한다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending-copy 제거, mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.

## reduced 재실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT_SENDSEND,SPOT_REQREP,STREAM \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round100_reduced_stream_transition_stability
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`
- status: complete
- success: 12
- fail: 0
- start load_avg: `5.92 11.58 9.53`

| case | round90 current | round100 | delta |
|---|---:|---:|---:|
| PUBSUB/tcp/64B | 2,548,363.4 | 2,368,520.0 | -7.06% |
| PUBSUB/tls/64B | 2,278,477.4 | 2,216,652.6 | -2.71% |
| PUBSUB/wss/64B | 2,494,252.0 | 2,465,419.0 | -1.16% |
| SPOT_SENDSEND/tcp/64B | 245,644.8 | 253,359.0 | +3.14% |
| SPOT_SENDSEND/tls/64B | 233,617.4 | 249,998.2 | +7.01% |
| SPOT_SENDSEND/wss/64B | 248,696.6 | 244,308.4 | -1.76% |
| STREAM/tcp/64B | 320,253.2 | 280,166.0 | -12.52% |
| STREAM/tls/64B | 219,801.0 | 209,046.0 | -4.89% |
| STREAM/wss/64B | 190,692.6 | 175,863.2 | -7.78% |

`SPOT_REQREP`은 round90에 같은 set이 없어 May26 full 기준으로 본다.

| case | May26 full | round100 | delta |
|---|---:|---:|---:|
| SPOT_REQREP/tcp/64B | 252,212.6 | 252,806.8 | +0.24% |
| SPOT_REQREP/tls/64B | 229,720.4 | 223,681.2 | -2.63% |
| SPOT_REQREP/wss/64B | 219,301.2 | 214,421.2 | -2.22% |

## 판정

- round97의 `STREAM/tls,wss` failure는 이번 reduced 재실행에서는 반복되지 않았다.
- full/reduced 실패 0개 목표는 이 reduced 범위에서는 충족했지만, full multi 전체로는 아직 증명되지 않았다.
- 성능은 여전히 미달이다.
  - `STREAM/tcp`는 round90 대비 `-12.52%`, 목표 400Kops와도 거리가 크다.
  - `PUBSUB/tcp`와 `PUBSUB/tls`도 낮다.
  - retained round92 변경은 `SPOT_SENDSEND/tls`에서 반복 개선을 보이지만, `wss`는 흔들린다.
- 이 round는 source 변경 없이 안정성 재현만 수행했다.

## 다음

- 실패가 즉시 재현되지 않았으므로 다음 라운드는 성능 병목으로 돌아간다.
- 우선순위는 `STREAM/tcp`와 `PUBSUB/tcp/tls`다.
- 작은 코드 후보는 하락 없는 반복 개선을 보여야 하며, STREAM 후보는 `tcp,tls,wss` 순서 guard를 필수로 둔다.
