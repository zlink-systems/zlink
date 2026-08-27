# CP3 C++ spotruntime state lane 전환 보고

## 결과

`spot_node_builder_state_t`의 handoff-request parking map을 독립 C2
ownership region으로 전환했다. `pending_handoff_requests`의 삽입, 만료,
정확한 terminal 취득·삭제는 이 map의 entry와 deadline, reply token, route/fence가
함께 유지하는 불변식이므로 map별 atomic/concurrent container로 분해하지 않았다.
`pending_handoff_requests_lane`이 그 전 영역을 FIFO로 소유한다.

변경 파일은 다음 셋이다.

- `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp`
- `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp`
- `doc/plan/concurrency-redesign/cp3-cpp-convert-spotruntime.ko.md`

## 상태 그룹과 분류

| 그룹 | 분류 | 소유/근거 | 이번 패스 |
|---|---|---|---|
| factory와 builder snapshot | C2 | factory type, lifecycle, relocation policy와 snapshot의 spot kind/execution 설정이 activation 결정을 함께 만든다. | 기존 `lane` 유지, 미전환 |
| Spot context와 spot ID/name/native Spot | C2 | context 등록/제거, name 양방향 index, native attachment, location claim이 함께 전이한다. | 미전환 |
| Actor location/generation/instance/queue/tombstone | C2 | actor route, instance index, generation, queue snapshot 및 destroy 상태가 같은 actor identity를 결정한다. | 미전환 |
| pending creation | C2 | reservation, future, context 생성과 factory type 검사가 하나의 exactly-one creation 전이다. | 미전환 |
| relocation/handoff coordinator 및 remote cleanup | C2 | authority fence, Message Follow, cleanup과 local actor registry가 commit/rollback 순서를 공유한다. | 미전환 |
| handoff request parking | C2 | pending entry의 deadline/reply token/source fence/route-id가 terminal 취득 또는 만료와 함께 전이한다. | **전용 lane 전환** |
| actor pending request 수 | C2 | actor별 in-flight 수의 증가·감소·transfer 시 sample을 함께 유지한다. | checkout 시작 시 이미 `actor_pending_requests_lane` 소유 |
| execution queue snapshot, `stopping`, 마지막 application 완료 시각 | C3 | copy-on-write shared_ptr publication 또는 단일 flag/counter다. | 기존 atomic 유지 |
| resolver map 단독 조회 | C1 | map 자체에는 교차 write가 없다. 다만 local Spot lookup과 fallback 순서를 한 번에 결정하는 호출은 현재 C2 context 경계에 남긴다. | 미전환 |

감사표 기준 `spot_runtime.cpp`은 실행 primitive 31, 상태 보호 147이고
`spot_runtime.hpp`는 상태 보호 5 취득이다. 실행 primitive 31과 socket/dispose
프로토콜 취득은 건드리지 않았다. 이번 C2 그룹의 상태 보호 취득은 **4 -> 0**이다
(`spot_runtime.cpp:526,10799,11329,11530`의 감사 시점 위치). 감사표에는 이미 lane으로
바뀐 `actor_pending_requests`의 옛 4 취득도 남아 있어, 표의 147을 현재 checkout의
직접 token 수로 재사용하지 않았다.

## 재진입과 lane 경계

1차에서 `spot_name_for()`의 중첩 public 호출은
`spot_name_for_unlocked()`으로 이미 해소되어 있다
(`spot_runtime.cpp:9842,9846,9975`). 이번 handoff lane의 네 turn은 map entry의
find/erase/expiry/insert와 coordinator의 동기 fence 조회만 수행하며, 같은 객체의 public
표면이나 외부 callback을 호출하지 않는다. 따라서 이 그룹에서 새 재진입은 0곳이다.

terminal을 고르는 두 read 묶음은 각각 하나의 lane turn에 그대로 남겼다.

- `spot_runtime.cpp:524-539`: pending key lookup, reply route와 parking-node fence 확인,
  entry 이동·erase.
- `spot_runtime.cpp:10802-10840`: 같은 key/route 확인, Message Follow source fence 확인,
  entry 이동·erase.

따라서 발견 10의 파생 capture를 여러 turn으로 쪼개지 않았다.

## 동기 호환 경계

새 `.run(...).get()`은 4개다(`spot_runtime.cpp:524,10802,11339,11543`). 모두 기존
동기 반환/terminal 등록 시점을 유지하기 위한 bridge이며 호출자 전파는 없다.

- lane work는 node mutex, socket gate, dispose gate를 다시 취득하지 않는다.
- work는 외부 await/callback을 포함하지 않는다. `state_lane_t`의 FIFO work는
  `std::future`로만 완료되고 이 호출부에는 inline dependent continuation API가 없다.
- pending 등록, terminal entry 이동/삭제와 deadline cleanup은 원래 호출이 반환하거나
  relay completion을 계속하기 전에 끝나야 한다.

그러므로 스펙 06 §5의 세 조건을 이 bridge에 대해 충족한다고 판정한다.

본문 조정은 두 terminal 조기 반환을 lane 반환값으로 옮기기 위한
`optional<pending_handoff_request_t>` adapter뿐이다. map lookup 조건, route/fence
검사, move/erase와 그 뒤 reply 동작은 변경하지 않았다. 나머지 두 turn의 본문은 lock
대신 lane wrapper를 두는 것 외에는 변경하지 않았다.

## 검증

빌드는 지정 경로 `framework/languages/cpp/build`에서 수행했다. 최초 실행 중 build tree가
다른 작업의 재생성으로 `/home/hep7/project/zlink-cppbase` source path를 사용한 흔적을
발견해 그 결과를 판정에 사용하지 않았다. 현재 checkout source를 가리키는
`CMakeFiles/zlink_framework.dir/.../spot_runtime.cpp.o`를 확인한 뒤 다시 빌드했다.

```text
$ flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
exit 0

$ flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
89% tests passed, 5 tests failed out of 45

The following tests FAILED:
	10 - test_cpp_framework_m6b_runtime (SEGFAULT)
	12 - test_cpp_framework_m6c_runtime (SEGFAULT)
	19 - test_cpp_framework_layout_contract (Failed)
	26 - test_cpp_framework_actor_gateway (Subprocess aborted)
	33 - test_cpp_framework_execution (SEGFAULT)
```

`test_cpp_framework_state_lane`은 통과했다. `test_cpp_framework_layout_contract`는 요청에
명시된 기존 실패다. 나머지 4개는 이번 turn에서 원인을 분리하지 못했으며 통과로
판정하지 않는다. `execution` 단독 재실행도 SEGFAULT였고, 86/134 종료가 아니어서
지정된 재실행 규칙은 적용하지 않았다.

## STOP과 다음 패스

STOP: **없음**. 관측 동작 변경이나 구조적 재진입 경계 재설계는 필요하지 않았다.

예상과 달랐던 점은 두 가지다. 첫째, 감사표의 147+5 수치는 현재 checkout보다 앞서며
`actor_pending_requests_lane` 전환이 이미 반영돼 있었다. 둘째, 공용 C++ build tree가
동시 재생성되어 stale source를 잠시 참조했으므로, source path와 object timestamp를
추가 확인해야 했다. 다음 패스는 factory/snapshot을 독립 lane으로 꺼내기보다 Spot
creation의 context-ID-pending invariant를 먼저 한 ownership region으로 정리하고,
external lifecycle callback은 발견 6/7 방식으로 turn 밖에 남겨야 한다.
