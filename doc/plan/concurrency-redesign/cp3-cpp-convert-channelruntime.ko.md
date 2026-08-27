# CP3 C++ channelruntime 상태 소유 전환

## 범위와 분류

`channel_runtime_state_t`는 channel 설정·pending request·bundle·native transport·route/mesh
callback·handler·discovery cursor·shutdown을 함께 결정한다. 따라서 대상 37개 취득은 모두
**C2**다(C1 0, C2 37, C3 0). CP3 감사의 실행 primitive 및 socket/dispose protocol 취득은
그대로 두었다.

## 파일별 결과

### `runtime/channels/channel_runtime.hpp`

- 단일 `mutex`를 제거하고 객체 수명에 결박된 `offload_executor_t`와 `state_lane_t`를 추가했다.
- state lane은 executor보다 뒤에 파괴되어, lane의 drain/close가 executor 수명 안에서 끝난다.

### `runtime/channels/channel_runtime.cpp`

- 상태 보호 취득: **37 -> 0**.
- C1/C2/C3: **0/37/0**.
- 각 기존 임계 구역을 같은 객체의 `lane.run([&]{ ... }).get()` 한 turn으로 옮겼다. sender/requester
  pair, client-server/mesh channel 중복 검증+등록, shutdown+route snapshot은 각각 한 turn을 유지했다.
- 재진입 실측: 대상 37곳의 중첩 취득 0곳. lane 본문에서 동일 `channel_runtime_t` public 표면을
  다시 호출하는 자리를 만들지 않았다.
- blocking bridge: **37개**. 공개 동기 반환과 등록/캡처의 return-before 계약을 유지하기 위한 경계다.
  turn은 외부 gate를 재획득하지 않고, C++ `std::future` 완료에는 inline dependent continuation API가
  없으므로 spec 06 §5의 세 조건을 충족한다.
- 본문 조정: state lane의 반환값을 public 동기 반환값으로 전달하기 위한 return wrapper와,
  optional callback snapshot의 반환형 명시만 추가했다. 상태 전이·오류 kind/message·timeout은 그대로다.
- 발견 10: shutdown route-channel snapshot, sender/requester pair 및 transport 중복 검증은 원래 한
  lock 구간의 read/결정을 하나의 lane turn으로 유지했다.

### `runtime/channels/channel_outbound_exchange.cpp` (직접 호출자 전파)

- 제거된 상태 mutex를 참조하던 callback lookup, endpoint/default-timeout snapshot, outbound 기록,
  native client/publisher 등록·close snapshot을 동일 state lane으로 전파했다.
- socket 자체의 `transport`/readiness mutex는 변경하지 않았다.
- native publisher/client의 callback·I/O는 lane 밖에 남기고, current slot 판정 및 map 교체만 lane에서
  수행한다. shutdown/close 상태와 slot 선택은 한 turn 안에서 결정한다.

### `runtime/spots/spot_runtime.cpp` (직접 호출자 전파)

- route-channel 후보 세 read(명시 이름, 단일 route, accepted route)를 하나의 lane turn으로 묶었다.
  따라서 발견 10의 파생 snapshot 분할이 없다.

## 재진입·관측 동작·STOP

- 외부 callback은 기존처럼 lane 밖에서 호출한다. 이번 범위에서 발견 6의 placeholder 분리가 필요한
  callback 재진입은 발견되지 않았다.
- 오류 kind/message, timeout, 순서와 테스트 기대값은 변경하지 않았다.
- STOP: **아니오**.
- 예상과 달랐던 점: `channel_outbound_exchange.cpp`의 state mutex 참조와 spot route 선택의 직접
  참조가 있어, 헤더의 C2 mutex를 제거하려면 두 직접 호출자 전파가 필요했다. socket/readiness
  protocol mutex는 그대로 유지됐다.

## 검증

### 빌드

명령:

```bash
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j8
```

결과: 성공 (`[100%] Built target test_cpp_framework_contract_headers`).

### unit·contract gate

명령:

```bash
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
```

집계 원문:

```text
Total Test time (real) =  52.18 sec

The following tests FAILED:
	 13 - test_cpp_framework_host_lifecycle (Failed)
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
```

- `test_cpp_framework_layout_contract`: 지시문에 명시된 기존 실패다.
- `test_cpp_framework_host_lifecycle`: 단독 재실행은 통과했다.

```text
1/1 Test #13: test_cpp_framework_host_lifecycle ...   Passed   11.60 sec

100% tests passed, 0 tests failed out of 1
```

따라서 이 작업으로 인한 재현 가능한 신규 실패는 확인하지 못했지만, 전체 gate는 알려진
layout-contract 실패 때문에 exit 8로 끝났다.
