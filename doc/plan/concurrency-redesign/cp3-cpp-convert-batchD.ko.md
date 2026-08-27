# CP3 C++ 상태 보호 잔존 전환 — batchD

## 결과

`actor_client.cpp`의 상태 보호 취득 중 **C2 4개를 0개**로 전환했다. route cache C1
5개와 나머지 네 파일은
외부 callback/transport 또는 foreign recursive state의 turn 경계 재설계가 필요한 자리여서
이번 패스에서는 변경하지 않았다. 부분 전환이므로 STOP은 아니다.

## 파일별 판정

### `runtime/actors/actor_client.cpp`

- lock 전/후: **9 / 5**.
- 분류: `actor_create_call_state_t`의 4개는 option flag·option 값·submitted와 manager 호출
  준비가 한 불변식인 **C2 4개**다. `_route_cache`의 5개는 map 하나만 다루는 **C1 5개**다.
  C3은 없다.
- 전환: C2 state에 `offload_executor_t`와 `state_lane_t`를 추가했다. C1 cache는 기존 lock을
  유지했다.
- 재진입 실측·해소: public builder 사이의 재진입과 같은 state public 표면의 lane 안 재호출은
  발견하지 못했다.
- 블로킹 브리지: **4개**. 기존 동기 builder가 반환 전에 option 검증과 submitted claim을
  끝내야 하므로 `.get()`을 유지했다. §5 조건은 충족한다:
  외부 gate를 보유한 채 lane을 기다리지 않고, C++ `future::get()`에는 lane-current inline
  continuation이 없으며, 반환 전 완료 계약이 있다.
- 본문 조정: C2 builder 4개는 lock 선언 제거와 lambda 경계뿐이다. C1은 early-return block
  본문을 한 글자도 바꾸지 않기 위해 미전환이다.
- 발견 10: 생성 call의 submitted 확인·set·manager capture를 한 turn에 유지했다. C1 cache의
  lookup, expiry 비교, erase도 기존 하나의 lock 구간으로 유지된다.
- C1 미전환 사유: 현재 동기 early-return 표면을 동기 lane turn으로 감싸려면 return을 helper 결과로
  재구성해야 하며, 이는 이번 패스의 본문 불변 규칙을 위반한다.

### `runtime/actors/actor_gateway_runtime.cpp`

- lock 전/후: 상태 보호 **9 / 9**; 실행 primitive 3개는 제외.
- 분류: session id, token, ready actor, actor stream, native binder의 교차 상태이므로 **C2 9개**.
- 재진입: `1689`의 unlock/relock은 placeholder 설치와 native publish 사이 프로토콜이다. 발견 6의
  turn A(검증·상태 전이·claim) / publish 밖 / turn B(placeholder 교체)로 바꿔야 한다.
- 블로킹 브리지: 새로 추가 없음. 본문 조정: 없음. 발견 10: reuse 판단의 token/ready/stream read는
  하나의 turn이어야 한다.
- 미전환 사유: native binder와 `bind_session_stream`의 exact rollback을 보존하는 3-turn 재구성이
  필요하다.

### `runtime/stateful/raw_stateful_dispatch.cpp`

- lock 전/후: 상태 보호 **9 / 9**; socket·dispose 3개는 제외.
- 분류: `_next_sequence`, `_pending`, `_discarding_owners`, exact object claim이 결합된 **C2 9개**.
- 재진입: 직접 public 재호출은 확인하지 못했다. `complete_relocated_source_async`는 pending
  identity 확인·claim·transport send·정산을 발견 6의 turn A/send/turn B로 유지해야 한다.
- 블로킹 브리지: 새로 추가 없음. 본문 조정: 없음. 발견 10: `try_claim` 및 discard의 multi-map
  판독/변이는 각각 한 turn이어야 한다.
- 미전환 사유: coroutine의 `co_return`을 포함한 기존 lock 본문을 유지하면서 외부 send를 lane 밖으로
  꺼내려면 결과 타입화와 private turn helper가 필요하다.

### `runtime/host/app.cpp`

- lock 전/후: 상태 보호 **7 / 7**; socket·dispose 17개는 제외.
- 분류: app status cache와 relocation/termination operation의 started/terminal/result/waiter
  불변식은 **C2 7개**. `231`은 app이 아닌 foreign `spot_state`의 recursive C2이다.
- 재진입: foreign `spot_state` recursive mutex는 owner lane에서만 해소할 수 있어 app lane으로
  옮기지 않았다.
- 블로킹 브리지: 새로 추가 없음. 본문 조정: 없음. 발견 10: relocation/termination 상태의
  파생 status read는 한 turn으로 유지해야 한다.
- 미전환 사유: source-local app state와 foreign spot-state lock이 한 파일에 섞여 있고,
  recursive callback 경로를 private owner helper로 분리하는 별도 경계 작업이 필요하다.

### `runtime/mesh/mesh_node_host_service.cpp`

- lock 전/후: 상태 보호 **5 / 5**; 실행 primitive 11개는 제외.
- 분류: `1711,1723`은 foreign spot native-map/lifecycle **C2 2개**, `2179,2494,2565`는 location
  owner, published node/descriptor, store renewal의 **C2 3개**다.
- 재진입: 앞의 2개는 foreign recursive spot state다. descriptor publisher는 external store 작업을
  포함하므로 발견 6/7에 따라 turn A/store call/turn B 경계를 먼저 설계해야 한다.
- 블로킹 브리지: 새로 추가 없음. 본문 조정: 없음. 발견 10: owner 판독, descriptor copy/update,
  vector 반영은 각각 한 파생-state turn으로 유지해야 한다.

## 검증과 STOP

- 변경 파일: `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp`, 이 보고서.
- 테스트 기대값 변경: 없음.
- 빌드: 전체 명령과 `--target zlink_framework -j8`를 실행했다. 마지막 target build는 batchD
  범위 밖의 `runtime/mesh/service_liveness_registry.cpp`에서 `_mutex` 미선언 오류 7개로
  중단됐다(`admit`, `disconnect`, `acknowledge`, `acknowledge_probe`, `tick`, `next_activity`, `size`).
  현재 shared checkout에서 해당 헤더의 `_mutex`가 제거된 반면 구현은 아직 이를 참조한다. 이 파일은
  batchD 허용 범위 밖이므로 수정하지 않았다.
- focused compile: `cmake --build framework/languages/cpp/build --target framework/src/runtime/actors/actor_client.o -j8`
  통과.
- 테스트: `flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'` 실행. 집계 원문:

  ```text
  93% tests passed, 3 tests failed out of 45

  Total Test time (real) =  57.57 sec

  The following tests FAILED:
	  2 - test_cpp_framework_service_wire_codec (Not Run)
	 19 - test_cpp_framework_layout_contract (Failed)
	 33 - test_cpp_framework_execution (Failed)
  [ERROR_MESSAGE]
  Errors while running CTest
  ```

  `layout_contract`는 요청에 명시된 기존 실패다. `service_wire_codec`은 build tree에 executable이
  없어 Not Run이었고, `execution`은 별도 실패이므로 이번 actor-client 전환의 통과 증거가 아니다.
- STOP: **없음**. 관측 동작 또는 테스트 기대값을 바꿔야 하는 상황은 만들지 않았고,
  안전하게 전환 가능한 한 파일을 완료했다.
- 예상과 달랐던 점: 기존 보고서의 “선언 헤더가 범위 밖” 전제는 이번 요청에서 해소되어 있었다.
  남은 네 파일의 난점은 헤더 권한이 아니라 callback/transport/foreign recursive state의 turn
  경계를 보존하는 것이다.
