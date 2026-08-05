# Round 58 - STREAM/ws/256B post-failure isolation

## 기준

- 목표 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 핵심 stream 목표:
  `MULTI_STREAM/tcp/64B = 400,124.6 ops/s`
- failure gate:
  full multi failure 0을 먼저 만족해야 한다.

## 시작 상태

- core/perf source 변경 없음.
- round57 full `64B,256B` repro는 `MULTI_STREAM/ws/256B`에서 client return code 2로
  partial 종료했다.
- round57에서 `MULTI_STREAM/tcp/64B = 332,091.0 ops/s`로 baseline 목표 미달이다.

## 가설

1. 실패 직후 `STREAM/ws/256B` 단독도 실패하면, full-prefix 또는 직전 STREAM transport가
   남긴 OS/socket 상태가 다음 stream run에도 영향을 준다.
2. 실패 직후 단독이 성공하면, failure는 full runner 내부의 transport sequence 또는 해당
   case의 teardown/readiness timing 문제에 가깝다.

## 검증 예정

- post-failure standalone:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=256 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports ws --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round58_stream_ws256_post_failure`

## 검증 결과

- post-failure standalone:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=256 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports ws --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round58_stream_ws256_post_failure`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_060617_round58_stream_ws256_post_failure.txt`
  - completion: success 1, fail 0, status complete.
  - elapsed: 7s.
  - `MULTI_STREAM/ws/256B`: `285,815.6 ops/s`.

## 결과

- round57 failure 직후에도 `STREAM/ws/256B` 단독은 성공했다.
- 따라서 round57 `STREAM/ws/256B` failure는 OS/socket 잔여 상태가 이후 단독 run까지
  지속되는 문제로는 보이지 않는다.
- failure는 full runner 내부 sequence의 특정 시점, 또는 full-prefix 뒤 STREAM
  `tcp -> tls -> ws` 순서의 일회성 timing/resource 조건으로 좁혀졌다.

## 판정

- 목표 달성 여부: 미달성.
- failure 원인은 아직 core/perf source 변경 없이 좁히는 중이다.
- 다음 단계는 `STREAM tcp,tls,ws` `64B,256B` sequence만 단독 실행해, full-prefix 없이도
  `ws/256B` failure가 재현되는지 확인한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 라운드는 source 변경 없이 재현만 수행한다.
