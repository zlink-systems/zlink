# Round 60: stream tcp64 pipe activation 후보

## 목표

- 비교 기준: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `RESULT,current,MULTI_STREAM,tcp,64,throughput,400124.600`.
- 현재 재현값: round57 `332,091.0 ops/s`, round59 `330,382.8 ops/s`.
- perf helper의 `send_mutex`나 runner/client/server 동작은 성능 목적 변경 대상에서 제외한다.
- core source에서 `STREAM/tcp/64B` echo 경로의 pipe write/activation 비용을 줄일 수 있는지 확인한다.

## 관찰

- `stream_dispatch_send_current_msg_from_io()`는 이미 현재 dispatch pipe를 사용하므로 routing map lookup은
  steady-state hot path에서 제거되어 있다.
- packet copy, inflight atomic, packet callback lock scope, current pipe lookup 제거 후보는 이전 round에서
  400K에 접근하지 못했고 revert되었다.
- 다음 후보는 `pipe_t::write_single_message_and_flush_no_recursive_hwm_check()`가 매 echo 응답마다
  `_out_sync`, HWM check, ypipe flush, 필요 시 activate-read command를 수행하는 구간이다.

## Clean 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round60_stream_tcp64_clean_repeat`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_061125_round60_stream_tcp64_clean_repeat.txt`
- result:
  - `MULTI_STREAM/tcp/64B = 330,724.2 ops/s`
  - baseline `400,124.6 ops/s` 대비 약 `-17.34%`.

## Baseline commit 같은 머신 확인

- baseline report의 commit은 `cb605c6c1`이다.
- 별도 worktree `/tmp/zlink_cb605c6c1`에서 해당 commit을 빌드했다.
- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round60_cb605c6c1_stream_tcp64_same_machine`
- report:
  `/tmp/zlink_cb605c6c1/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_061357_round60_cb605c6c1_stream_tcp64_same_machine.txt`
- result:
  - `MULTI_STREAM/tcp/64B = 370,106.2 ops/s`
  - 시작 load_avg가 `24.04 10.49 5.37`로 높았기 때문에 baseline 파일의 `400,124.6`까지는
    재현하지 못했지만, 같은 머신에서 현재 checkout clean 값보다 약 `+11.9%` 높다.

## 회귀 축

- baseline 이후 `1b60c0159 fix: serialize C multi stream sends`에서
  `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`에 `session_t::send_mutex`가 추가됐다.
- 이전 diagnostic에서 이 mutex 제거는 `379K~386K` 대역까지 회복 신호를 보였지만, perf helper를
  성능 목적으로 바꾸는 것은 이번 core source 개선 범위에서 제외한다.
- core public send API는 `zlink_send_part_rid()`의 STREAM+FINAL 경로에서 이미
  `send_stream_message()` direct path를 사용한다. 따라서 part-helper 상태머신 제거만으로는
  남은 `70K` gap을 설명하기 어렵다.
