# CP3 C++ 상태 보호 잔존 전환 — batchB

## 결과

- 대상 상태 보호 취득 **45개를 0개**로 전환했다. `location_runtime.hpp`의
  heartbeat condition-variable 취득 1개는 CP3 분류상 실행 primitive여서 그대로 뒀다.
- 컬렉션은 모두 평범한 `std::map`으로 유지했다. C2를 collection별 concurrent
  자료구조로 분해한 곳은 없다.
- 직접 호출자와 test 기대값은 바꾸지 않았다. `service_descriptor_registry.hpp`는
  `.cpp` 대상의 mutex 선언과 lane 소유 필드가 있는 직접 선언 헤더라 함께 바꿨다.

## 파일별 전환

### `runtime/locations/location_lifecycle.hpp`

- lock 전/후: **13 / 0**
- 분류: **C2 13, C1 0, C3 0**. `active`와 actor/spot map, actor claim의
  generation·deactivation 수명이 함께 결정된다.
- 재진입 실측·해소: public-to-public 재호출은 찾지 못했다. `deactivate_actor`와
  `deactivate_all`은 actor 제거/claim 수집만 lane turn에서 끝내고 callback은 turn 밖에서
  호출한다(발견 6).
- 블로킹 브리지: **13** (public 동기 surface 10, destructor/private callback 보조 3).
  기존 반환 전 actor/spot claim·제거·count 관찰을 보존한다.
- 본문 조정: 없음. 원본에서 분리돼 있던 `update_actor_location`과 `release_spot`의 두
  lock 구간도 각각 별 lane turn으로 유지했다.
- 발견 10: `release_actor`의 한 lock 안 find→state-change 묶음은 한 turn으로 유지했다.
  원본부터 분리된 두 lock 구간은 기계적으로 합치지 않았다.

### `runtime/locations/location_runtime.hpp`

- lock 전/후: 상태 보호 **12 / 0**; `_heartbeat_gate` 실행 primitive **1 / 1**.
- 분류: 상태 보호 **C2 12, C1 0, C3 0**. lease token, health, renewed-at,
  last-error, renew metric epoch가 같은 owner lifecycle을 이룬다.
- 재진입 실측·해소: 같은 객체 public 표면을 lane 안에서 호출하는 경로는 없었다.
  heartbeat wait는 turn 밖에서 끝난 뒤 renew를 호출하므로 lane 재진입이 없다.
- 블로킹 브리지: **12**. 동기 lease claim/release/renew 및 상태 등록·캡처가 반환 전에
  끝나야 한다.
- 본문 조정: 없음.
- 발견 10: renew lateness의 이전 timestamp read와 그에 따른 상태 갱신은 각각 원본의
  독립 lock 구간이었다. 한 lock 구간 안의 복수 read를 분할한 곳은 없다.

### `runtime/locations/store_location_resolvers.hpp`

- lock 전/후: **8 / 0**.
- 분류: **C1 1, C2 7, C3 0**. `actor_location_observer_t::_generations`는 단일
  map C1이고, resolver route cache는 spot/actor map과 recovery generation을 함께
  보존하는 C2다.
- 재진입 실측·해소: 없음. cache mutation은 store call과 분리된 동기 state turn이다.
- 블로킹 브리지: **9** (lock 전환 8 + 무잠금 `set_actor_mesh_name` 1). setter도 같은
  lane 소유로 넣어 무잠금 state write가 남지 않게 했다.
- 본문 조정: 없음.
- 발견 10: cache hit의 expiry/recovery-generation 검사와 erase/return은 한 turn으로
  유지했다.

### `runtime/locations/in_memory_store_providers.hpp`

- lock 전/후: **7 / 0**.
- 분류: **C1 4, C2 3, C3 0**. relocation blob map의 put/read/renew/erase는 C1;
  location store의 value map, scan snapshot, version/epoch/cursor 전이는 C2다.
- 재진입 실측·해소: 없음. 모든 provider surface가 동기이고 public 메서드 상호 호출이 없다.
- 블로킹 브리지: **7**. Store 계약상 condition check, mutation, scan snapshot/cursor
  생성이 task 반환 전에 이미 완료돼야 한다.
- 본문 조정: 없음.
- 발견 10: `scan`의 cursor decode→snapshot lookup→page materialization 전체를 한
  turn에 유지했다.

### `runtime/locations/service_descriptor_registry.cpp`

- lock 전/후: **5 / 0**.
- 분류: **C2 5, C1 0, C3 0**. record revision/change stamp와 watcher set/watch id의
  교차 불변식이 있다.
