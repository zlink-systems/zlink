# CP3 C++ public_host_runtime g4 전환 보고

## 상태 그룹 판정

| 그룹 | 멤버 | 분류 | 근거 |
|---|---|---|---|
| g4-rs | `_relocation_target_attempts`, `_relocation_assemblies`, `_session_seal_terminals`, `_session_journal_terminals`, relocation session-route 상태와 관련 `_options`/`_sessions` 스냅샷 | C2 | 같은 relocation attempt key가 assembly 수명, S2 확정, command 44의 exactly-once 표식 및 seal/journal terminal 해제를 잇는다. session seal도 같은 relocation/session key의 ready·consumed와 route 처리를 함께 결정한다. |
| g4-user-terminal | `_user_spot_terminals`와 replay retention/capacity 옵션 | C2 | create/close의 같은 operation key에 대해 fingerprint 검증, expiry 제거, capacity 판정, terminal 저장이 하나의 exact-once 불변식이다. g4-rs key와 교차 불변식은 없다. |

따라서 클래스 경계는 유지하고 전용 lane 두 개를 추가했다. 기존 `_mutex`는 직접 호출자인 `maintenance_runtime.cpp`의 g3 configuration/maintenance 보호에만 남겼고, 대상 `.cpp`의 g4 상태 보호 취득에는 남기지 않았다.

## 파일별 결과

### `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp`

- lock 전/후: 주 `_mutex` 상태 보호 36/0.
- C1/C2/C3: 0/36/0. socket·dispose 작업 프로토콜 lock은 이 파일의 g4 대상에 없어서 변경하지 않았다.
- 재진입: g4 lane turn 안에서 같은 public surface를 부르는 호출은 확인되지 않았다. 기존 lock 구간을 짧은 state turn으로 유지했고, `try_finalize_relocation_target`·command 44 전송·transport reply/callback은 turn 밖에서 호출해 state lane 재진입을 만들지 않았다.
- 블로킹 bridge: 36개 `.run(...).get()` 표면. 원래 동기 lock 구간이 반환 전에 registration, exact-key 캡처, terminal claim을 끝내던 계약을 보존하는 호환 bridge다. §5 조건 판정은 충족: g4 turn은 외부 gate를 획득하지 않고, `state_lane_t`는 promise completion을 lane-current scope가 끝난 뒤 수행하며, 공개 동기 surface의 반환 전 상태 확정이 필요하다.
- 본문 조정: control-flow가 lambda 경계를 넘는 9곳에서 `continue`/`return`을 lane 결과 bool/상태값으로 바꿨다. 상태 판정·전이·오류 값은 유지했다. capacity 거절 및 relocation failure transport reply는 state turn 밖으로 이동했다.
- 발견 10: relocation prepare의 attempt identity+expiry, assembly identity+chunk accept, seal identity+ready/consumed 검증, user-terminal fingerprint+expiry/capacity는 각각 한 lane turn에서 함께 수행했다. 연속 read를 별도 turn으로 분리하지 않았다.
- 발견 6·7: transport reply, session completion 및 command 44 await는 lane 밖에 둔다. lane 안에서는 terminal placeholder/claimed state만 확정한다.

### `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp`

- `_relocation_session_terminal_lane` 및 `_user_spot_terminal_lane`과 각각의 executor를 추가했다.
- `_mutex`는 g4가 아닌 maintenance configuration 직접 호출자와의 기존 계약을 위해 유지한다.

### `framework/languages/cpp/framework/src/runtime/stateful/maintenance_runtime.cpp`

- 링크/선언 호환 확인만 수행했다. g4 상태에는 접근하지 않으며 수정 없음.

## 검증

빌드:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
[100%] Built target test_cpp_framework_contract_headers
```

집중 재현:

```text
test_cpp_framework_host_lifecycle ...   Passed   11.27 sec
100% tests passed, 0 tests failed out of 1
```

그 전에 같은 focused test는 두 차례 `Actor placement candidates were exhausted`로 실패했고, 마지막 재실행에서는 통과했다. 전체 label 실행은 45개 중 24개까지의 출력에서 `test_cpp_framework_m6b_runtime`(Subprocess aborted), `test_cpp_framework_host_lifecycle`(Failed), `test_cpp_framework_layout_contract`(알려진 기존 실패), `test_cpp_framework_actor_gateway`(Subprocess aborted)를 기록한 뒤 실행 수집 시간이 만료되어 최종 집계가 없다. exit 86/134 계열은 m6b focused 재실행을 한 번 시도했으나 수집 시간이 만료되어 결과를 얻지 못했다.

## STOP

STOP 아님. 관측 동작을 바꿔야 하는 요구나 구조적 재진입은 확인하지 못했다.

## 예상과 달랐던 점

처음에는 잔여 멤버를 하나의 g4 aggregate로 보았지만, `_user_spot_terminals`는 relocation/session key와 교차 불변식이 없어 별도 lane이 맞았다. 또한 `_mutex` 선언은 대상 `.cpp` 밖의 maintenance configuration 직접 호출자에도 남아 있어, g4 상태 lock 제거와 별도로 기존 gate를 유지해야 했다.
