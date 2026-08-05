# Round 87: mailbox/wakeup hot path 후보 검토

## 목표

전체 64B one-way 처리량에 영향을 줄 수 있는 mailbox/wakeup 경로에서 POSD-safe 후보가 있는지 확인한다.
완료 기준은 실제 call path를 확인하고, 새 상태나 재진입 위험 없이 적용 가능한 후보가 없으면 수정하지 않고
근거를 기록하는 것이다.

## 기준 report

- May26 full 보정 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 최신 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음
- 최근 미채택 후보:
  - round84: TCP native send
  - round85: STREAM current pipe lookup
  - round86: ASIO output buffer loop 중복 제거

## 병목 가설

1. 64B one-way path는 `pipe_t::flush_unlocked()`가 peer pipe에 `activate_read` command를 보내는
   빈도가 높다. 같은 thread 활성화에서는 mailbox roundtrip을 줄일 수 있다.
2. mailbox/wakeup 비용보다 pipe HWM, fanout, TLS write completion, perf sequence variance가 더 크다.
   같은 thread 직접 처리나 schedule 정책 변경은 재진입/공정성 위험만 늘릴 수 있다.

## 확인한 코드

- `core/src/runtime/core/pipe.cpp`
  - `flush_unlocked()`는 reader 활성화를 위해 `send_activate_read(_peer)`를 호출한다.
  - `process_activate_write()`는 `_out_active`를 다시 켜고 sink에 `write_activated()`를 알린다.
- `core/src/runtime/core/object.cpp`
  - `send_activate_write()`는 이미 `destination_->get_tid() == _tid`이면 `process_command()`를 직접 호출한다.
  - `send_activate_read()`는 항상 command path를 사용한다.
- `core/src/runtime/core/mailbox.cpp`
  - `send()`는 cpipe에 command를 넣고, receiver가 inactive일 때만 signal/schedule한다.
  - async mailbox는 `_scheduled` atomic으로 중복 post를 막는다.
- 이전 관련 검증:
  - round26의 `activate_read` 직접 처리 후보는 STREAM/tcp 64B를 올리지 못해 되돌렸다.

## POSD 판단

- `activate_write` 직접 처리는 이미 존재한다. 같은 패턴을 `activate_read`에 적용하는 것은 표면상 단순하지만,
  read activation은 sink의 `read_activated()` 재진입을 유발할 수 있다.
- round26에서 이미 같은 방향의 후보가 성능 개선을 만들지 못했다.
- mailbox schedule 정책을 바꾸려면 `_active`, `_scheduled`, cpipe 상태의 의미를 함께 바꿔야 하므로
  shallow optimization이 아니라 concurrency contract 변경이 된다.
- 새 상태, 별도 wakeup cache, perf 전용 shortcut은 POSD와 이번 목표에 맞지 않는다.

## 판정

- 이번 round에서는 source를 수정하지 않는다.
- mailbox/wakeup 후보는 현재 근거로는 채택하지 않는다.
- 다음 후보는 message allocation/refcount 또는 pipe enqueue/dequeue에서 기존 구현을 더 직접 확인한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: source 변경이 없다.
- 추가로 실행한 회귀 테스트: 없음
