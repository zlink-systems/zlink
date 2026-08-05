# Round 98: STREAM transport transition failure recheck

## goal

round97 reduced guard에서 `STREAM/tls`와 `STREAM/wss`가 실패했다. 새 성능 변경을 넣기 전에,
실패가 STREAM 자체의 tcp -> tls -> wss transport transition에서도 재현되는지 확인한다.

## 기준

- round97 reduced guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153913_round97_retained_spot_fastpath_reduced_guard.txt`
- round97 STREAM/tls standalone:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154648_round97_stream_tls_failure_repro.txt`

## 병목/실패 가설

1. reduced guard의 실패는 앞선 PUBSUB/SPOT pattern 이후 STREAM transport가 시작될 때 생기는 자원 또는
   transition 문제다.
2. STREAM 자체의 tcp -> tls -> wss 순서만으로도 실패하거나 큰 성능 하락이 재현될 수 있다.

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round98_stream_transport_transition_failure_recheck
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154759_round98_stream_transport_transition_failure_recheck.txt`
- status: complete
- success: 3
- fail: 0
- start load_avg: `1.32 2.93 5.78`

| case | result | note |
|---|---:|---|
| STREAM/tcp/64B | 304,673.8 | May26 full보다 높지만 400Kops 미달 |
| STREAM/tls/64B | 185,889.0 | standalone `219,094.8`보다 낮음 |
| STREAM/wss/64B | 177,304.6 | May26 full `184,722.2`보다 낮음 |

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 이 round는 source 변경 없이 실패 재현만 수행했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending copy 제거, mtrie 비재귀화, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 건드리지 않았다.
- 추가로 실행한 회귀 테스트:
  - source 변경 없음.

## 판정

- STREAM-only tcp/tls/wss 순서에서는 실패가 재현되지 않았다.
- 하지만 `STREAM/tls`는 standalone보다 크게 낮고, `STREAM/wss`도 May26 full보다 낮아 transport 순서와
  run 상태에 민감하다.
- 이 상태에서 STREAM 관련 작은 후보를 채택하려면 tcp 단독뿐 아니라 tls/wss 순서 run까지 반복 확인해야
  한다.
- 실패 0개 목표는 아직 full/reduced 전체에서 검증되지 않았다.

## 다음

- STREAM 실패는 즉시 재현되지 않았으므로 다음 성능 후보는 더 큰 남은 하락인 `PUBSUB/tls`와 공통
  one-way path에서 찾는다.
- 단, STREAM 후보를 다시 만질 때는 `tcp,tls,wss` 순서 run을 필수 guard로 둔다.
