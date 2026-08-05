# Round 11: distributor final data frame write helper 검토

- 기준 커밋: `5e3c438a2`
- 시작 상태: core source diff 없음. round 9, round 10 로그만 미추적 상태.
- 대상: `dist_t` fanout write path
- 제외: `bindings/c/perf` runner, client, server 수정

## 배경

`MULTI_PUBSUB`와 `MULTI_SPOT` 64B one-way는 distributor가 여러 pipe로 같은 메시지를
fanout한다. 현재 `dist_t::write_at()`은 최종 단일 data frame에서도
`pipe_t::write_and_flush_no_recursive_hwm_check()`를 호출한다. 이 함수는 매 pipe마다
`msg_->flags()`와 `msg_->is_routing_id()`를 다시 확인한다.

`pipe_t`에는 이미 final non-routing-id 메시지를 위한
`write_single_message_and_flush_no_recursive_hwm_check()`가 있다. 이 helper는 같은 lock,
HWM, flush 규칙을 유지하면서 final data frame에 불필요한 분기를 줄인다.

## 가설

1. PUBSUB/SPOT fanout에서 pipe 수만큼 반복되는 final-frame 분기를 줄이면 64B one-way
   처리량이 개선될 수 있다.
2. 병목이 transport 또는 downstream wakeup이라면 개선 폭은 10%에 못 미칠 것이다.

## 적용 계획

- `dist_t::write_at()`에서 multipart frame은 기존 경로를 유지한다.
- final frame 중 routing-id가 아닌 메시지만
  `write_single_message_and_flush_no_recursive_hwm_check()`를 사용한다.
- routing-id frame은 `_msgs_written` 증가 의미가 다르므로 기존 경로를 유지한다.

## 검증 계획

- `cmake --build core/build`
- `ctest --test-dir core/build --output-on-failure -R 'test_pubsub$|test_transport_matrix|test_backpressure_(oneway_)?matrix_pubsub_regression|test_backpressure_(oneway_)?matrix_spot_regression|test_spot_pubsub_scenario'`
- targeted 64B perf:
  - `PUBSUB,SPOT`
  - `tcp,tls,ws,wss`
  - failure 0 유지

## 판정 기준

- 10% 이상 반복 가능한 개선이 없거나 transport별 결과가 혼재하면 source 변경을 되돌린다.

## 적용 내용

`dist_t::write_at()`에서 multipart frame은 기존
`write_no_recursive_hwm_check()`를 유지하고, final data frame 중 routing-id가 아닌
메시지만 `write_single_message_and_flush_no_recursive_hwm_check()`를 쓰도록 임시 변경했다.

## 검증

- `cmake --build core/build`
  - 통과
- `ctest --test-dir core/build --output-on-failure -R 'test_pubsub$|test_transport_matrix|test_backpressure_(oneway_)?matrix_pubsub_regression|test_backpressure_(oneway_)?matrix_spot_regression|test_spot_pubsub_scenario'`
  - 15/15 통과
- `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5 --results-tag round11_dist_final_helper`
  - 결과 파일: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_170826_round11_dist_final_helper.txt`
  - success 8, fail 0

## 64B targeted 비교

비교 기준은 zero-fail 전체 기준
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`이다.

| 패턴 | 전송 | 변화 |
|------|------|------|
| `MULTI_PUBSUB` | tcp | -1.45% |
| `MULTI_PUBSUB` | tls | -1.37% |
| `MULTI_PUBSUB` | ws | -0.88% |
| `MULTI_PUBSUB` | wss | +0.51% |
| `MULTI_SPOT` | tcp | -0.68% |
| `MULTI_SPOT` | tls | -14.86% |
| `MULTI_SPOT` | ws | -5.56% |
| `MULTI_SPOT` | wss | +19.16% |

## 판정

failure 0은 유지했지만 결과가 transport별로 크게 갈렸다. `MULTI_SPOT wss`는 +19.16%였지만
`MULTI_SPOT tls`는 -14.86%였고, PUBSUB는 의미 있는 개선이 없었다.
따라서 source 변경은 되돌렸다.

## 결론

- distributor final-frame helper 선택은 안정적인 개선 후보가 아니다.
- 같은 distributor fanout 안에서도 transport별 병목이 다르거나 noise가 큰 상태다.
- 다음 후보는 per-transport wakeup/flush 비용 또는 SPOT data-plane 경로를 더 직접적으로
  분리해 보아야 한다.
