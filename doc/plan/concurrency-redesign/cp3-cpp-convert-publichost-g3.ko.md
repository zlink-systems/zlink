# CP3 C++ public_host_runtime G3 전환 보고

## 결과

`public_host_runtime_t`의 C2 `lifecycle/configuration` 그룹을 전용
`_lifecycle_configuration_lane`으로 전환했다. 이 그룹의 주 mutex 취득은 **30 -> 0**이다.
남은 `_mutex` 36개는 local dispatch/completion, Spot/Actor index, session seal/journal,
relocation assembly/attempt, user-Spot terminal의 C2 상태 보호이며 이번 그룹의 상태를
읽거나 쓰지 않는다.

변경 파일:

- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp`
- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp`
- 이 보고서

## 상태 그룹과 경계

| 그룹 | 분류 | 소유자 | 경계 근거 |
|---|---|---|---|
| lifecycle/configuration | C2 | `_lifecycle_configuration_lane` | `_started`·`_closing`, 시작 전 구성 거부, callback/port 설정의 캡처, start/close와 maintenance callback 관측 순서를 함께 결정한다. |
| local dispatch/completion | C2 | 기존 `_mutex` + `_local_dispatch_completion_lane` | completion terminal, request/deadline index, dispatch deque의 삽입·삭제 불변식이다. |
| Spot/Actor index | C2 | 기존 `_mutex` + `_spot_actor_index_lane` | object generation/authority fence와 index 갱신을 함께 결정한다. |
| session seal/journal | C2 | 기존 `_mutex` | seal terminal, journal terminal, response route의 exact relocation key가 함께 움직인다. |
| relocation assembly/target attempt | C2 | 기존 `_mutex` | assembly, attempt, authority fence, route-send claim/retention terminal이 교차한다. |
| user-Spot terminal | C2 | 기존 `_mutex` | fingerprint/header/application terminal의 exact-once 기록이다. |
| peer endpoint/route cache | C1 | 기존 각 state lane | 독립 map이다. 이번 패스 변경 없음. |
| operation sequence | C3 | 기존 `std::atomic<uint64_t>` | 독립 call-id 원천이다. 이번 패스 변경 없음. |

`close()`의 session terminal map clear는 `_closing`이 먼저 진입을 막은 뒤 수행하는 독립
정리이며, lifecycle 값을 바탕으로 session terminal의 결과를 결정하지 않는다. 따라서
G3와 session seal/journal 사이에 교차 불변식은 발견하지 못했다.

## 파일별 전환

### `public_host_runtime.hpp`

- C1/C2/C3: C2 lifecycle/configuration에 전용 executor와
  `_lifecycle_configuration_lane`을 추가했다. C1/C3 변경 없음.
- lock 전후: 이 그룹은 기존 `_mutex` 30개, 전환 뒤 0개다.

### `public_host_runtime.cpp`

- C1/C2/C3: lifecycle/configuration 30개를 C2 lane turn으로 옮겼다. C1/C3 변경 없음.
- `start()`은 turn에서 transport 시작, `_started` 전이, maintenance 시작 callback 캡처만
  완료하고 callback은 turn 밖에서 호출한다. `close()`도 closing callback을 turn 밖에서
  호출한다.
- configuration snapshot은 여러 값을 한 번에 쓰는 `dispatch_user_spot_operations()`에서
  하나의 lane turn으로 유지했다. peer readiness, session-route owner 등 live callback
  snapshot도 각각 하나의 turn으로 캡처했다.

## 재진입, bridge, 본문

- 재진입 실측: 기존 `_mutex`는 non-recursive이며, lane turn 안에서 같은 객체의 public
  표면을 호출하는 경로는 찾지 못했다. `start()`의 maintenance callback은 재진입 가능
  외부 callback이므로 발견 6에 따라 상태 전이와 callback 캡처를 turn A에서 끝내고 turn
  밖으로 분리했다. private unlocked helper가 필요한 실제 public 재호출은 없었다.
- 새 blocking bridge: **30개**. 모두 기존 동기 public/configuration 표면 또는 반환 전에
  완료되어야 하는 local dispatch 등록/terminal 검사다. `run(...).get()`을 유지해 반환 전
  등록·캡처를 보존했다.
- spec 06 §5 판정: **충족**. (1) G3 turn은 기다리는 외부 mutex를 다시 얻지 않는다. 남은
  session mutex 정리는 G3 turn 완료 뒤에만 수행한다. (2) `state_lane_t::run`의
  promise/future 완료에는 C++ inline continuation 소유권 전파가 없다. (3) 동기 signature와
  start/close·등록 거부·등록 완료의 반환 전 관측 계약이 남아 있다.
- 본문 조정: `start()`의 maintenance 시작 callback만 발견 6에 맞춰 lane 밖 호출로 이동했다.
  나머지 전환은 기존 임계 구역의 상태 판정·전이·등록 본문을 같은 lane turn으로 감쌌다.

## 발견 목록 적용

- 발견 1·4: C++ state lane은 AsyncLocal/ExecutionContext를 전파하지 않는다. 이번 turn에서
  장기 timeout 작업을 시작하지 않았다.
- 발견 2: G3 bridge에서 lane work가 호출자가 보유한 mutex를 재취득하지 않는다.
- 발견 6: `start()`의 maintenance callback은 상태 전이 뒤 lane 밖에서 호출한다.
- 발견 7: socket/dispose 또는 외부 await 작업 프로토콜 gate는 변경하지 않았다.
- 발견 9: 등록 거부, completion erase/complete, local dispatch/deadline 등록은 모두
  `.get()`으로 caller 반환 전에 끝난다.
- 발견 10: `dispatch_user_spot_operations()`의 configuration 값 묶음과 각 callback snapshot은
  각각 한 lane turn 안에서 함께 캡처했다. 연속 read를 여러 turn으로 분리하지 않았다.

## STOP 및 예상과 달랐던 점

STOP: **아니오**. lifecycle/configuration 상태와 남은 C2 그룹 상태를 함께 결정하는
불변식 또는 구조적 재진입은 발견하지 못했다.

예상과 달랐던 점: start callback은 기존 mutex 안에서 직접 호출됐지만, state lane의
비재진입 규칙에서는 그대로 둘 수 없었다. callback 전에 `_started` 전이와 callback
캡처를 끝낸 뒤 밖에서 호출해 기존 lock 해제 뒤에 관측 가능한 상태를 그대로 보존했다.

## 빌드·테스트 결과

실행 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build --rerun-failed
```

빌드: exit 0. 최종 출력은 `100% Built target zlink_cpp_e2e_observability_ops_client`였다.

CTest 집계 원문:

```text
98% tests passed, 1 tests failed out of 45

Total Test time (real) =  59.89 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

실패 재실행 집계 원문:

```text
0% tests passed, 1 tests failed out of 1

Label Time Summary:
framework-contract    =   8.13 sec*proc (1 test)

Total Test time (real) =   8.13 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
```

남은 실패는 기존 알려진 `ShoppingMall/Server/OrderWorkflow/main.cpp` 350·446의 blocking
`result()` layout contract 위반이며 이번 허용 범위 밖이다.
