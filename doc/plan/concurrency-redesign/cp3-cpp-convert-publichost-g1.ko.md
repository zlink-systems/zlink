# CP3 C++ publichost G1 Spot/Actor index 전환 보고

## 결과

`public_host_runtime_t`의 C2 Spot/Actor index 그룹을 전용
`_spot_actor_index_lane`으로 전환했다. `_spots`와 `_actors`는 이제 평범한
`std::map`으로 남고, 이 두 map의 조회·generation/authority 비교·삽입·삭제·갱신은 모두
같은 FIFO lane turn에서만 실행한다. 이 그룹을 보호하던 `_mutex` 취득은 남지 않았다.

변경 파일:

- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp`
- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp`
- 이 보고서

git 명령은 작업 지시에 따라 실행하지 않았다.

## 상태 그룹과 경계

| 그룹 | 분류 | 소유자 | 근거와 이번 처리 |
|---|---|---|---|
| peer endpoint registry (`_peer_endpoints`) | C1 | 기존 `_peer_endpoint_lane` | 독립 map이다. 변경하지 않았다. |
| route cache (`_spot_route_fences`) | C1 | 기존 `_route_cache_lane` | 독립 cache와 만료 항목이다. 변경하지 않았다. |
| operation sequence (`_next_operation`) | C3 | 기존 atomic | 단조 call-id 원천이다. 변경하지 않았다. |
| lifecycle/configuration | C2 | `_mutex` | start/close와 callback 설정의 관측 순서를 함께 결정한다. 변경하지 않았다. |
| local dispatch/completion | C2 | `_mutex` | completion, request/deadline index, dispatch deque가 교차한다. 변경하지 않았다. |
| **Spot/Actor index (`_spots`, `_actors`)** | **C2** | **신규 `_spot_actor_index_lane`** | object generation/authority와 type/index 갱신을 같은 turn으로 fence한다. **전부 전환했다.** |
| Session seal/journal | C2 | `_mutex` | relocation key와 terminal/response route가 교차한다. 변경하지 않았다. |
| relocation assembly/target attempt | C2 | `_mutex` | assembly, attempt, authority fence, route-send claim이 교차한다. 변경하지 않았다. |
| user-Spot terminal | C2 | `_mutex` | exact-once terminal record다. 변경하지 않았다. |

Spot/Actor index가 다른 C2 그룹 상태와 함께 결정되는 `_mutex` 블록은 발견하지 못했다.
특히 relocation target attempt의 finalization은 actor index 갱신 turn 뒤에도 별도의
`_relocation_target_attempts` turn으로 남으며, 원래도 서로 다른 임계 구간이었다.

## lock 계수와 분류

CP3 감사 기준의 이 파일 상태 보호 취득은 99곳이었다. 앞선 패스가 C1 peer endpoint
3곳과 C3 operation sequence 1곳을 제거하여 이번 시작 시 주 `_mutex` 취득은 95곳이었다.
이번 G1은 Spot/Actor index **20곳을 20개 lane turn으로 전환**하여 주 `_mutex` 취득을
**95 -> 75**로 줄였다. `_spots`·`_actors` 접근 주변에 `_mutex` 취득이 남지 않는 것을
정적 검색으로 확인했다.

| 분류 | 전 | 후 | 위치/판정 |
|---|---:|---:|---|
| C1 | 0 | 0 | 이미 전용 lane인 peer endpoint와 route cache는 변경하지 않았다. |
| C2 Spot/Actor index | 20 | 0 | `.cpp:1005,1297,1321,2234,2264,2270,2282,2299,2330,2341,2375,3287,5163,5375,5807,5952,5967,6165,6549,6568`; 모두 `_spot_actor_index_lane.run(...).get()`으로 전환했다. |
| C2 다른 상태 그룹 | 75 | 75 | lifecycle/configuration, local dispatch/completion, session, relocation, user-Spot terminal. 범위 밖이라 유지했다. |
| C3 | 0 | 0 | `_next_operation`은 기존 atomic이다. |
| 실행 primitive / socket·dispose protocol | 0 | 0 | CP3 감사표의 이 파일 행은 `0 / 0 / 99 / 0`이며, 해당 취득을 새로 분류하거나 변경하지 않았다. |

## 재진입, bridge와 본문

