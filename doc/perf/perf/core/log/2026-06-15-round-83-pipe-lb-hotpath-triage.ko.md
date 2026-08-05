# Round 83 - pipe/lb hot path triage

## 이번 라운드 목표

- `pipe`, `fq`, `lb` hot path에서 새 상태를 늘리지 않는 후보가 있는지 확인한다.
- 완료 기준: call path를 확인하고, POSD-safe 후보가 있으면 검증한다. 후보가 위험하면 수정하지 않고 근거를 기록한다.

## 기준 report

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- current reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
- round82 focused:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_141449_round82_dist_more_pubsub_focused.txt`

## 시작 상태

- current HEAD: `903a366c0`
- core source diff: 없음.
- perf code: 수정하지 않는다.

## 병목 가설

1. one-way send 경로의 `pipe_t::check_hwm_unlocked()`나 write/flush lock path가 64B 비용에 영향을 준다.
2. recv 경로의 `fq_t::normalize_state()`와 `pipe_t::check_read()` 반복이 small-message recv 비용에 영향을 준다.
3. `lb_t` weighted scheduling map lookup이 DEALER 계열 one-way 비용에 영향을 줄 수 있다.

## 확인 결과

- `pipe_t` write path:
  - `write_and_flush_no_recursive_hwm_check()`와 `write_single_message_and_flush_no_recursive_hwm_check()`가 이미 있다.
  - `check_hwm_unlocked()`는 `_hwm > 0 && _msgs_written - _peers_msgs_read >= hwm` 한 줄로, 제거하거나 우회하면 HWM 계약을 흔든다.
- `fq_t` recv path:
  - `normalize_state()`는 activation/termination 뒤 active/current 상태를 보정한다.
  - 성능 목적으로 제거하면 pipe termination edge에서 상태 불일치 위험이 커진다.
- `lb_t` send path:
  - one active pipe fast path가 이미 있다.
  - weighted scheduling은 `std::map`을 쓰지만 peer weight 계약과 multi-pipe fairness에 연결되어 있다.
  - map lookup을 없애려면 pipe에 weight를 중복 저장하거나 별도 side table을 추가해야 해서 상태와 정보 중복이 늘어난다.

## 판정

- 이번 범위에서 바로 적용할 POSD-safe 후보는 찾지 못했다.
- HWM/termination/fairness 경계는 성능 라운드에서 추측으로 줄이지 않는다.
- 다음 후보는 ASIO batching/read-write 정책에서 새 상태 없이 적용 가능한 변경을 찾는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 소스 변경 없이 triage만 수행했다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, port parsing, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - 소스 변경 없음.
