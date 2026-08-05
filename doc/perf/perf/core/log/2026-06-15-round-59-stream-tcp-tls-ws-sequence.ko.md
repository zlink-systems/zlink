# Round 59 - STREAM tcp/tls/ws sequence isolation

## 기준

- 목표 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 핵심 stream 목표:
  `MULTI_STREAM/tcp/64B = 400,124.6 ops/s`
- failure gate:
  full multi failure 0을 먼저 만족해야 한다.

## 시작 상태

- core/perf source 변경 없음.
- round57 full `64B,256B` run은 `STREAM/ws/256B`에서 실패했다.
- round58에서 같은 `STREAM/ws/256B` 단독은 실패 직후에도 성공했다.

## 가설

1. full-prefix 없이도 STREAM `tcp -> tls -> ws` sequence만으로 `ws/256B` failure가
   재현되면, STREAM transport 전환/teardown 경로가 주된 원인이다.
2. 이 sequence가 성공하면, full-prefix의 앞쪽 workload가 failure를 유발하는 필요조건이다.

## 검증 예정

- stream sequence:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp,tls,ws --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round59_stream_tcp_tls_ws_sequence`

## 검증 결과

- stream sequence:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp,tls,ws --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round59_stream_tcp_tls_ws_sequence`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_060653_round59_stream_tcp_tls_ws_sequence.txt`
  - completion: success 6, fail 0, status complete.
  - elapsed: 55s.
  - `MULTI_STREAM/tcp/64B`: `330,382.8 ops/s`.
  - `MULTI_STREAM/tcp/256B`: `324,954.8 ops/s`.
  - `MULTI_STREAM/tls/64B`: `227,157.8 ops/s`.
  - `MULTI_STREAM/tls/256B`: `219,166.6 ops/s`.
  - `MULTI_STREAM/ws/64B`: `285,381.2 ops/s`.
  - `MULTI_STREAM/ws/256B`: `272,688.0 ops/s`.

## 결과

- STREAM `tcp -> tls -> ws` sequence만으로는 round57 `ws/256B` failure가 재현되지 않았다.
- round57 failure는 full-prefix가 필요조건이다.
- STREAM 성능 자체는 재현성 있게 목표 미달이다.
  - 이번 단독 sequence의 `tcp/64B`: `330,382.8 ops/s`.
  - 목표 baseline `tcp/64B`: `400,124.6 ops/s`.
  - 차이: 약 `-17.4%`.

## 판정

- 목표 달성 여부: 미달성.
- full-prefix failure는 아직 core 원인으로 특정되지 않았다. 단독 STREAM sequence가 통과하므로
  full-prefix의 앞쪽 workload가 timing/resource 조건을 만든다.
- source 변경 없음.
- 다음 단계는 failure 원인 추적과 별개로, `STREAM/tcp/64B` 400K 목표를 위해 source-level
  hot path를 다시 본다. perf helper의 send mutex 제거가 큰 효과를 보였지만 perf helper
  성능 변경은 금지되어 있으므로 core `stream` 송수신 경로에서만 후보를 찾아야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 라운드는 source 변경 없이 재현만 수행한다.
