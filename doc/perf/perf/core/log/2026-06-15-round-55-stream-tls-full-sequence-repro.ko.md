# Round 55: STREAM tls full-sequence 실패 축소 재현

- goal: full multi perf 실패 0개 목표를 위해 round52에서 나온 `MULTI_STREAM/tls`
  partial 실패를 축소 재현한다.
- 완료 기준:
  - source 변경 없이 full sequence 후반과 유사한 targeted sequence를 실행한다.
  - 실패가 재현되면 pattern/transport/size, report, stderr 단서를 기록하고 core 경로를
    추적한다.
  - 실패가 재현되지 않으면 round52 실패를 장시간 full-run 누적/부하성 이슈로 분리하고,
    다음 round는 더 강한 재현 조건 또는 전체 64B hot path로 이동한다.
- 시작 시각: 2026-06-15 KST
- 시작 git status:
  - core/perf source diff: empty
  - perf log files under `doc/plan/perf/core/log` are untracked

## 기준 report

- baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- round52 full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_042257_round52_full_failure_gate.txt`
- round52 stream tcp,tls:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_045320_round52_stream_tcp_then_tls_repro.txt`

## 기준 수치와 실패

- baseline completion: success 184, fail 0.
- problem completion: success 152, fail 40.
- round52 full completion: success 174, fail 18, status partial.
- round52 full failure list:
  - `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 256B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 1024B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 4096B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 65536B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current tls 131072B: non_zero_exit_2_size_64`
- round52 `STREAM/tls/64B` 단독: success 1, fail 0, `228,244.6 ops/s`.
- round52 `STREAM tcp,tls`: success 12, fail 0.

## 가설

1. round52 `STREAM/tls` 실패는 STREAM 자체가 아니라 full sequence 앞쪽의 SPOT service
   패턴 또는 장시간 실행 후 남은 socket/thread/resource 상태가 영향을 준다.
2. round52 실패는 load 또는 일회성 process 상태 문제이며, 동일 sequence를 좁혀도 반복되지
   않는다.
3. 실패가 재현되면 core TLS transport 또는 STREAM accept/connect readiness 경로에서
   이전 pattern 종료 후 리소스 회수 지연이 드러나는지 본다. perf runner 조건은 바꾸지
   않는다.

먼저 검증할 가설: 가설 1. round52에서 실패 직전까지 거친 후반 sequence를 축소해
`SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM` 순서로 실행한다. transport는 round52 실패가
나온 `STREAM/tls`까지 가도록 `tcp,tls`로 제한한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: 이번 라운드는 source 변경 없이 실패 재현만 수행한다.
  WS/WSS pending message copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.

## 검증 예정

- targeted sequence:
  - command:
    `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --pattern SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round55_spot_stream_tls_sequence`

## 검증 결과

- targeted sequence:
  - command:
    `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --pattern SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round55_spot_stream_tls_sequence`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_050515_round55_spot_stream_tls_sequence.txt`
  - completion: success 48, fail 0, status complete.
  - elapsed: 9m 53s.
  - load_avg: `12.13 18.96 12.49`.

## 결과

- `SPOT`, `SPOT_REQREP`, `SPOT_SENDSEND`, `STREAM`의 `tcp,tls` 전체 48개 조합은
  모두 성공했다.
- round52에서 실패한 `MULTI_STREAM/tls` 6개 size는 이번 축소 sequence에서 모두
  성공했다.
- `MULTI_STREAM/tcp/64B`: `330,892.2 ops/s`.
- `MULTI_STREAM/tls/64B`: `218,702.8 ops/s`.
- `MULTI_SPOT/tcp/256B`가 `5,120.0 ops/s`로 단일 이상치를 보였다. 같은 run의
  `64B`, `1024B`, `4096B`, `65536B`, `131072B`는 정상 대역이므로 이 라운드는
  성능 판단 자료가 아니라 실패 재현 자료로만 사용한다.

## 판정

- 목표 달성 여부: 미달성.
- round52 full failure는 후반 `SPOT* -> STREAM tcp,tls` subset만으로는 재현되지 않았다.
- 따라서 실패 조건은 더 긴 full sequence, 앞쪽 DEALER/ROUTER/PUBSUB 포함 누적, 또는
  일회성 부하/리소스 상태일 가능성이 남는다.
- source 변경 없음. 다음 선택지는 두 가지다.
  - full failure 0 목표를 위해 full run을 한 번 더 반복해 round52 실패가 반복되는지 본다.
  - 또는 full 재현 비용이 큰 만큼, 64B 전체 hot path로 돌아가되 full failure는 아직
    미해결 위험으로 남긴다.

현재 원칙상 실패가 있으면 성능 개선보다 먼저 실패 0개 상태를 만들어야 하므로, 다음은 full
또는 앞쪽 pattern을 더 포함한 sequence로 반복 재현을 시도한다.

## 추가 검증 예정

- full repeat:
  - command:
    `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round55_full_repeat_after_subset`
  - 목적: round52 full partial이 반복되는지 확인한다.

## 추가 검증 결과

- full repeat:
  - command:
    `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round55_full_repeat_after_subset`
  - runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_051558_round55_full_repeat_after_subset.txt`
  - completion: success 174, fail 18, status partial.
  - expected_result_lines: 960.
  - actual_result_lines: 870.
  - failed item:
    `MULTI_STREAM current tls 256B`
  - failure details:
    - `missing_bandwidth_non_zero_exit_2_size_256`
    - `missing_latency_non_zero_exit_2_size_256`
    - `missing_latency_p95_non_zero_exit_2_size_256`
    - `missing_latency_p99_non_zero_exit_2_size_256`
    - `missing_throughput_non_zero_exit_2_size_256`

## 추가 결과

- round52와 마찬가지로 full sequence에서는 `MULTI_STREAM/tls` failure가 반복되었다.
- 이번 반복에서는 `64B`가 아니라 `256B`에서 먼저 실패했다.
- full repeat의 `MULTI_STREAM/tcp/64B`: `324,352.6 ops/s`.
- full repeat의 `MULTI_STREAM/tcp/256B`: `324,661.0 ops/s`.
- `SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM`의 `tcp,tls` subset은 실패하지 않았지만,
  full sequence는 다시 partial로 끝났다. 따라서 실패 조건은 STREAM 자체 단독 조건이
  아니라 full sequence 앞쪽 workload 누적, 장시간 실행 후 리소스 상태, 또는 earlier
  pattern의 teardown 영향으로 좁혀졌다.

## 추가 판정

- 목표 달성 여부: 미달성.
- full failure 0 조건은 아직 만족하지 못했다.
- source 변경 없음.
- 다음 단계는 runner가 기록한 `non_zero_exit_2` 의미와 process stderr 위치를 확인하고,
  필요하면 full sequence를 `64B,256B`로 줄여 반복 비용을 낮춘 재현을 만든다.
