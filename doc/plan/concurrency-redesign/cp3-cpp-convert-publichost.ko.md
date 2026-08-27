# CP3 C++ publichost 상태 lane 전환 보고

## 결과

`public_host_runtime_t`의 독립 C1 peer endpoint 레지스트리를 별도 state lane으로 전환했다.
주 `_mutex` C2 aggregate를 C1 map으로 쪼개지 않았고, socket·dispose/실행 primitive도 건드리지
않았다. 이번 패스는 안전한 독립 그룹만 끝낸 부분 전환이다.

변경 파일:

- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp`
- `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp`
- 이 보고서

## 상태 그룹과 근거

| 그룹 | 분류 | 소유 primitive | 근거와 이번 판정 |
|---|---|---|---|
| peer endpoint registry (`_peer_endpoints`) | C1 | 새 `_peer_endpoint_lane` | map 하나의 connect 성공 뒤 등록, transport disconnect 전/후 삭제만 수행한다. 다른 field/collection과 함께 결정하는 불변식이나 map 외 read가 없다. |
| lifecycle/configuration (`_started`, `_closing`, configure callback/port fields) | C2 | 기존 `_mutex`, 다음 패스 | start/close와 callback 설치의 순서 및 lifecycle 전이가 결합한다. |
| completion/local Spot (`_completions`, request/deadline index, dispatch deque, `_spots`, `_actors`, `_next_operation`) | C2 | 기존 `_mutex`, 다음 패스 | completion terminal, deadline 제거, queued/terminal claim, Spot/Actor exact generation이 함께 바뀐다. |
| Session seal/journal | C2 | 기존 `_mutex`, 다음 패스 | seal terminal, journal terminal, local completion 및 response route가 exact session relocation identity를 공유한다. |
| relocation assembly/target attempt | C2 | 기존 `_mutex`, 다음 패스 | assembly, attempt, authority fence, route send claim과 retention/terminal이 교차한다. |
| user-Spot terminal | C2 | 기존 `_mutex`, 다음 패스 | request fingerprint/header/application terminal의 exact-once 결과가 lifecycle과 요청 처리에서 쓰인다. |
| scalar C3 후보 | C3 | 없음 | `_next_operation`, `_started`, `_closing`은 각자 C2 lifecycle/operation 결정에 참여하므로 이번 패스에서 원자화하지 않았다. |
| route cache (`_spot_route_fences`) | C1 | 기존 `_route_cache_lane` | 이미 독립 lane 소유다. 이번 변경 없음. |

## 파일별 lock 계수·개별 분류

| 파일 | 전 | 후 | C1 | C2 | C3 |
|---|---:|---:|---|---|---|
| `runtime/stateful/public_host_runtime.cpp` 상태 보호 잔존 | 99 | 96 | 전환 3: 기존 L1169, L1198, L1211 | 잔존 96: L877,1005,1037,1050,1063,1079,1088,1096,1110,1126,1147,1295,1314,1338,1349,1358,1367,1378,1394,1417,1520,1529,1552,1607,1642,1664,1737,1776,1945,2117,2225,2251,2259,2267,2281,2309,2321,2352,2366,2461,2566,2719,2757,2773,2793,2805,2822,2835,2870,2902,3116,3173,3203,3265,3286,3415,3428,3437,3837,3870,3887,4059,4076,4091,4131,4178,4310,4393,4863,4896,4940,5137,5157,5184,5214,5350,5396,5542,5786,5844,5927,5939,5974,5997,6104,6137,6194,6228,6280,6315,6368,6395,6421,6445,6519,6535 | 0 |
| `runtime/stateful/public_host_runtime.hpp` | 1 mutex declaration | 동일 | 새 C1 executor/lane과 map field | 주 C2 `_mutex` 선언 유지 | 0 |

CP3 감사표의 대상 행(실행 primitive 0 / socket·dispose 0 / 상태 보호 99)을 기준으로 했다.
따라서 전환하지 않은 96개는 모두 C2 상태 보호 잔존이며, 실행 primitive나 socket·dispose
프로토콜로 재분류하지 않았다.

## 구현·재진입·관측 보존

- 헤더에 `_peer_endpoint_lane_executor`, `_peer_endpoint_lane`을 추가했다. map을 lane보다 앞에
  배치해 소멸 시 lane이 먼저 닫히고 mailbox가 map 수명 안에서 drain되게 했다.
- `connect_peer`의 성공 뒤 map 등록, `disconnect_peer(endpoint)`의 transport 호출 전 map 삭제,
  expected-RID overload의 transport 결과 뒤 조건부 map 삭제를 각각 하나의 lane turn으로 옮겼다.
  각 원래 임계구역의 본문은 변경하지 않았다.
- 재진입 실측: C1 lane turn 안에서 같은 `public_host_runtime_t` public 메서드를 호출하는 경로는
  없다. 세 turn은 map `insert_or_assign` 또는 `erase`만 수행한다. private unlocked helper는 필요하지
  않았다.
- 발견 10: 해당 없음. C1 turn은 map 한 연산뿐이며, 여러 read가 하나의 파생 값을 만드는 capture
  block을 분할하지 않았다.
- 발견 1·4: C++ lane은 AsyncLocal execution-context를 쓰지 않아 해당 없음. 발견 2: C++ future
  `.get()`에는 inline continuation이 없어 해당 없음. 발견 6: external callback을 lane 안에서 호출한
  자리가 없다. 발견 7: transport operation은 lane 안에 넣지 않았고 protocol gate도 변경하지 않았다.
  발견 9: 동기 표면의 반환 전 map 등록/삭제를 `.get()`으로 보존했다.

## 블로킹 bridge와 스펙 06 §5 판정

새 bridge는 3개다: `public_host_runtime.cpp:1169-1174`, `:1200`, `:1211`.
사유는 각 동기 public surface가 반환하거나 바로 다음 transport operation으로 진행하기 전에 원본
등록/삭제를 끝냈던 return-before 계약을 유지하기 위해서다.

스펙 06 §5 조건은 모두 충족한다.

1. lane 항목은 `_mutex`, socket gate 또는 외부 gate를 재획득하지 않고 map만 만진다.
2. `state_lane_t::run`의 C++ promise/future 완료는 inline caller continuation을 실행하지 않는다.
3. 공개 동기 signature와 원래의 등록·삭제 순서를 유지해야 한다.

기존 route-cache bridge 5개(`:1344,1354,2383,2411,2433`)는 변경하지 않았다.

## 본문 조정·다음 패스·STOP

본문 조정 목록: **없음**. lock body는 그대로 두고 그 바깥에 lane turn만 추가했다.

다음 패스는 위 표의 C2 그룹별로 callback/transport boundary와 public-to-public 재진입을 먼저
private unlocked helper로 분리한 뒤 진행한다. 특히 `start()`의 transport/maintenance callback,
local request terminal/dispatch, relocation completion은 같은 C2 turn 안에 보존해야 한다.

STOP: **아니오**. 관측 동작을 바꾸지 않고 완결 가능한 C1 그룹을 전환했다.

예상과 달랐던 점: 기존 보고서의 “전부 또는 STOP” 전제와 달리, peer endpoint map은 주 C2 aggregate와
교차 불변식이 없는 독립 C1 ownership region이었다. 따라서 전체 99개를 한 lane으로 옮기지 않아도
안전한 진전이 가능했다.

## 빌드·테스트 결과

실행 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build --rerun-failed --output-on-failure
```

