# LIN-1 — Linux CI `unittest_flow_state_monitor` 실패 귀속과 수정

- 대상: GitHub Actions build.yml run 34185949926, job "Build Linux x64"(ubuntu-24.04, 2 cores)
- 증상: `unittest_flow_state_monitor` ***Timeout 10.01 sec,
  `core/tests/unittest/unittest_flow_state_monitor.cpp:707 test_pause_applied_by_pair_admission_is_booked: FAIL: Expected 0 Was 1`,
  이어서 "Forced closure of 2 sockets"
- 작업 트리: `~/project/zlink-work/lin1` (detached @ f5d7cccde2), dev 빌드(JOBS=4)
- 결론: **f5d7cccde2(WIN-1)와 무관한 선행 테스트 결함**. 수정은 테스트 파일 1개.

## 1. 재현 매트릭스

| 빌드 | 실행 조건 | 결과 |
|---|---|---|
| f5d7cccde2 (main) | `ctest --repeat until-fail:30 --timeout 10` | 30/30 PASS |
| f5d7cccde2 (main) | `taskset -c 0,1` + 위와 동일 | **25번째 실행 실패** — `:664 FAIL: Expected FALSE Was TRUE` + Timeout 10.01 s |
| f5d7cccde2 (main) | `taskset -c 0` (1코어) 단발 ×3 | **3/3 실패** — 동일 `:664`, 동일 Timeout |
| f5d7cccde2 + random.cpp·ctx_termination.cpp 를 f5d7cccde2~1 로 되돌림 | `taskset -c 0` 단발 ×3 | **3/3 실패** — 동일 `:664`, 동일 Timeout |
| 수정 후 | `taskset -c 0` 단발 ×5 / `until-fail:10` | 전부 PASS |
| 수정 후 | `taskset -c 0,1 --repeat until-fail:40 --timeout 10` | 40/40 PASS |
| 수정 후 | `--repeat until-fail:30 --timeout 10` | 30/30 PASS |
| 수정 후 | `ctest -R 'flow|monitor|pair|ctx|term|random' --repeat until-fail:3` | 41/41 PASS |

귀속: 두 변경 파일을 부모(f5d7cccde2~1) 내용으로 되돌린 빌드에서 **완전히 동일하게 실패**하므로,
`random.cpp`(RtlGenRandom/`weak_random()` 재구성)도 `ctx_termination.cpp`(`wait_for_reaper_done()` EAGAIN 재대기)도
원인이 아니다. f5d7cccde2 는 소켓·파이프 생성 경로의 `generate_random()` 이 `getrandom()` 시스템콜을 타게 만들어
타이밍을 바꿨을 뿐이고, 2코어 러너에서 원래 있던 경합이 드러난 것이다.
(실패 뒤의 Timeout·"Forced closure" 역시 되돌린 빌드에서 동일하게 재현 — 이것도 f5d7cccde2 탓이 아니다.
Unity 의 FAIL longjmp 로 `fixture.teardown()` 가 건너뛰어지고, 강제 종료된 소켓 때문에 컨텍스트 종료가 끝나지 않는 기존 동작이다.)
개별 부분 되돌림(파일 하나씩)은 두 파일 동시 되돌림이 이미 실패를 재현했으므로 불필요해 판단하고 생략했다.

## 2. 근본 원인

| 위치 | 내용 |
|---|---|
| `core/src/runtime/sockets/common/socket_base_monitor.cpp:329` | `zlink_socket_monitor_open()` 경로가 `acquire_monitor_async_command_processing()` 을 호출한다 |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1145`, `:1186` | 그 함수가 `start_async_mailbox_processing()` 으로 **I/O 스레드에 그 소켓의 mailbox 실행자(async command owner)를 설치**한다 |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:472`, `:500` | 이후 `process_commands()` 는 `async_mailbox_owns_commands() && !owns_control_scope` 이면 **아무것도 처리하지 않고 0 을 반환**한다 → 테스트의 `contract_socket_pair_t::pump_owner()`(= `test_process_commands_only()`)는 no-op 이 되고, 커맨드는 오직 I/O 스레드가 실행한다 |
| `core/src/runtime/core/pipe.cpp:2092` `apply_remote_flow_state()` | `_out_sync` 안에서 파이프의 `_remote_flow_paused` 를 먼저 뒤집고 결정만 돌려준다 |
| `core/src/runtime/core/pipe.cpp:2060` → `core/src/runtime/sockets/common/socket_base_flow_state.cpp:374` `flow_state_applied()` | 그 **뒤에** 게이지(`_flow_paused_connections`) → 합계 → 모니터 이벤트 순으로 계상한다 |

테스트는 프로브 모니터를 연 상태에서

