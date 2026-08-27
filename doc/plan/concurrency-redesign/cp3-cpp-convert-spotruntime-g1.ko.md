# CP3 C++ spotruntime G1 — Actor registry·실행 전환 STOP

## 결론

**STOP**. `Actor registry·실행`을 주 mutex에서 하나의 state lane으로만 옮기면
관측 가능한 Spot close/idle-eviction과 Actor join/destroy의 원자 경계가 갈라진다. 이 경계는
이번 패스에서 건드리지 않는 `Spot context·생성` 그룹도 같은 결정을 하므로, 대상 그룹만의
lock 제거는 C2를 lane과 mutex로 나누어 보호하게 된다. 스펙 06 §8의 교차 불변식/양방향
대기 금지에 해당한다.

source 변경은 없다. 전환 가능성을 확인하려고 추가했던 actor lane과 일부 접근의 부분 전환은
검증 전에 모두 되돌렸다. 이 보고서만 추가했다.

## 상태 그룹과 분류

| 그룹 | 분류 | 상태 | 이번 판정 |
|---|---|---|---|
| Builder 구성 | C2 | snapshot, factory, spot context 생성·삭제 | 주 mutex 유지 |
| Spot context·생성 | C2 | `spot_contexts_by_id`, `closed`, `actor_count`, idle close | Actor 그룹과 실제 교차 불변식 확인 |
| Actor registry·실행 | C2 | location/generation/fence, instance/index, route, queue snapshot, native actor | **STOP** |
| Relocation·handoff | C2 | coordinator, recovery, cleanup/terminal | 주 mutex 유지 |
| `route_client` | C1 | optional 등록·복사 조회 | 기존 `route_client_lane`, mutex 취득 0 |
| `stopping` | C3 | stop flag | 기존 atomic, mutex 취득 0 |

CP3 감사 기준 상태 보호 취득은 이전 C1 전환 후 **149 (.cpp 144 + .hpp 5) → 149**다.
Actor registry에는 별도 mutex가 없고 주 `recursive_mutex`의 임계 구역 안에서 다른 C2
그룹 상태와 함께 접근한다. 따라서 이 패스에서 Actor 전용 mutex 취득을 별도로 세어
"0으로 전환"할 수 없다. 이를 임의로 Actor 소유로만 분류하면 아래의 교차 구간을 빠뜨린다.

## STOP 근거

1. `spot_context_state_t::close_now()`는 주 mutex를 보유한 채 `closed || actor_count != 0`을
   한 결정으로 검사한다([spot_runtime.hpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp:495)).
   idle close도 같은 mutex 아래 `actor_count`, idle 조건, `closed`를 함께 검사한다
   ([spot_runtime.hpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp:650)).
   `actor_count`는 Actor join route를 기록하는 helper에서 증가한다
   ([spot_runtime.hpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp:769)),
   leave/destroy에서는 감소한다
   ([spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:1211)).
   따라서 Actor lane turn만으로 count를 바꾸면 close가 옛 count를 보고 close를 승인하거나,
   mutex를 든 close가 lane `.get()`을 기다리게 된다. 후자는 스펙 06 §5의 gate-보유 bridge
   조건을 만족하지 않는다. lane turn은 context close 경로의 주 mutex 접근을 피할 수 없다.

2. Actor 전이 자체가 context map의 현재성에 의존한다. `destroy_actor()`는 한 mutex turn에서
   generation/destroying 상태를 검사한 뒤 `actor_spot_ids`에서 context를 찾아 count를 줄이고,
   route·instance·queue snapshot·native-actor 관련 상태를 함께 제거한다
   ([spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:9912)).
   이 사이 `close_spot`/context detach가 map을 제거하지 못하게 하는 것이 현재 관측 순서다.
   context shared pointer를 먼저 snapshot해 Actor lane으로 넘기면 map-current와 close 승인 사이의
   원자성이 사라진다. 반대로 lane에서 map을 읽으면 Spot context 그룹의 잠금 없는 collection을
   읽게 된다.

3. join/move도 같은 형태다. `commit_accepted_actor_join_unlocked()`는 previous context count
   감소, actor route 제거, native actor 생성, context route 기록을 하나의 주 mutex turn에 둔다
   ([spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4497)).
   `leave_previous_actor_route()`는 외부 OnLeave callback을 위해 mutex를 풀고 다시 잡은 뒤 count와
   route를 정산한다([spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4454)).
   이는 발견 6의 turn A/callback/turn B 분리가 필요하다는 실증이며, turn A/B가 context close와
   동일한 ownership region을 가져야 한다.