빌드: exit 0, `100% Built target test_cpp_framework_store_location_resolvers`.

첫 CTest 집계 원문:

```text
89% tests passed, 5 tests failed out of 45
Total Test time (real) = 54.01 sec
The following tests FAILED:
  10 - test_cpp_framework_m6b_runtime (SEGFAULT)
  12 - test_cpp_framework_m6c_runtime (SEGFAULT)
  19 - test_cpp_framework_layout_contract (Failed)
  26 - test_cpp_framework_actor_gateway (Subprocess aborted)
  33 - test_cpp_framework_execution (SEGFAULT)
```

실패 1회 재실행 집계 원문:

```text
0% tests passed, 5 tests failed out of 5
Total Test time (real) = 12.67 sec
The following tests FAILED:
  10 - test_cpp_framework_m6b_runtime (SEGFAULT)
  12 - test_cpp_framework_m6c_runtime (SEGFAULT)
  19 - test_cpp_framework_layout_contract (Failed)
  26 - test_cpp_framework_actor_gateway (Subprocess aborted)
  33 - test_cpp_framework_execution (SEGFAULT)
```

`test_cpp_framework_layout_contract`는 지시된 알려진 기존 실패이며, 재실행 출력은 ShoppingMall
sample의 blocking `result()` 두 위치를 지적했다. 나머지 네 실패도 재현됐지만 이번 변경의 C1
peer-registry 세 turn과의 직접 원인은 확인하지 못했으므로 기존 실패로 단정하지 않는다.
