# CP3 C++ publichost G2 local dispatch/completion 전환 보고

## 결과

`public_host_runtime_t`의 C2 그룹 **local dispatch/completion**을 전용
`_local_dispatch_completion_lane`으로 전환했다. 이 lane은 `_completions`,
`_local_spot_requests`, `_local_spot_request_deadlines`,
`_local_application_dispatches`를 함께 소유한다. request 등록, deadline index 등록,
dispatch deque 등록과 terminal claim/index 삭제는 각각 하나의 lane turn 안에서 실행한다.

변경 파일:

- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp`
- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp`
- 이 보고서

## 상태 그룹과 분류

앞선 `cp3-cpp-convert-publichost.ko.md`의 3개 독립 C2 그룹 판정을 유지했다.

| 그룹 | 분류 | 이번 소유자 | 근거 |
|---|---|---|---|
| peer endpoint registry, route cache | C1 | 기존 각 lane | 단일 map이며 다른 그룹과 교차 불변식이 없다. 변경 없음. |
| operation sequence | C3 | 기존 `std::atomic<uint64_t>` | `_next_operation`은 독립 단조 원천이다. 변경 없음. |
| lifecycle/configuration | C2 | 기존 `_mutex` | start/close와 callback/configuration의 관측 순서를 함께 결정한다. 변경 없음. |
| **local dispatch/completion** | **C2** | **`_local_dispatch_completion_lane`** | completion terminal, request terminal claim, deadline index 삭제, dispatch deque 등록/제거가 같은 operation의 exactly-once 및 queue 상태를 함께 결정한다. |
| Spot/Actor index | C2 | 기존 `_spot_actor_index_lane` 및 `_mutex` 경로 | local dispatch/completion과 함께 결정되는 field/collection이 없다. 변경 없음. |
| session seal/journal, relocation attempt, user-Spot terminal | C2 | 기존 `_mutex` | 각 exact relocation/terminal 불변식이 local dispatch/completion과 교차하지 않는다. 변경 없음. |

경계 누수는 발견하지 못했다. lifecycle의 `_started`/`_closing` 확인만 기존 `_mutex`에
남겼고, 그 mutex를 보유한 bridge lane 항목은 local 상태만 접근하며 `_mutex`나 callback을
재획득하지 않는다. close는 먼저 `_closing`을 설정하고 local request terminal 처리를 lane에
FIFO로 제출하므로, 등록이 먼저 승인되면 종료 terminal이 그 뒤에 오고 종료가 먼저면 등록이
거절되는 기존 순서를 보존한다.

## lock 계수와 개별 판정

이번 작업 시작 시 이 그룹 field를 직접 보호하던 `_mutex` 취득은 14곳이었다. 전환 후
이 그룹을 직접 보호하는 `_mutex` 취득은 **0곳**이다. 파일 전체의 `_mutex` 취득은
**76 -> 66**으로 줄었다. 남은 66곳은 이번 그룹 밖 C2 또는 lifecycle 판단이며, 이 패스에서
그 상태를 수정하지 않았다.

| 분류 | 전 -> 후 | 위치/판정 |
|---|---:|---|
| 실행 primitive | 0 -> 0 | CP3 감사의 `public_host_runtime.cpp` 행을 존중했다. 해당 없음. |
| socket/dispose 프로토콜 | 0 -> 0 | 해당 없음. 외부 I/O gate를 변경하지 않았다. |
| C1 | 0 -> 0 | 기존 peer endpoint/route cache lane은 변경하지 않았다. |
| C2 local dispatch/completion 직접 보호 | **14 -> 0** | publish, close clear, dispatch dequeue, activity wait, completion reserve/complete/erase, local Actor/Spot enqueue, request terminal/expiry/shutdown/deadline, operation complete의 group field 접근을 모두 lane으로 옮겼다. |
| C2 lifecycle bridge mutex | 6 -> 6 | `_started`/`_closing` 확인만 유지한다. lane lambda는 `_mutex`를 재취득하지 않는다. |
| C3 | 0 -> 0 | 기존 `_next_operation` atomic 유지. |

## 재진입, bridge, 본문 조정