- (구) 664행 `TEST_ASSERT_FALSE (application_pipe_remote_flow_paused (...))`
  — "큐잉된 FLOWSTATE 커맨드는 아직 실행되지 않았다"를 단정했다. 그러나 모니터가 열린 순간부터 I/O 스레드가
  그 커맨드를 언제든 실행할 수 있으므로 이 부정 단정은 **테스트 스레드가 관측할 수 없는 값**이다.
  → 1코어/2코어에서 `Expected FALSE Was TRUE`.
- (구) 706~707행 — `wait_for_applied_pause(false)` 는 **파이프 플래그**만 본다. I/O 스레드가 RESUMED 커맨드를
  처리하는 중이면 플래그는 이미 false 인데 `flow_state_applied()` 의 게이지 감소는 아직이다.
  그 사이에 `read_flow_metrics()` 를 읽으면 `paused_connections == 1`.
  → CI 가 본 `:707 Expected 0 Was 1`.

즉 CI 의 707행 실패와 로컬의 664행 실패는 **같은 뿌리(모니터가 설치한 비동기 커맨드 실행자와의 경합)** 의 두 국면이다.

## 3. 수정

파일: `core/tests/unittest/unittest_flow_state_monitor.cpp` (테스트 1개, 공개 인터페이스·계약·제품 코드 변경 없음)

| 변경 | 내용 |
|---|---|
| 순서 교체 | "아무것도 계상되지 않았다"(`remote_flow_paused == false`, `pause_applied == 0`)를 **주입 전에** 확인. 주입 후의 "아직 적용되지 않았다"는 부정 단정은 제거 — 두 인터리빙(관리 진입 admission 이 적용 / I/O 스레드가 큐 커맨드를 먼저 적용) 모두에서 성립하는 것만 단정한다: 플립은 **정확히 한 번** 계상된다 |
| 동기화 지점 | 게이지·합계는 항상 대응하는 **모니터 이벤트를 기다린 뒤** 읽는다. `flow_state_applied()` 가 게이지→합계→이벤트 순으로 커밋하므로 이벤트가 "계상 완료" 경계다 |
| RESUMED 쪽 | `wait_for_applied_pause(false)` 뒤에 `flow_unit_monitor_has_count (&probe, 2, 2000)` 를 추가하고 그 다음에 metrics 를 읽는다 (CI 707행 원인 제거) |
| 주석 | 위 메커니즘(모니터 = 비동기 커맨드 소유자, 이벤트가 계상 완료 경계)을 테스트 머리주석에 명시 |

설계 비교:
- (A) 채택: 관측 불가능한 부정 단정을 제거하고, 이벤트를 완료 경계로 삼아 동기화.
  새 제어점·플래그·훅을 추가하지 않고, 테스트가 관측할 수 있는 것만 단정한다.
- (B) 기각: 모니터를 주입·사전 단정 뒤에 열어 창을 없애는 안.
  `acquire_monitor_async_command_processing()` 이 `monitor.socket` 공개보다 **먼저** 실행되므로
  (`socket_base_monitor.cpp:329` vs `:358` 부근) 그 사이에 방출된 PAUSED 이벤트가 유실될 수 있어 새 경합을 만든다.
- (C) 기각: 타임아웃 확대. 경합을 가리기만 한다.
- (D) 기각: 제품 쪽에 "모니터가 커맨드 소유권을 갖지 않는" 테스트 전용 스위치 추가.
  새 규칙·제어점 추가라 POSDDD 에 어긋나고, 테스트 편의를 위해 런타임을 바꾸게 된다.

## 4. 검증

- `taskset -c 0` (기존 결정적 재현기): 단발 5/5 PASS, `--repeat until-fail:10` PASS
- `taskset -c 0,1 --repeat until-fail:40 --timeout 10`: 40/40 PASS
- `--repeat until-fail:30 --timeout 10`: 30/30 PASS
- `ctest -R 'flow|monitor|pair|ctx|term|random' --repeat until-fail:3`: 41/41 PASS

## 5. 분류·산출물

- 변경 분류: **B — 기존 결함**(테스트의 동기화 결함, f5d7cccde2 는 노출 계기일 뿐)
- 커밋하지 않음. 작업 트리 `~/project/zlink-work/lin1` 에 미커밋 상태로 둠.
- 패치: `~/project/zlink-work/all-artifacts/LIN-1.patch`
- 스펙 변경 없음. 공개 계약 테스트 기대값 변경 없음(이 파일은 core/tests/unittest 내부 테스트).

## 6. 남은 관찰(후속 후보, 이 job 범위 밖)

이 유닛 테스트가 실패하면 Unity 의 longjmp 로 fixture teardown 이 건너뛰어지고,
"Forced closure of N sockets" 뒤 컨텍스트 종료가 끝나지 않아 프로세스가 ctest 타임아웃까지 매달린다
(f5d7cccde2 와 그 부모 양쪽에서 동일). 실패가 FAIL 로 즉시 보고되지 않고 Timeout 으로 보고돼
CI 로그 해석을 어렵게 하므로, 별도 job 으로 다룰 가치가 있다.
