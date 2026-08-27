# CP3 C++ 상태 보호 잔존 전환 — batchA

## 결과

상태 보호 잔존 취득 30개를 0개로 전환했다. `handler_registry.cpp`의 실행 primitive
2개(`handler_invocation_executor_mutex`)는 감사 분류를 따라 유지했다. C3 대상은 없었다.
관측 순서·타임아웃·오류 코드와 테스트 기대값은 수정하지 않았다.

`monitoring_runtime.hpp`는 `monitoring_runtime.cpp`가 소유하는 private 상태 선언이라 lane
필드를 넣기 위해 함께 수정했다. public API와 시그니처는 바꾸지 않았다.

## 파일별 판정과 전환

### `runtime/diagnostics/logging.cpp`

- lock 전후: 상태 보호 12 → 0.
- 분류: C2. capture ring, file path와 rotating option의 짝, provider name/callback sink,
  level 이름과 level 값이 같은 `logging_state_t`의 교차 불변식이다.
- 전환: state가 `offload_executor_t`와 `state_lane_t`를 소유한다. 기존 12개 구간과
  lock 없이 읽던 같은 상태의 동기 getter를 하나의 lane turn으로 실행했다.
- 재진입: 0곳. `set_level()`은 lane에 들어가기 전에 `set_min_level()`로 위임하므로
  same-lane public 재진입이 아니다.
- 블로킹 브리지: public 동기 표면 19개, 내부 `is_log_enabled()`·`emit_log()` 2개, 합계
  21개 `.get()`. 반환 전 설정/조회·capture snapshot이 완료돼야 하는 기존 동기 계약을
  유지한다. 외부 gate를 보유하지 않고, lane work는 외부 callback/file I/O를 실행하지
  않으며 snapshot만 만든다. C++ `future` 대기는 inline continuation을 lane 위에서 실행하지
  않으므로 spec 06 §5 조건을 충족한다.
- 본문 조정: 없음.
- 발견 10: capture record, callback, file path, rotating option, console flag의 연속 read와
  snapshot copy를 기존 한 lock 구간 그대로 한 lane turn에 유지했다.

### `runtime/diagnostics/listener_status_registry.hpp`

- lock 전후: 상태 보호 3 → 0.
- 분류: C1. kind/name으로만 조회·추가·삭제하는 단일 listener registry이며 외부 collection
  또는 async 행동과 교차 불변식이 없다.
- 전환: 평범한 nested `std::map`은 registry 전용 lane이 소유한다.
- 재진입: 0곳.
- 블로킹 브리지: `update`·`remove`·`find` 3개. 기존 동기 등록·삭제·조회가 반환 전에
  끝나는 계약을 보존한다. 외부 gate와 장기 작업이 없고 C++ future completion은 inline
  continuation을 실행하지 않아 §5를 충족한다.
- 본문 조정: 없음.
- 발견 10: `find`의 kind map 조회와 name map 조회·status copy·timestamp 갱신은 한 turn에
  유지했다.

### `runtime/diagnostics/monitoring_runtime.cpp`

- lock 전후: 상태 보호 3 → 0.
- 분류: C2. `spot_sources` membership이 `spot_handlers` snapshot의 publication 여부를
  결정한다.
- 전환: `monitoring_runtime_state_t`의 lane이 source 등록, handler 등록, timer failure의
  source 검사와 handler snapshot을 소유한다. handler callback은 원본과 같이 snapshot 뒤
  lane 밖에서 호출한다.
- 재진입: 0곳. callback은 lane 밖에 있으므로 handler가 builder/runtime public 표면을
  호출해도 same-lane 재진입이 아니다.
- 블로킹 브리지: `add_spot_events`, `on_spot_event`, 내부 handler snapshot 3개. 모두
  반환 전 등록/캡처가 필요하며 외부 gate·장기 작업이 없다. §5 충족.
- 본문 조정: 없음.
- 발견 10: source membership 확인과 handler vector copy를 같은 turn에 보존했다.

### `runtime/codecs/serializer.cpp`

- lock 전후: 상태 보호 2 → 0.
- 분류: C1. `resolved_serializers`는 atomic shared snapshot으로 publish되는 단일 lookup
  cache다. serializer registration map과의 교차 상태를 이 cache lock이 소유하지 않는다.
- 전환: cache writer만 전용 lane에서 copy-on-write snapshot을 만들고 atomic publish한다.
  lock-free atomic reader는 그대로 유지했다.
- 재진입: 0곳. `cache_serializer`와 `invalidate_cached_serializer`는 다른 public surface를
  lane 안에서 호출하지 않는다.
- 블로킹 브리지: private cache writer 2개. cache publication/invalidation이 반환 전에
  끝나야 하는 기존 동기 cache 계약을 유지하며, external gate·장기 작업이 없다. §5 충족.
- 본문 조정: 없음.
- 발견 10: current cache load, capacity 판정, copy, insert/erase, atomic publish를 각각
  분리하지 않고 한 writer turn에 유지했다.

