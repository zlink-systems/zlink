# CP3 C++ publichost 상태 소유권 전환 보고

## 결과

이번 패스에서는 `public_host_runtime_t::_next_operation` 한 개를 C3 원자 카운터로 전환했다.
`fetch_add`가 기존의 1부터 `uint64_t` 최댓값까지의 배정과 그 다음 호출의 exhausted 오류를
그대로 보존한다. C2 aggregate에는 lane과 mutex가 섞여 같은 불변식을 두 ownership mechanism이
동시에 지키는 상태가 되지 않도록 손대지 않았다.

변경 파일:

- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp`
- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp`
- 이 보고서

## 상태 그룹과 분류

| 그룹 | 분류 | 현재 소유 | 근거 |
|---|---|---|---|
| peer endpoint registry (`_peer_endpoints`) | C1 | `_peer_endpoint_lane` | connect 성공 뒤 등록과 disconnect 삭제만 하는 독립 map이다. 이번 패스 변경 없음. |
| route cache (`_spot_route_fences`) | C1 | `_route_cache_lane` | cache map과 만료 항목만 다룬다. 기존 bridge 5개 유지. |
| operation sequence (`_next_operation`) | C3 | `std::atomic<uint64_t>` | 다른 field/collection의 상태로 결정을 내리지 않는 단조 call-id 원천이다. 이번 패스에서 전환했다. |
| lifecycle/configuration (`_started`, `_closing`, callback/port 설정) | C2 | `_mutex`, 다음 패스 | start/close, 등록 거부, maintenance callback 관측 순서가 함께 결정된다. |
| local dispatch/completion (`_completions`, request/deadline index, dispatch deque) | C2 | `_mutex`, 다음 패스 | terminal claim, deadline index 삭제, queue 등록이 하나의 불변식이다. |
| Spot/Actor index (`_spots`, `_actors`) | C2 | `_mutex`, 다음 패스 | object generation/authority와 index 갱신을 함께 fence한다. |
| Session seal/journal | C2 | `_mutex`, 다음 패스 | seal terminal, journal terminal, response route와 local completion이 exact relocation key를 공유한다. |
| relocation assembly/target attempt | C2 | `_mutex`, 다음 패스 | assembly, attempt, authority fence, route-send claim과 retention terminal이 교차한다. |
| user-Spot terminal | C2 | `_mutex`, 다음 패스 | fingerprint/header/application terminal의 exact-once record다. |

## lock 계수와 개별 판정

CP3 감사 기준은 이 파일의 상태 보호 취득 99개다. 현재 checkout에는 기존 C1 peer-endpoint
전환으로 3개가 이미 빠져 있었고, 이번 C3 전환으로 `next_operation()`의 1개를 더 제거했다.
따라서 이번 패스의 직접 `_mutex` 취득은 **96 -> 95**, 감사 기준 누적은 **99 -> 95**다.

| 분류 | 취득 수 | 위치 또는 대상 | 판정 |
|---|---:|---|---|
| C1 | 0 | 주 `_mutex` 취득 없음 | peer endpoint/route cache는 이미 각 lane에 소유되어 있어 이번에 변경하지 않았다. |
| C2 | 95 | `877,1005,1037,1050,1063,1079,1088,1096,1110,1126,1147,1295,1314,1338,1349,1358,1367,1378,1394,1417,1520,1529,1552,1607,1642,1664,1737,1776,1945,2117,2225,2251,2259,2267,2281,2309,2321,2352,2366,2461,2566,2719,2757,2773,2793,2805,2822,2835,2870,2902,3116,3173,3203,3265,3286,3415,3428,3437,3837,3870,3887,4059,4076,4091,4131,4178,4310,4393,4863,4896,4940,5137,5157,5184,5214,5350,5396,5542,5786,5844,5927,5939,5995,6102,6135,6192,6226,6278,6313,6366,6393,6419,6443,6517,6533` | 상태 보호 잔존이다. 실행 primitive 및 socket·dispose 프로토콜 취득으로 재분류하거나 변경하지 않았다. |
| C3 | 1 -> 0 | 이전 `next_operation()` | `_next_operation`을 atomic `fetch_add(memory_order_relaxed)`로 교체했다. 반환한 `low == 0`이면 기존과 같이 `overflow_error`를 던진다. |

## 재진입·bridge·본문·발견 목록

- 재진입 실측: C3 전환은 lane turn을 만들지 않고 public 메서드 재호출도 없다. private unlocked helper가 필요하지 않았다.
- 블로킹 bridge: **0개 신규**. 기존 route-cache bridge 5개는 변경하지 않았다. 따라서 이번 C3 전환 자체에는 spec 06 §5 blocking-bridge 판정 대상이 없다.
- spec 06 §5: 기존 bridge는 CP3 감사의 세 조건(외부 lock 순환 없음, inline continuation 없음, sync return-before 사유 있음)을 그대로 충족한다. 이번 변경은 bridge를 추가하지 않아 그 판정에 영향을 주지 않는다.
- 본문 조정: C2 lock 본문은 **없음**. C3 `next_operation()`은 mutex critical section을 atomic increment와 exhausted 검사로 교체했다.
- 발견 1·4: C++에는 AsyncLocal 흐름 억제가 없어 해당 없음. 발견 2: 신규 bridge 없음. 발견 6: callback을 새 lane 안에서 호출하지 않았다. 발견 7: socket/dispose gate 및 외부 await를 건드리지 않았다. 발견 9: 반환 전 등록·캡처가 있는 표면을 비동기로 바꾸지 않았다. 발견 10: 연속 read capture 묶음을 분할하지 않았다.

## 다음 패스와 STOP

다음 패스는 C2를 한 mutex에서 기계적으로 떼지 않고, 위 표의 lifecycle/configuration,
local dispatch/completion, Spot/Actor index, Session seal/journal, relocation attempt, user-Spot
terminal 순서로 별도 lane 후보와 교차 불변식을 확정한다. callback/transport await를 품는 구간은
발견 6·7에 따라 state turn과 protocol gate를 분리한다.

STOP: **아니오**. 관측 동작 변경이나 구조적 재진입 경계 재설계 없이 독립 C3 전환을 완료했다.

예상과 달랐던 점: `_next_operation`은 CP3 L2 표에서 큰 completion aggregate에 함께 열거됐지만,
증가·wrap·exhausted 판정 외에는 그 aggregate를 읽거나 쓰지 않는 독립 C3 원천이었다.

## 빌드·테스트 결과

실행 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build --rerun-failed --output-on-failure
```

빌드: exit 0. 최종 출력은 `100% Built target test_cpp_framework_actor_gateway`였다.

CTest 집계 원문:

```text
98% tests passed, 1 tests failed out of 45

Total Test time (real) =  57.62 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

실패 재실행 집계 원문:

```text
50% tests passed, 1 tests failed out of 2

Total Test time (real) =  19.88 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

재실행에서 `test_cpp_framework_host_lifecycle`은 11.67초에 통과했다. 남은
`test_cpp_framework_layout_contract`는 지시된 기존 실패이며 ShoppingMall `OrderWorkflow/main.cpp`
350·446의 blocking `result()`를 지적한다.
