# #4162 — invalid PUBSUB latency 참고값

## 변경 파일과 이유

- `bindings/c/perf/multi/common/perf_multi_measurement.hpp`: PUBSUB catch-up 측정 기반과 진단값을 연결했다. 1 ms backlog 한도, 연속 0.5초 hold, 기존 latency duration deadline은 그대로 적용했다.
- `bindings/c/perf/multi/common/perf_multi_metrics.hpp`: p50 샘플 통계를 추가하고 invalid latency일 때만 p50/p99, in-flight floor와 ms값을 기록한다. late 송신 수는 0보다 클 때만 출력한다.
- `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp`: 관측 표본과 progress tick별 최소 subscriber in-flight floor를 모으고, catch-up 미도달 시 invalid 결과를 출력한다.
- `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp`: catch-up 이후 표본 phase를 진행하고 기존 `schedule.late_count`로 late RESULT를 출력한다.
- `bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp`: 유효 latency, invalid latency 참고값, late marker 및 catch-up 경계 테스트를 추가했다.
- `bindings/c/perf/run_comparison.py`: 참고 RESULT를 로그로 전달하되 집계에서 제외하고, invalid latency를 invalid 사유로 유지한다.
- `bindings/c/perf/single/run_comparison.py`: 참고 키와 late marker를 judged metric으로 파싱하지 않는다.
- `bindings/c/perf/perf_regression_gate.py`: 참고 키를 verdict 셀과 결과 개수에서 제외한다.
- `bindings/c/perf/single/tests/test_multi_run_comparison_policy.py`: reference/late 제외와 참고 RESULT 전달을 검증한다.
- `bindings/c/perf/tests/test_perf_regression_gate.py`: reference/late 키가 verdict를 만들지 않는지 검증한다.
- `bindings/c/bench/with_zmq/single/run_comparison.py`: reference/late 키를 verdict 파싱에서 제외한다.
- `bindings/c/bench/with_zmq/multi/run_comparison.py`: reference/late 키를 verdict 파싱에서 제외한다.
- `bindings/cpp/perf/run_comparison.py`: reference/late 키를 verdict 파싱에서 제외한다.
- `bindings/cpp/perf/single/run_comparison.py`: reference/late 키를 verdict 파싱에서 제외한다.
- `bindings/dotnet/perf/single/run_emit.py`: reference/late 키를 verdict 파싱에서 제외한다.
- `bindings/python/perf/perf_metrics.py`: reference/late 키를 metric row에서 제외한다.
- `bindings/python/perf/single/run_benchmarks.py`: report RESULT 개수를 judged metric만 세도록 했다.
- `bindings/python/perf/multi/run_benchmarks.py`: report RESULT 개수를 judged metric만 세도록 했다.
- `bindings/python/tests/test_perf_runner.py`: Python metric parser에서 reference/late 제외를 검증한다.
- `doc/perf/PERF_POLICY.md`: `판정 외 참고값` 필드 정의를 추가했다.

## 결정 위치와 상태 재사용

- invalid latency에서 reference RESULT를 출력하는 결정 위치: 수정 전 0곳 → 수정 후 1곳 (`run_single_size_case`의 invalid 분기).
- 송신 late 수는 새 counter 대신 기존 `schedule.late_count`를 사용한다.
- 필요한 통계 필드 `p50_ns`, 관측 sampler, progress tick 최소값 `inflight_floor`를 추가했다.

## 검증

- 기준선 실패: `test-fail-before-clean.log`, exit 1. 기존 코드에는 `p50_ns`와 `print_invalid_latency_result`가 없어 probe compile이 실패했다.
  ```text
  error: ‘struct bench_latency_stats_t’ has no member named ‘p50_ns’
  error: ‘print_invalid_latency_result’ was not declared in this scope
  FAIL_BEFORE_EXIT=1
  ```
- 변경 후 C++ 메트릭 테스트: `test-pass-after.log`, exit 0.
  ```text
  PASS_AFTER_EXIT=0
  ```
- CMake에서 `perf_multi_metrics_test`, `comp_src_pubsub_client`, `comp_src_pubsub_server` 빌드 성공. Core prefix는 1.24.0이다. C binding은 worktree 소스를 빌드했으며, `USE_BINDINGS_SOURCE=ON`은 C 프로젝트에서 미사용 변수라는 CMake 경고가 있었다.
- 관련 Python 테스트 통과: C multi policy 46, C regression gate 13, Python perf runner 2, C zmq policy 6. 다중 consumer parser smoke 5/5 통과.
- `git diff --check` 통과.

## 실측 RESULT

1회 PUBSUB/TCP 256 B 실행 결과다. 참고값은 child RESULT 출력과 결과 RESULT 데이터에서 합쳐 적었다. `sender_late`는 0이라 출력되지 않았다.

```text
RESULT,current,PUBSUB,tcp,256,throughput,1357049.200
RESULT,current,PUBSUB,tcp,256,bandwidth,347.405
RESULT,current,PUBSUB,tcp,256,latency,invalid:receiver_not_caught_up_by_deadline
RESULT,current,PUBSUB,tcp,256,latency_p95,invalid:receiver_not_caught_up_by_deadline
RESULT,current,PUBSUB,tcp,256,latency_p99,invalid:receiver_not_caught_up_by_deadline
RESULT,current,PUBSUB,tcp,256,latency_ref_p50,1.548288
RESULT,current,PUBSUB,tcp,256,latency_ref_p99,50.855936
RESULT,current,PUBSUB,tcp,256,inflight_floor,4
RESULT,current,PUBSUB,tcp,256,inflight_floor_ms,0.369697
BENCH_DIAG,PUBSUB,256,active_sender_rate,456187.2,target_sender_rate,10819.68,latency_sends,108197,late_sends_gt_1ms,0,received_rate,13524.6,paced_received_rate,0,caught_up,no,catch_up_seconds,-1
```

- raw 실행 로그: `/root/jobs/fx/cx/v21/s033/f4162r/raw/run-pubsub-one.log`
- 저장된 report: `/root/jobs/fx/cx/v21/s033/f4162r/raw/results-pubsub-one/multi/report/perf_c_multi_linux_20261111_233222.txt`
- governor: cpu0–15를 실행 중 performance로 설정했고 trap 후 powersave 복원을 확인했다.

- 로컬 commit: `65fc5b9902` (`perf(c): record invalid PUBSUB latency references`), push하지 않았다.
