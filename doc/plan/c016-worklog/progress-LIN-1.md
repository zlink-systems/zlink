# LIN-1 진행

- 08:xx 시작. worktree ~/project/zlink-work/lin1 @ f5d7cccde2 생성, dev 빌드 시작 예정.

- 13:27~13:52 worktree lin1 @ f5d7cccde2 dev 빌드 완료.
- 재현 결과 (f5d7cccde2):
  - `ctest -R ^unittest_flow_state_monitor$ --repeat until-fail:30 --timeout 10` → 30/30 PASS (실패 0).
  - `taskset -c 0,1` 동일 명령 → **25번째 실행에서 실패**. `unittest_flow_state_monitor.cpp:664 test_pause_applied_by_pair_admission_is_booked: FAIL: Expected FALSE Was TRUE` + Forced closure of 2 sockets + Timeout 10.01s.
  - CI(run 34185949926)는 같은 테스트의 707행(Expected 0 Was 1)에서 실패 — 같은 테스트의 다른 국면.
- => 2코어 제한에서만 나오는 타이밍 의존 실패. 원인 분석 진행 중.

- 14:1x 메커니즘 규명(코드 독해):
  - `zlink_socket_monitor_open()` → `socket_base_monitor.cpp:329` → `acquire_monitor_async_command_processing()` (`socket_base_lifecycle.cpp:1145`) → `start_async_mailbox_processing()`. 즉 **모니터를 열면 그 소켓의 mailbox를 I/O 스레드가 비동기로 drain**한다.
  - 테스트는 662행 `inject_frame(..., drain_=false)` 뒤 664행에서 "큐잉된 flow_state 커맨드는 아직 실행되지 않았다"를 단정하는데, 모니터가 이미 열려 있으므로 I/O 스레드가 먼저 커맨드를 실행할 수 있다 → `Expected FALSE Was TRUE`.
  - CI의 707행(`paused_connections==0`)도 같은 뿌리: RESUMED 커맨드를 I/O 스레드가 처리하면 `pipe_t::apply_remote_flow_state()`가 `_out_sync` 안에서 파이프 플래그를 먼저 뒤집고(→ `wait_for_applied_pause(false)`가 즉시 true), 게이지 감소는 그 뒤 `socket_base_t::flow_state_applied()`에서 일어난다. 그 사이에 메트릭을 읽으면 1이 보인다.
  - 두 변경 파일(random.cpp / ctx_termination.cpp)과 인과관계 없음 → 선행 결함(타이밍 노출)로 잠정 판단. 되돌림 실험으로 확인 중.

- 14:3x 결정적 재현기 확보: `taskset -c 0` (1코어)로 3/3 실패(664행).
- 귀속 실험: random.cpp + ctx_termination.cpp 를 모두 f5d7cccde2~1 내용으로 되돌리고 재빌드 → `taskset -c 0` **3/3 동일 실패(664행, Timeout)**. => **f5d7cccde2 무관, 선행 결함**. 두 파일 원복.
- 14:5x 수정: core/tests/unittest/unittest_flow_state_monitor.cpp 의 test_pause_applied_by_pair_admission_is_booked 를 결정적으로 재작성.
  - 주입 전에 "아무것도 계상되지 않았다"를 확인하고, 주입 후의 "아직 파이프에 적용되지 않았다"(관측 불가능한 부정 단정) 제거.
  - 게이지/합계는 항상 해당 모니터 이벤트를 기다린 뒤 읽는다(flow_state_applied()가 게이지→합계→이벤트 순으로 커밋하므로 이벤트가 완료 경계).
  - RESUMED 뒤에도 두 번째 이벤트를 기다린 뒤 metrics 를 읽는다(=CI 707행 실패 원인 제거).
  - 검증: `taskset -c 0` 5/5 PASS.
- 패치: ~/project/zlink-work/all-artifacts/LIN-1.patch

- 15:2x 완료. 보고서 doc/plan/c016-worklog/core-rf-LIN-1-report.md 작성. 미커밋 상태로 lin1 워크트리에 둠.
