**변경 요청 — 판정 외 참고값을 거르는 규칙이 11곳에 복사되어 있습니다**

histogram, catch-up 판정, PUBSUB 양쪽 steady_clock, matched client bucket 복사, reservoir 제거와 정책 문서 갱신은 확인했고 문제가 없습니다. 아래 네 가지를 고쳐 주십시오.

1. **같은 제외 규칙이 11곳에 있습니다.** `metric.startswith("latency_ref_") or metric in ("inflight_floor", "inflight_floor_ms", "sender_late")`가 다음 파일에 반복됩니다.
   - c/bench/with_zmq multi·single
   - c/perf run_comparison(두 곳)·single
   - cpp/perf run_comparison·single
   - dotnet run_emit
   - python perf_metrics
   - perf_regression_gate

   gate에서는 같은 줄을 자기 코드로 한 번 거르고, `REPORT_PARSER.parse_result_line`이 같은 규칙으로 다시 거릅니다. 참고값 metric을 하나 더하면 열한 곳을 고쳐야 하고, 한 곳만 빠뜨려도 `latency_ref_*`가 `LATENCY_PREFIX` 규칙으로 판정 cell이 됩니다.

   **제안:** 판정 값이 아니라는 사실은 값을 만드는 C 엔진이 정합니다. 참고값을 `RESULT,` 대신 별도 접두어(예: `REFERENCE,`)로 출력하면, 모든 parser는 `RESULT,` 줄만 읽으므로 고칠 곳이 없습니다. runner가 참고값을 화면에 다시 보여 주는 곳 한 군데만 그 접두어를 알면 됩니다.
   - 판정 위치: 수정 전 11곳 → 수정 후 0곳(표시 1곳).
   - python runner가 `REQUIRED_METRICS`로 허용 목록을 쓰는 두 곳의 추가 분기도 함께 지울 수 있습니다.

2. **gate에서 invalid latency가 보고서 전체를 비교 불가로 만듭니다.** `parse_result_line`의 warning이 `result.errors`에 들어가므로, PUBSUB latency 하나가 invalid이면 그 보고서의 throughput cell까지 판정하지 못합니다. 정책 문서는 "throughput은 유효한 측정값을 유지"한다고 적었습니다.
   - 둘 중 어느 쪽으로 할지 정해 주십시오.
     - invalid cell만 판정에서 빼고 명시적으로 표시한다.
     - 보고서를 비교 불가로 둔다.
   - 정한 쪽을 PERF_POLICY에 한 줄로 적고, gate 시험을 한 건 넣어 주십시오.

3. **multi run_comparison이 invalid 사유를 다시 쓰지 않고 고정 문자열로 바꿉니다.** `bindings/c/perf/run_comparison.py:4291`은 NaN이 된 latency를 모두 `invalid:receiver_not_caught_up_by_deadline`으로 출력합니다. 엔진이 다른 사유를 보내면 그 사유가 사라집니다. 사유 문자열을 cell과 함께 보관해 그대로 출력해 주십시오.

4. **invalid를 받는 metric 범위가 parser마다 다릅니다.**
   - c/perf multi parser: 모든 metric에서 `invalid:`를 받습니다.
   - single parser: `latency`, p95, p99에서만 받고, 그 밖의 metric은 `float()`에서 예외가 납니다.

   같은 결정에 규칙이 둘입니다. 하나로 맞춰 주십시오.

