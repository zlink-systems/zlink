# #4162 PR #4323 — PM 리뷰 반영 결과

## 커밋

- `8dd6d661ae` — `perf: separate reference values from judged results`
- 브랜치 `perf/4162-latency-reference`, push하지 않았다.

## 결정 위치 표

| 항목 | 수정 전 | 수정 후 |
|---|---|---|
| 1. 참고값 판정 제외 | metric 이름 제외 분기 11곳, Python runner 허용 목록 예외 2곳 | 제외 분기 0곳, runner의 `REFERENCE,` 표시 경로 1곳. C 엔진만 `REFERENCE,`를 출력한다. |
| 2. invalid latency gate | parser 경고 1곳이 `result.errors`가 되어 보고서 전체를 비교 불가 처리 | invalid cell 기록·출력·제외 1곳. 다른 cell은 기존 비교를 계속한다. |
| 3. 엔진 invalid 사유 | NaN latency를 고정 사유로 출력하는 결정 1곳 | parser가 보관한 사유를 median 값과 함께 전달해 그대로 출력하는 결정 1곳 |
| 4. parser invalid 규칙 | multi는 전체 metric, single은 latency 계열만 수용 — 규칙 2개 | 지원되는 RESULT metric이면 `invalid:<reason>`을 invalid cell로 수용 — 규칙 1개 |

## 변경 파일과 이유

- `bindings/c/perf/multi/common/perf_multi_metrics.hpp`, `bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp`: 참고값과 `sender_late`를 `RESULT,`에서 `REFERENCE,`로 바꾸고 출력 기대값을 갱신했다.
- `bindings/c/perf/run_comparison.py`: `REFERENCE,` child 출력 전달을 한 표시 지점으로 모으고, invalid 이유를 값과 함께 보존해 출력한다.
- `bindings/c/perf/perf_regression_gate.py`: invalid cell을 오류가 아닌 별도 목록으로 보관하고 해당 cell만 판정에서 제외한다.
- `bindings/c/perf/single/run_comparison.py`: multi parser와 같은 invalid metric 수용 규칙을 적용했다.
- `bindings/c/perf/tests/test_perf_regression_gate.py`, `bindings/c/perf/single/tests/test_multi_run_comparison_policy.py`: invalid throughput regression과 listing, parser parity, reason 보존을 검증한다.
- `bindings/c/bench/with_zmq/{multi,single}/run_comparison.py`, `bindings/cpp/perf/{run_comparison.py,single/run_comparison.py}`, `bindings/dotnet/perf/single/run_emit.py`: 참고 metric 이름에 따른 중복 제외 분기를 제거했다.
- `bindings/python/perf/perf_metrics.py`, `bindings/python/perf/{multi,single}/run_benchmarks.py`, `bindings/python/tests/test_perf_runner.py`: 중복 제외 분기와 runner metric 허용 목록 예외를 제거하고 `REFERENCE,` 입력을 확인한다.
- `doc/perf/PERF_POLICY.md`: PUBSUB multi invalid cell 판정 규칙과 참고값 접두어를 기록했다.

## fail-before / after

Fail-before는 테스트만 추가한 뒤 현재 HEAD 제품 Python 파일 세 개(`run_comparison.py`, single parser, gate)의 hunk만 되돌려 실행했다. 테스트 파일은 유지했다.

```text
BEFORE gate: FAIL — PUBSUB latency가 missing-candidate로 판정되고 report_errors=1
BEFORE parser parity: FAIL — 같은 throughput invalid line에서 single parser 결과가 None
BEFORE reason: FAIL — 기대 engine_reason, 출력 receiver_not_caught_up_by_deadline
```

After gate는 invalid cell을 표시하면서 같은 PUBSUB row throughput regression을 판정했다.

```text
invalid: PUBSUB tcp 256 latency — receiver_not_caught_up_by_deadline
| single | PUBSUB | tcp | 256 | throughput | 100 | 94.9 | 0.9490 | ... | FAIL regression |
Final: FAIL (... report_errors=0)
```

Exit 1은 테스트 입력에 throughput regression이 있어 기대한 gate 결과다. 나머지 fail-before/after 로그는 `/root/jobs/fx/cx/v21/s033/f4162s/logs/`에 보존했다.

## 검증

- `test_multi_run_comparison_policy`: 48 passed.
- `test_perf_regression_gate`: 14 passed.
- `test_perf_runner`: 2 passed.
- with_zmq single parser policy: 6 passed. C++/dotnet parser 전용 테스트 파일은 찾지 못했다.
- `perf_multi_metrics_test`: passed.
- `bash scripts/format/format.sh --check cpp`: passed. 전체 C binding 파일에 직접 실행한 `clang-format-18 --dry-run`은 기존 코드의 다수 줄바꿈 차이로 실패했지만, 수정한 C++ 줄은 `clang-format-diff-18`에서 통과했다.
- Python 변경 파일 `py_compile` 및 `git diff --check`: 통과.
- Python 파일용 저장소 formatter/linter 설정이나 실행 도구는 확인되지 않았다.

## 실측 PUBSUB / TCP / 256 B

실행은 `flock /root/jobs/fx/cx/measure.lock` 단독 잠금으로 했다. server/source affinity 0–7, client/target affinity 8–15 적용을 확인했다.

```text
REFERENCE,current,PUBSUB,tcp,256,latency_ref_p50,1.769472
REFERENCE,current,PUBSUB,tcp,256,latency_ref_p99,3.768320
REFERENCE,current,PUBSUB,tcp,256,inflight_floor,2
REFERENCE,current,PUBSUB,tcp,256,inflight_floor_ms,0.300048
RESULT,current,PUBSUB,tcp,256,bandwidth,223.407
RESULT,current,PUBSUB,tcp,256,latency,invalid:receiver_not_caught_up_by_deadline
RESULT,current,PUBSUB,tcp,256,latency_p95,invalid:receiver_not_caught_up_by_deadline
RESULT,current,PUBSUB,tcp,256,latency_p99,invalid:receiver_not_caught_up_by_deadline
RESULT,current,PUBSUB,tcp,256,throughput,872684.000
```

`REFERENCE,` 출력은 child transcript에만 표시되고 Result Data에는 `RESULT,` 판정 줄만 남았다. 실측 뒤 reader와 callback의 중복 표시를 제거했고 parser policy 48건을 다시 통과시켰다. 보존한 raw 실측 로그는 이 수정 전 실행이라 `REFERENCE,` 줄이 중복 표시되어 있다. `sender_late`는 0이라 엔진이 출력하지 않았다. 전체 runner report는 `logs/real-run-report.txt`, raw log는 `logs/real-run.log`다.

## 남은 문제

- 없음. 작업용 `build-f4162s`와 테스트가 만든 Python cache를 제거했다.