- 재진입 실측: 전환한 20개 turn 안에서 같은 `public_host_runtime_t` public 표면을 다시
  호출하는 경로는 없었다. index turn은 map 연산, 순수 `framework_actor_ref()` 생성, 또는
  별도 `stateful_object_runtime_t`의 generation/authority 연산만 호출한다. 따라서 private
  unlocked helper가 필요한 재진입은 없었다. `state_lane_t`의 재진입 예외는 그대로
  방어선으로 남는다.
- 블로킹 bridge: **신규 20개**다. 기존 동기 public 표면의 반환 전 lookup·등록·generation
  fence를 보존하기 위해 `.get()`을 사용했다. 호출자 전파나 허용 파일 밖 변경은 없었다.
- 스펙 06 §5 판정: **충족**. 각 제출 항목은 `_mutex`, socket/dispose gate 또는 session lane을
  재획득하지 않는다. C++ `std::future`에는 dependent continuation API가 없으므로 promise
  완료가 lane worker에서 inline caller continuation을 실행하지 않으며, `.get()` 호출자는
  자신의 대기 thread에서 재개된다. 동기 표면은 기존처럼 반환 전에 index 판독·등록·갱신을
  완료해야 한다.
- 본문 조정: 상태 전이·순서·오류 코드는 변경하지 않았다. 함수 반환이 lock 블록 안에 있던
  `destroy_application_actor`, `get_or_create_spot`, `create_actor`,
  `create_reserved_actor`, `try_finalize_relocation_target`에서는 lane 결과를 바깥에서
  반환하도록 기계적으로 전달했다. 이는 lambda의 `return` 범위만 보정한 것이며, 원래
  조건·map 연산·오류 결과는 유지했다.

## 발견 목록 적용

- 발견 1·4: C++에는 AsyncLocal 흐름 억제가 없고, 이번 turn에서 timer/retry/background
  작업을 시작하지 않았다.
- 발견 2: bridge 20개는 위 §5 세 조건을 충족한다. 외부 gate를 가진 상태로 같은 gate를
  재획득하는 lane 항목은 없다.
- 발견 6: 새 lane turn 안에서 외부 callback을 호출하지 않았다.
- 발견 7: socket/dispose 및 외부 await를 품는 protocol gate는 변경하지 않았다.
- 발견 9: lookup, index 등록, generation/authority 캡처는 모두 `.get()`으로 호출 반환 전에
  끝난다.
- 발견 10: `_actors`에서 type과 object ref를 함께 읽는 자리는 하나의 index turn으로
  유지했다. 특히 `resolve_actor()`의 generation 비교와 object 반환, relocation target의
  기존 generation/authority 비교와 insert, session resolver의 actor lookup은 각각 분할하지
  않았다.

## STOP과 예상 차이

STOP: **아니오**. 관측 동작 변경이나 구조적 재진입 경계 재설계가 필요하지 않았고,
Spot/Actor index와 다른 상태 그룹이 함께 결정되는 누수도 발견하지 못했다.

예상과 달랐던 점: map 접근은 20개였지만, 호출자 전파 없이 모두 기존 동기 표면에서
return-before 보장을 지켜야 했다. 따라서 이 패스의 lane 도입은 모두 blocking bridge가
되었다.

## 빌드·테스트 결과

실행 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build --rerun-failed --output-on-failure
```

빌드: exit 0. 최종 출력은 다음과 같다.

```text
[100%] Built target test_cpp_framework_m6c_runtime
```

첫 전체 실행의 실패 목록에는 layout contract와 messaging이 있었고, failed-only 재실행에서
messaging은 통과했다. 재실행 집계 원문은 다음과 같다.

```text
50% tests passed, 1 tests failed out of 2

Label Time Summary:
framework-contract      =   8.15 sec*proc (1 test)
framework-regression    =   0.06 sec*proc (1 test)
framework-unit          =   0.06 sec*proc (1 test)
messaging               =   0.06 sec*proc (1 test)

Total Test time (real) =   8.21 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

messaging 재확인 뒤 전체 범위를 한 번 더 실행해 보존한 최종 집계 원문은 다음과 같다.

```text
98% tests passed, 1 tests failed out of 45

Total Test time (real) =  57.44 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

남은 `test_cpp_framework_layout_contract` 실패는 기존 알려진 실패이며
`samples/ShoppingMall/Server/OrderWorkflow/main.cpp:350,446`의 blocking `result()` 검사다.
이번 허용 범위와 무관하며 변경하지 않았다.