4. 확정 재진입은 `admit_remote_actor_to_spot()`의 주 mutex 보유 상태
   ([spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6578))에서
   `actor_join_context_unlocked()`를 호출하고, 이 helper가 동적 Spot 생성을 위해 public
   `get_or_create_spot()`을 다시 호출하는 경로다
   ([spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4406)).
   이 재진입을 Actor lane으로 기계적으로 옮기면 lane이 같은 public 표면 재진입을 즉시 예외로
   끝낸다. private helper 분리만으로는 해결되지 않는다. context map/close 및 callback
   placeholder를 함께 소유하는 새 turn 경계를 설계해야 한다.

## 재진입·외부 callback 판정

- 재진입 실측: 위 4번의 `actor_join_context_unlocked()` → `get_or_create_spot()` 1개.
  `recursive_mutex`가 이를 현재 허용한다.
- callback: `leave_previous_actor_route()`는 이미 callback 동안 mutex를 해제한다. 하지만 callback
  전후의 count/route 정산은 context close와 원자여야 하므로 Actor-only lane으로 옮기면 안 된다.
- 발견 1·4: C++ lane은 thread-local 재진입 검출이며 AsyncLocal 상속 문제는 직접 적용되지 않는다.
- 발견 2: mutex를 보유한 close/join 경로가 Actor lane `.get()`을 기다리면 lane turn의 context
  접근과 순환할 수 있어 허용 bridge 조건을 충족하지 않는다.
- 발견 6·7: callback/serial dispatch는 lane 밖이어야 하며, 현재 mutex unlock/relock 프로토콜은
  state lane 대체가 아닌 callback 작업 프로토콜 경계다.
- 발견 9: join/destroy의 registration·context count·route 제거는 동기 반환/다음 관측 전 끝나야
  하므로 fire-and-forget 전환은 불가하다.
- 발견 10: location, generation, fence, route, instance/index, queue snapshot은 하나의 파생
  Actor 전이이므로 개별 lane turn으로 쪼개지 않았다.

## 블로킹 bridge와 스펙 06 §5

새 bridge **0개**. 임시 전환의 `.get()`은 모두 되돌렸다. 기존 state-lane bridge의 CP3 판정은
변경하지 않았다. 위 close/join 경로에 bridge를 새로 두는 것은 (1) 외부 gate 재획득 부재를
증명할 수 없고, (3) context-map 현재성과 actor count의 반환 전 원자성이 깨지므로 §5 불충족이다.

## 본문 조정·변경 파일

- 본문 조정: 없음.
- source 변경: 없음.
- 변경 파일: 이 보고서만.

## 검증

빌드는 지정 디렉터리에서 통과했다.

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
...
[100%] Built target test_cpp_framework_app_host
```

최종 label gate의 집계 원문:

```text
96% tests passed, 2 tests failed out of 45

Total Test time (real) =  48.22 sec

The following tests FAILED:
	13 - test_cpp_framework_host_lifecycle (Failed)
	19 - test_cpp_framework_layout_contract (Failed)
```

`test_cpp_framework_layout_contract`는 요청에서 지정된 기존 실패다.
`test_cpp_framework_host_lifecycle`은 같은 checkout에서 focused 재실행 1/1 통과했다.

```text
1/1 Test #13: test_cpp_framework_host_lifecycle ...   Passed   11.49 sec

100% tests passed, 0 tests failed out of 1
```

이번 패스는 source 동작을 남기지 않았으므로 두 full-run 실패를 이 작업의 회귀로 판정하지 않는다.

## 예상과 달랐던 점

선행 보고서는 Actor registry와 Spot context를 독립 C2 그룹으로 기록했지만, `actor_count`와
context close/idle eviction이 같은 주 mutex 아래서 Actor route 전이와 함께 결정된다. 이 경계를
보존하려면 최소한 Spot context lifecycle/close 조건까지 같은 ownership region으로 전환하거나,
그 경계를 보존하는 새 callback placeholder/close protocol을 먼저 설계해야 한다. 이는 이번
요청의 "다른 그룹은 건드리지 않음" 범위를 넘는다.