- 재진입 실측: G2 lane turn 안에서 같은 `public_host_runtime_t` public 메서드를 다시 호출하는 자리는 없었다. 기존 L2 조사도 대상의 non-recursive `_mutex` 중첩이 없다고 판정했다. 따라서 private unlocked helper를 추가할 자리는 없었다.
- 블로킹 bridge: **20개 syntactic `.run(...).get()`**. 동기 public 표면의 반환 전 reservation, completion terminal claim, request/deadline 등록, queue dequeue/empty 판독을 끝내야 하므로 유지했다. 이 중 lifecycle `_mutex`를 보유하는 제출 표면은 completion enqueue와 local Actor/Spot send/request enqueue 4개다.
- spec 06 §5 판정: 충족. lane 항목은 `_mutex`를 재획득하지 않고, C++ `std::future`에는 Java식 inline dependent continuation이 없으며, `state_lane_t`는 lane-current 표시를 thread-local로만 유지한다. bridge는 sync public signature와 반환 전 등록/캡처 보존 사유가 있다.
- 본문 조정: C2 상태 전이 자체는 변경하지 않았다. `dispatch_ready()`의 빈 deque `break`만 lambda 안에서는 바깥 loop를 탈출할 수 없으므로 lane 밖 `optional` 검사로 등가 변환했다. 세 local enqueue의 `signal_activity()`는 성공한 state turn 뒤, 같은 lifecycle mutex 범위에서 실행하도록 lane 밖으로 옮겼다. wake 순서와 성공/실패 조건은 유지했고, transport wake/callback을 state turn에 넣지 않았다.

## 발견 목록 적용

- 발견 1: C++에는 AsyncLocal/ExecutionContext 흐름이 없고, 이번 turn에서 장기 작업을 시작하지 않았다.
- 발견 2: mutex 보유 bridge는 lane lambda가 `_mutex`를 재획득하지 않고 completion에 inline continuation이 없음을 source로 확인했다.
- 발견 4: 동기 prefix를 가진 장기 async 작업을 lane에서 시작하지 않았다.
- 발견 6: `dispatch`와 `spot_request_completion_t` callback은 terminal/dequeue state turn이 끝난 뒤 호출한다.
- 발견 7: socket/dispose/외부 await gate를 변경하지 않았다. `signal_activity()`는 state turn 밖의 동기 wake로 남겼다.
- 발견 9: `reserve`, request/deadline/deque 등록, terminal claim과 deadline 판독을 모두 `.get()`으로 반환 전에 완료한다.
- 발견 10: request map 삽입, deadline index 삽입, dispatch deque 삽입을 한 turn으로 묶었다. dequeue와 해당 request의 queued/terminal 판정도 한 turn으로 묶었다.

## STOP

**아니오.** local dispatch/completion과 다른 두 C2 그룹이 함께 결정되는 실제 위치는 없었다. 관측 순서, timeout, 오류 코드를 바꾸지 않고 전환했다.

## 예상과 달랐던 점

전체 CTest 순서에서는 `test_cpp_framework_host_lifecycle`가 remote Actor placement 후보 소진으로
간헐 실패했고, 단독 재실행은 통과했다. 이 테스트는 이번 G2의 local dispatch/completion
전환과 직접 같은 failure path가 아니며, 원인은 현재 확정하지 않았다. known failure인 layout
계약과 별개로 남은 검증 제한으로 기록한다.

## 빌드·테스트 결과

실행 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build --rerun-failed --output-on-failure
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -R '^test_cpp_framework_host_lifecycle$' --output-on-failure
```

빌드: exit 0. 최종 출력은 `100% Built target zlink_cpp_cross_language_host`였다.

최종 필터 CTest 집계 원문:

```text
96% tests passed, 2 tests failed out of 45

Total Test time (real) =  47.49 sec

The following tests FAILED:
	 13 - test_cpp_framework_host_lifecycle (Failed)
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

실패 재실행 원문:

```text
remote Actor create completion must reach the source after target creation: route-ready=1 created-callback=0 joined-callback=0 created=0 authority-active=0 error=Actor placement candidates were exhausted

sample server/shared code must use task_t await or callback completion instead of blocking result(): "/home/hep7/project/zlink/framework/languages/cpp/samples/ShoppingMall/Server/OrderWorkflow/main.cpp":350
sample server/shared code must use task_t await or callback completion instead of blocking result(): "/home/hep7/project/zlink/framework/languages/cpp/samples/ShoppingMall/Server/OrderWorkflow/main.cpp":446
```

`test_cpp_framework_host_lifecycle` 단독 재실행 집계 원문:

```text
100% tests passed, 0 tests failed out of 1

Total Test time (real) =  11.39 sec
```

`test_cpp_framework_layout_contract`는 지시된 기존 ShoppingMall blocking `result()` 실패다.
