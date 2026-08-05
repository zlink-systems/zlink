# Round 117-118: process_output 변경 A/B 재확인

## 이번 라운드 목표

- round112 retained 후보가 낮은 STREAM 수치와 실패를 만들 가능성이 있는지 확인한다.
- 같은 실행 환경에서 round112 변경 상태와 원복 상태를 연속으로 비교한다.
- 하락 항목이나 실패가 있으면 source는 원복 상태로 둔다.

## POSD 검토

- round112 변경은 중복 output-buffer 정책을 줄이는 방향이라 POSD 관점의 장점이 있다.
- 다만 성능 작업에서는 구조상 더 좋아 보여도 하락 항목이 반복되면 채택하지 않는다.
- 원복은 새로운 복잡도를 추가하지 않고, 확인되지 않은 최적화를 제거하는 판단이다.

## Round 117: round112 변경 상태 재확인

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round117_stream_ws_inclusive_recheck_after_guard
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_175743_round117_stream_ws_inclusive_recheck_after_guard.txt`
- status: partial, fail 3.
- failure: `MULTI_STREAM current tls 64B: non_zero_exit_2_size_64`.

| Transport | Throughput |
|-----------|-----------:|
| tcp | 276415.4 |
| tls | FAIL |
| ws | skipped |
| wss | skipped |

## Round 118: `process_output()` 원복 상태 A/B

```bash
cmake --build core/build --target libzlink -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round118_stream_ab_original_process_output
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_175847_round118_stream_ab_original_process_output.txt`
- status: complete, fail 0.
- load_avg: `4.27 4.72 7.71`.

| Transport | round117 changed | round118 original |
|-----------|-----------------:|------------------:|
| tcp | 276415.4 | 283969.4 |
| tls | FAIL | 184777.4 |
| ws | skipped | 217486.2 |
| wss | skipped | 172117.8 |

## 최종 판단

- round112 source 변경은 미채택으로 전환한다.
- 변경 상태에서 tls 실패가 재현됐고, 원복 상태는 같은 조건에서 완료됐다.
- round112의 초기 좋은 수치는 의미 있는 참고값이지만, 이후 guard와 A/B가 안정 채택 기준을 만족하지 못했다.
- 현재 source는 `process_output()` 원복 상태로 둔다.
