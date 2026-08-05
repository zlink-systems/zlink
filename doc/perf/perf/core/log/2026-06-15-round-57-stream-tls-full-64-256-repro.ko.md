# Round 57 - STREAM/tls full 64/256 repro without debug noise

## 기준

- 목표 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 핵심 stream 목표:
  `MULTI_STREAM/tcp/64B = 400,124.6 ops/s`
- failure gate:
  full multi failure 0을 먼저 만족해야 한다.

## 시작 상태

- core/perf source 변경 없음.
- round52와 round55 full run에서 `MULTI_STREAM/tls` failure가 반복 재현되었다.
- round55의 후반 subset은 실패를 재현하지 못했다.
- round56의 `PERF_DEBUG=1` full-prefix run은 debug 출력이 과도해 `STREAM`에 도달하기
  전에 중단했다. 이 결과는 failure 판단 자료로 쓰지 않는다.

## 가설

1. full sequence의 전체 앞쪽 workload를 유지하되 size를 `64B,256B`로 제한해도
   `MULTI_STREAM/tls` failure가 재현된다.
2. 재현되면 실패 비용을 full all-size보다 크게 낮출 수 있고, 다음 디버그는
   STREAM 전용으로 제한할 수 있다.
3. 재현되지 않으면 failure는 all-size run의 누적 연결/해제량 또는 큰 size까지 포함한
   장시간 실행 조건에 더 가깝다.

## 검증 예정

- full 64/256 repro:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round57_full_64_256_repro`

## 검증 결과

- full 64/256 repro:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round57_full_64_256_repro`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_055424_round57_full_64_256_repro.txt`
  - completion: success 60, fail 4, status partial.
  - expected_result_lines: 320.
  - actual_result_lines: 300.
  - elapsed: 11m 7s.
  - failed item:
    `MULTI_STREAM current ws 256B`
  - failure details:
    - `missing_bandwidth_non_zero_exit_2_size_256`
    - `missing_latency_non_zero_exit_2_size_256`
    - `missing_latency_p95_non_zero_exit_2_size_256`
    - `missing_latency_p99_non_zero_exit_2_size_256`
    - `missing_throughput_non_zero_exit_2_size_256`

## 결과

- `64B,256B` full-prefix만으로도 `MULTI_STREAM` transport failure가 재현되었다.
- 이번에는 `STREAM/tls`가 아니라 `STREAM/ws/256B`가 실패했다.
- `MULTI_STREAM/tcp/64B`: `332,091.0 ops/s`.
- `MULTI_STREAM/tcp/256B`: `326,629.4 ops/s`.
- `MULTI_STREAM/tls/64B`: `226,232.8 ops/s`.
- `MULTI_STREAM/tls/256B`: `216,411.0 ops/s`.
- `MULTI_STREAM/ws/64B`: `286,359 ops/s`로 성공했다.
- `MULTI_STREAM/ws/256B`: client return code 2로 실패해 metric이 없다.

## 판정

- 목표 달성 여부: 미달성.
- `MULTI_STREAM/tcp/64B`는 baseline `400,124.6 ops/s` 대비 약 `-17.0%`로 목표에
  못 미친다.
- failure는 `tls` 고정 문제가 아니라 full-prefix 뒤에서 `STREAM`의 다음 transport/size가
  client pass 조건을 만족하지 못하는 문제로 보인다.
- source 변경 없음.
- 다음 단계는 full-prefix 뒤 STREAM failure를 더 작게 만들기 위해 `STREAM` 내
  transport sequence를 `tcp,tls,ws`로 제한하거나, `STREAM ws 256B` 단독/후속 재현을
  비교한다. 단독이 성공하면 원인은 앞선 STREAM `tcp,tls` 또는 full-prefix의 누적
  연결/해제 상태다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 라운드는 source 변경 없이 재현만 수행한다.
- WS/WSS pending message copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