- 재진입 실측·해소: `publish`/`remove`는 callback 후보와 event를 lane에서 capture한 뒤,
  `notify`는 turn 밖에서 실행한다. callback이 public registry API를 재호출해도 lane
  재진입이 되지 않는다(발견 6).
- 블로킹 브리지: **5**. publish/remove 결과, snapshot, watcher 등록/해제의 동기
  signature와 반환 전 등록 계약을 유지한다.
- 본문 조정: callback dispatch만 기존 lock 밖 위치를 보존하도록 lane 밖에 명시적으로
  남겼다.
- 발견 10: snapshot의 change stamp와 filtered record vector를 한 turn에서 만들었다.

## 블로킹 호환 경계와 스펙 06 §5

총 **46개** `.run(...).get()`을 뒀다(상태 보호 취득 45개는 원래 lock 구간별로 각각
submission을 유지했고, 무잠금 setter 1개를 같은 lane에 추가했다).

1. 외부 gate 순환: 충족. 각 bridge는 state mutex를 제거한 뒤 호출하며, 남은
   `_heartbeat_gate`를 들고 lane을 기다리는 경로는 없다.
2. completion continuation: 충족. `state_lane_t::run`은 promise를 완료할 뿐이고 C++
   `std::future::get()`은 dependent continuation을 inline 실행하지 않는다.
3. 동기 계약 사유: 충족. Store mutation/scan, resolver cache invalidation, descriptor
   watch 등록 및 location lease/claim의 반환 전 완료가 기존 동기 관찰 계약이다.

## 발견 목록 적용

- (1), (4): 대상 lane turn에서 timeout/retry/background task를 새로 시작하지 않았다.
- (2): gate를 들고 bridge를 기다리는 새 경로가 없다.
- (6): lifecycle/decriptor callback은 turn 밖으로 뒀고 상태 전이·event capture는 먼저
  완료한다.
- (7): heartbeat condition-variable gate는 작업/실행 primitive로 유지했고 lane 내부에서
  획득하지 않는다.
- (9): 모든 기존 동기 등록·캡처는 `.get()`으로 반환 전에 완료한다.
- (10): 하나의 lock 구간의 read/capture 묶음을 별 lane turn으로 분해하지 않았다.

## 검증

실행 build tree: `framework/languages/cpp/build`.

```text
cmake --build framework/languages/cpp/build -j8
실패: runtime/mesh/service_liveness_registry.cpp:42,54,69,90,102,131,143:
_mutex was not declared in this scope
```

이는 허용 범위 밖 `service_liveness_registry.hpp`가 이미 `_mutex` 대신 lane field를
선언한 상태인데 cpp가 아직 `_mutex`를 참조해서 생긴 build blocker다. 대상 source를
바꾸지 않고도 확인한 개별 object/헤더 컴파일은 통과했다.

```text
CMakeFiles/zlink_framework_m6a_objects.dir/framework/src/runtime/locations/service_descriptor_registry.cpp.o: PASS
CMakeFiles/test_cpp_framework_location_lifecycle.dir/tests/Zlink.Framework.UnitTests/test_cpp_framework_location_lifecycle.cpp.o: PASS
CMakeFiles/test_cpp_framework_location_runtime.dir/tests/Zlink.Framework.UnitTests/test_cpp_framework_location_runtime.cpp.o: PASS
CMakeFiles/test_cpp_framework_store_location_resolvers.dir/tests/Zlink.Framework.UnitTests/test_cpp_framework_store_location_resolvers.cpp.o: PASS
```

전체 ctest 첫 실행의 집계 실패 목록 원문:

```text
2:test_cpp_framework_service_wire_codec
10:test_cpp_framework_m6b_runtime
19:test_cpp_framework_layout_contract
```

`service_wire_codec`은 build 중단으로 executable이 없어 Not Run이었다. `m6b_runtime`의
SIGABRT(134)는 규칙에 따라 정확히 1회 재실행했고 다음 원문으로 통과했다.

```text
1/1 Test #10: test_cpp_framework_m6b_runtime ...   Passed    5.85 sec

100% tests passed, 0 tests failed out of 1
```

`layout_contract`는 알려진 기존 실패다. 전체 test 결과는 여전히 대상 변경을 링크한
유효한 검증이 아니다. full build blocker를 해소한 뒤 같은 전체 ctest를 다시 실행해야 한다.

## STOP 여부와 예상 밖 사항

- **STOP: 없음.** 관측 동작 변경이나 lane 경계 재설계가 필요한 구조적 재진입은 발견하지
  못했다.
- 예상 밖: 대상과 무관한 liveness 선언/구현 불일치가 full build를 막았고, ctest도 그
  결과로 service-wire executable 부재를 함께 보고했다.