### `runtime/handlers/handler_registry.cpp`

- lock 전후: 상태 보호 4 → 0; 실행 primitive 2 → 2 유지.
- 분류: C2. 대상 4개는 registry map이 아니라 filter continuation별 `called`, `duplicate`,
  `downstream` terminal 상태다. next의 단발 호출과 downstream terminal 결과가 같은
  불변식이다.
- 전환: continuation state마다 lane을 소유한다. await 전 claim, await 뒤 success/failure
  terminal 기록, filter 복귀 뒤 final snapshot을 각각 lane turn으로 처리했다.
- 재진입: 0곳. lane 안에서 public registry 표면을 호출하지 않는다.
- 블로킹 브리지: continuation state turn 4개. 외부 gate를 잡지 않으며, await는 lane 밖에
  있고 terminal 기록은 반환 전 완료된다. C++ future completion의 실행 특성까지 포함해 §5
  조건을 충족한다.
- 본문 조정: final read 구간만 non-coroutine lane lambda가 `co_return`할 수 없어서
  기존 분기·오류를 그대로 result 값으로 반환하고, lane 밖 coroutine에서 같은 값을
  `co_return`하도록 기계적으로 추출했다. 오류 종류·문구·성공 message는 변경하지 않았다.
- 발견 10: `duplicate`, `called`, `downstream` 검사와 final value/error 선택은 하나의
  final lane turn 안에 유지했다.

### `runtime/spots/message_follow_suppression_registry.hpp`

- lock 전후: 상태 보호 6 → 0.
- 분류: C2. suppression state는 독립 registry가 아니다. `actor_transfer_coordinator_t`의
  message-follow route 추가·교체·만료·삭제와 같은 불변식에 참여한다.
- 전환: 별도 concurrent map/lane으로 분할하지 않았다. 모든 직접 호출이 이미
  `actor_transfer_coordinator_t::_lane.run(...).get()` turn 안에서 일어나므로, 평범한 map을
  그 부모 C2 lane이 소유한다.
- 재진입: 0곳. parent lane turn은 suppression helper를 직접 호출하며 child lane에
  재진입하지 않는다.
- 블로킹 브리지: 신규 0개. 기존 parent coordinator bridge를 추가·변경하지 않았다.
- 본문 조정: 없음.
- 발견 10: route와 suppression key의 교체/erase/상태 전이는 기존 부모 turn 안에 함께
  남아 있어 연속 read/capture가 분리되지 않는다.

## 검증

### 상태 보호 수 재확인

대상 6파일의 `std::lock_guard` 검색 결과는 handler invocation executor의 실행 primitive
2개만 남았다. 상태 보호 잔존은 30 → 0이다.

### 빌드

지정 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
```

첫 전체 빌드는 `test_cpp_framework_service_wire_codec` 링크에서 중단됐다. 이 target의
명시 source 목록에는 `serializer.cpp`와 기존 `service_topology_registry.cpp`가 들어가지만
`state_lane.cpp`와 `offload_executor.cpp`가 링크되지 않는다. 후자는 batchA 전부터 lane을
참조한다. 따라서 이 실패는 batchA의 source compile failure가 아니라 CMake test target의
link 구성 차단이다.

`zlink_framework_m6b_objects`는 `[100%] Built target zlink_framework_m6b_objects`로
통과했다. `serializer.cpp`를 포함한 M6A object는 첫 전체 빌드에서 컴파일됐다. 이후 M6A
객체 재빌드는 대상 밖 `runtime/mesh/service_topology_registry.hpp`와
`runtime/mesh/service_liveness_registry.hpp`의 `std::mutex` include 누락으로 중단됐으며,
batchA 파일은 수정하지 않았다.

### 단위·계약 CTest 집계 원문

```text
93% tests passed, 3 tests failed out of 45

Total Test time (real) =  57.93 sec

The following tests FAILED:
	  2 - test_cpp_framework_service_wire_codec (Not Run)
	 19 - test_cpp_framework_layout_contract (Failed)
	 33 - test_cpp_framework_execution (Failed)
Errors while running CTest
```

- `test_cpp_framework_service_wire_codec`: 위 링크 차단으로 executable이 없어 Not Run.
- `test_cpp_framework_layout_contract`: 요청에서 지정한 알려진 기존 실패.
- `test_cpp_framework_execution`: 집중 재실행 원문은 다음과 같으며 통과했다.

```text
1/1 Test #33: test_cpp_framework_execution .....   Passed    0.49 sec

100% tests passed, 0 tests failed out of 1
```

## STOP 여부와 예상과 달랐던 점

- STOP: 없음. 관측 동작 변경이나 lane 경계 재설계가 필요하지 않았다.
- 예상과 달랐던 점: suppression registry는 독립 C1이 아니라 parent coordinator와 결합된
  C2였고, handler의 감사 대상 4개도 registry 자체가 아니라 async filter terminal state였다.
  또한 전체 test target의 lane primitive source-link 누락과 대상 밖 mutex include 누락 때문에
  전체 build green은 확보하지 못했다.
