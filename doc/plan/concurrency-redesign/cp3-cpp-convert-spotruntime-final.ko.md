# CP3 C++ `spot_runtime` state lane 전환 최종 보고

## 1. 결론과 범위

`framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp`와
`spot_runtime.hpp` 안에서 `spot_node_builder_state_t`의 교차 불변식 상태를 한
`state_lane_t`가 소유하도록 전환했다. 대상 구현의 주 상태 `recursive_mutex` 취득은 0개이고,
`std::unique_lock<std::recursive_mutex>&` 전달도 0개다. 주 lane 안의 동일-lane 재진입,
외부 callback·native·dispatcher·Location Store 실행, 독립 lane에서 주 상태를 직접 읽는 경로는
최종 정적 감사에서 모두 0개였다.

다만 저장소 전체 기준으로는 완결되지 않았다. 허용 파일 밖의 production 호출자 3개가
`spot_node_builder_state_t::mutex`를 취득해 같은 상태를 직접 읽거나 변경한다. 이 호출자들을 이번
범위에서 수정할 수 없어서 `spot_runtime.hpp`에 호환용 `std::recursive_mutex` 선언 1개를 남겼다.
따라서 판정은 다음과 같다.

- 지정된 두 대상 파일의 live C2 접근: **CLEAN**
- 저장소 전체의 단일 lane 소유권: **NOT-CLEAN — 허용 범위 밖 호환 호출자 잔존**

스펙 파일, 테스트 기대값, 공개 API와 허용 범위 밖 소스는 변경하지 않았다.

## 2. lock 전후 수

작업 시작 체크아웃에서 구문과 소유 상태를 분리해 센 값은 다음과 같다. 사용자 요청에 적힌
이전 패스의 `151 → 155`, `recursive_mutex` 132곳은 그 패스의 광역 lexical 수치이고, 아래 표는
이번 작업 시작 시점의 두 대상 파일을 `spot_node_builder_state_t` C2 취득과 독립 작업-protocol
mutex로 다시 분류한 수치다.

| 구분 | 전 | 후 |
|---|---:|---:|
| 대상 파일의 주 C2 `recursive_mutex` 취득 | 120 | **0** |
| 대상 파일의 전체 mutex 취득 구문 | 154 | 34 |
| 독립 timer/completion/remote-commit mutex 취득 | 34 | 34 |
| `recursive_mutex` 선언 | 1 | 1(호환 선언) |
| `unique_lock<recursive_mutex>&` 인자 | 3차 실패 구현에 존재(개별 baseline 미보존) | **0** |

후 상태의 일반 `std::unique_lock` 3개와 `std::lock_guard` 31개는 remote commit 완료 상태,
deadline/timer, submission/completion 같은 독립 작업 protocol 상태만 보호한다. 주 C2 상태에는
사용하지 않는다.

## 3. 교차 불변식 그룹 판정과 lane 편성

### 3.1 한 주 lane으로 합친 그룹

다음 네 영역은 하나의 교차 불변식 그룹이다.

1. Builder 구성: snapshot, Spot/Actor factory, lifecycle, resolver, serializer·runtime callback 연결
2. Spot context·생성·close·idle eviction: 이름/ID/context/pending creation/native facade/close reservation
3. Actor registry·실행: instance index, Spot membership, route, generation, authority fence, 실행 queue snapshot
4. Relocation·handoff: recovery, remote source cleanup, Message Follow, Actor Join admission·commit·destroy

Builder factory·lifecycle 등록은 Spot 생성과 Actor materialization/relocation에서 바로 소비된다.
Spot close와 idle eviction은 Actor membership·destroy 및 relocation cleanup과 같은 context,
generation, authority fence를 함께 바꾼다. Relocation/handoff도 Actor route·generation·pending cleanup을
같은 원자 경계에서 판정한다. 따라서 Builder, Spot, Actor, relocation을 분리 lane으로 나누지 않고
`spot_node_builder_state_t::lane` 하나에 배치했다(`spot_runtime.hpp:68-72`). 컨테이너는 기존
`std::map`, `std::set`, `std::vector`인 평범한 컨테이너 그대로이며 컬렉션별 concurrent container를
추가하지 않았다.

### 3.2 독립 ownership region

다음 상태는 주 C2 aggregate와 양방향 불변식을 공유하지 않아 기존 독립 lane/내부 동기화를
유지했다.

- `pending_handoff_requests_lane`: parked reply token과 OperationId terminal 정산만 소유
- `actor_pending_requests_lane`: transfer 시점의 in-flight request 계수만 소유
- `route_client_lane`: route client handle 교체·snapshot만 소유
- 각 `spot_context_state_t::callback_lane`: callback depth, close admission, deferred lifecycle queue만 소유
- `actor_transfer_coordinator`, `exactly_once_table_t`: 각 타입이 소유하는 독립 상태 머신
- timer, deadline, submission, completion mutex: C2 collection이 아닌 완료/대기 protocol

독립 lane이 local node RID 같은 주 상태 값을 필요로 하는 두 자리는 먼저 주 lane에서 값을
projection한 뒤 전달하도록 바꿨다. 독립 lane 안에서 주 `snapshot`을 다시 읽지 않는다.

## 4. 참조 구현에서 가져온 패턴

### .NET `ZLinkSpotNodeCatalog`

- 참조 커밋 `0fef22fa62`의 lock 48 → 0, 한 lane과 평범한 Dictionary 6개 구성
- `_lane.RunAsync(() => { ... })`가 평범한 Dictionary aggregate를 소유하는 구조
- sync 표면만 `AwaitStateLane(...)`로 완료를 기다리는 bridge
- `RemoveActivationLocked`, `ThrowIfClosingLocked`, `EnsureLocalSpotCapacityLocked`,
  `BeginCreationLocked`처럼 “이미 lane turn 안”을 뜻하는 private helper 규약
- callback 전 prepare/claim turn, callback 밖 실행, 결과를 정산하는 후속 turn

### .NET `ZLinkActorRuntimeState`

- public 표면이 lane에 한 번 들어가고 `TransitionLocalInstanceCore(...)`를 호출하며,
  core끼리는 직접 호출하는 구조

### Java `ZLinkSpotRuntime`

- `synchronized`/lock 0인 가장 강한 형태
- 상위 serial queue가 소유하는 `ArrayList`/`HashMap`을 내부 lock 없이 사용하는 구조
- 사용자 callback과 장기 작업을 상태 소유 turn과 분리하는 구조

C++에서는 이 패턴을 `find_context_core`, `framework_worker_executor_core`,
`framework_deadline_executor_core`, `reserve_close_core`, `clear_close_reservation_core`,
`idle_age_allows_close_core`에 적용했다. Actor Join은 `actor_join_context`, `actor_factory`,
`actor_admission`, `commit_accepted_actor_join`이 함수 객체와 상태 snapshot을 **값으로** 주고받는다.
어느 helper도 lock 객체나 `unique_lock&`를 인자로 받지 않는다.

## 5. 재진입 실측과 해소

`state_lane_t::run()`은 같은 lane에서 다시 호출하면 즉시 예외를 던진다. 최신 cpp의 246개,
hpp의 36개 `.run` 영역을 brace 단위로 검사한 결과 nested run은 0개였다.

해소한 핵심 지점은 다음과 같다.

- public `find_context()`는 lane에 들어가고, turn 내부 호출자는 `find_context_core()`를 직접 호출
- worker/deadline executor public bridge는 lane에 들어가고, turn 내부는 각각 `*_core()`를 호출
- Spot close/idle은 reservation core가 한 turn 안에서 claim·검증·해제를 수행
- Actor Join은 context/factory/admission을 값으로 projection한 후 사용자 factory·admission callback을
  lane 밖에서 실행하고, commit turn에서 context/route/fence를 재검증
- relocation materialization과 Actor handoff는 prepare/외부 작업/finalize turn으로 분리

최종 감사 결과는 동일-lane 재진입 0, main lane 내부 callback/native/dispatcher/store 호출 0이다.

## 6. 블로킹 bridge 수와 spec 06 §5 판정

구문 단위로 센 주 state lane의 동기 `run(...).get()` bridge는 **238곳**이다. 이 수치는 public과
private 동기 표면을 합친 보수적 수치다. 전체 lane turn은 282곳이고, 나머지 44곳은 per-context
callback lane, pending-handoff, actor-pending, route-client, channel-runtime 같은 독립 lane이다.
전체 `.get()` lexical 수 432에는 일반 future/task 완료가 포함되므로 bridge 수로 사용하지 않았다.

동기 bridge가 남은 이유는 다음과 같다.

- Builder registration/snapshot과 동기 create/find/list/current-route API는 반환 전에 등록·snapshot이
  완료되어야 한다.
- Actor/Spot admission, generation/fence claim, close reservation은 caller가 반환을 관찰하기 전에
  정확한 ownership 결과가 확정되어야 한다.
- 이번 범위에서 공개 C++ signature와 관측 순서를 async로 바꾸는 것은 금지되어 있다.

§5의 세 조건 판정은 다음과 같다.

1. lane work가 재취득할 외부 gate를 caller가 보유하는 경로: 발견되지 않음. nested run은 0이고,
   독립 lane과의 교차 값은 projection으로 전달한다.
2. 완료 신호: `state_lane_t`는 executor에서 `std::promise`를 완료하고 blocked caller의
   `std::future::get()`을 깨운다. `std::future`에는 completion thread에서 inline 실행되는 dependent
   continuation이 없으며, caller는 자기 thread에서 재개한다.
3. 동기 완료 필요성: registration/capture/claim과 기존 public sync signature 때문에 존재하며 위에
   사유를 기록했다.

따라서 대상 내부 bridge는 §5 조건을 만족한다. lock을 보유한 채 `.get()`하는 경로는 없다.

## 7. 원래 lock 본문 외 조정 목록

단순히 본문을 `lane.run`으로 감싸는 것만으로 외부 호출이 lane 안에 남거나 turn 사이 수명이
끊기는 곳에는 다음 최소 조정을 했다. 오류 코드, timeout, callback 순서와 terminal 의미는
바꾸지 않았다.

1. Spot create: pending-creation reservation을 turn A에서 claim하고 factory/native/Location 작업을
   밖에서 수행한 뒤 publish/rollback turn에서 reservation을 재검증한다.
2. Native Spot 생성: `attach_native_spot()`이 `shared_ptr`을 반환하고 `staged_native`가 최종 publish
   turn까지 strong owner를 유지한다. weak pointer만 남아 native facade가 조기 소멸하던 수명 공백을
   없앴다.
3. Spot close/idle eviction: close reservation과 generation을 한 turn에서 묶고 외부 lifecycle,
   native close, Store 작업은 밖에서 수행한 후 정산한다. 이미 시작된 close에는 같은 기존 완료를
   돌려준다.
4. Actor Join: factory/admission/serializer/context/route 값을 복사해 callback과 materialization을
   밖에서 실행하고, commit turn에서 동일 context와 authority fence를 다시 확인한다.
5. Actor destroy/transfer/relocation: route·generation·authority fence·actor count 전이는 한 turn에
   두고 registry/lifecycle/native/Store/relay 호출은 밖으로 옮겼다.
6. shutdown: queue/executor/context 목록을 주 turn에서 projection 또는 reset하고 실제 drain/cancel은
   lane 밖에서 수행한다.
7. sync serializer/config/callback 표면: channel runtime과 함수 객체를 주 turn에서 복사하고 호출은
   밖에서 수행한다.
8. restore 경로: Location lifecycle이 없는 기존 허용 경로에서는 Location projection을 만들지 않도록
   기존 조건을 보존했다.
9. C++20 lambda 문법을 유지하도록 trailing return lambda에 명시적 `()`를 넣었다. 동작 변경은 없다.

## 8. 발견 10 적용 자리

한 파생 값을 만드는 여러 read는 한 turn에서 구조체/tuple로 projection했다.

- route channel 선택: node snapshot의 configured/accepted channel과 channel runtime handle
  (`spot_runtime.cpp:1664-1698`)
- Spot publish: native facade, dispatch mode 설정, mesh/discovery channel, serializer runtime
  (`spot_runtime.cpp:3281-3335`)
- local/remote Actor Join: context instance, serializer, mesh/node RID, source Spot, Message Follow duration,
  root-service 존재 여부(`spot_runtime.cpp:5270-5310`, `5476` 이후 대응 projection)
- relocation prepare/install: target context와 `native_node` shared pointer를 같은 turn에서 얻고
  `native->status()`는 turn 밖에서 호출(`spot_runtime.cpp:6216-6256`, `6689-6695`)
- wire Actor Join: Entry Spot 판정과 local node RID를 같은 turn에서 산출
  (`spot_runtime.cpp:12830-12841`)

## 9. 빌드·테스트

빌드 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock cmake --build framework/languages/cpp/build -j10
```

결과: exit 0. 최종 source rebuild와 link가 모두 완료됐다.

집중 검증:

- `test_cpp_framework_m6c_runtime`: 통과
- `test_cpp_framework_execution`: 통과
- `test_cpp_framework_actor_gateway`: 통과
- `test_cpp_framework_handler_registry`: 통과
- `test_cpp_framework_messaging`: 확대 focused 묶음의 첫 실행에서 출력 없이 2.02초 실패했으나,
  같은 바이너리 직접 실행은 exit 0, 즉시 단독 ctest는 0.05초 통과, 아래 최종 전체 gate도 0.10초
  통과했다.

최종 전체 gate 명령:

```text
flock -w 10800 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build -L 'framework-(unit|contract)' -LE 'e2e|sample|perf'
```

최종 집계 원문:

```text
98% tests passed, 1 tests failed out of 45

Label Time Summary:
DI                               =   0.00 sec*proc (1 test)
actor                            =   5.96 sec*proc (1 test)
actor-join                       =   0.01 sec*proc (1 test)
admission                        =   0.21 sec*proc (2 tests)
async                            =   0.20 sec*proc (2 tests)
backpressure                     =   0.02 sec*proc (1 test)
channel                          =   1.11 sec*proc (2 tests)
claim                            =   0.16 sec*proc (1 test)
diagnostics                      =   0.05 sec*proc (1 test)
execution                        =   0.63 sec*proc (3 tests)
framework-actor                  =   0.70 sec*proc (1 test)
framework-client-server          =   0.04 sec*proc (1 test)
framework-contract               =  13.22 sec*proc (8 tests)
framework-foundation             =   0.03 sec*proc (7 tests)
framework-integration            =   0.00 sec*proc (1 test)
framework-location               =  18.59 sec*proc (6 tests)
framework-m6-runtime             =  23.38 sec*proc (5 tests)
framework-monitoring             =   0.04 sec*proc (1 test)
framework-observability          =   0.07 sec*proc (2 tests)
framework-package                =   4.11 sec*proc (1 test)
framework-regression             =  22.03 sec*proc (23 tests)
framework-tooling                =   4.11 sec*proc (1 test)
framework-unit                   =  46.70 sec*proc (39 tests)
framework-zlink                  =   1.58 sec*proc (1 test)
framework-zlink-actor-gateway    =   1.58 sec*proc (1 test)
framework-zlink-channel          =   1.58 sec*proc (1 test)
framework-zlink-spot             =   1.58 sec*proc (1 test)
gtest                            =   0.01 sec*proc (1 test)
handler                          =   0.15 sec*proc (1 test)
instance-activation              =   0.08 sec*proc (1 test)
liveness                         =   5.37 sec*proc (1 test)
mailbox                          =   5.37 sec*proc (1 test)
maintenance                      =  11.89 sec*proc (2 tests)
messaging                        =   0.10 sec*proc (1 test)
metrics                          =   0.02 sec*proc (1 test)
operation                        =   0.00 sec*proc (1 test)
protocol                         =   0.03 sec*proc (6 tests)
raw-binding                      =   5.38 sec*proc (2 tests)
recovery                         =   0.51 sec*proc (1 test)
reliability                      =   0.02 sec*proc (1 test)
relocation                       =  11.89 sec*proc (2 tests)
reply                            =   0.24 sec*proc (2 tests)
resource                         =   0.52 sec*proc (2 tests)
runtime                          =   0.00 sec*proc (1 test)
scope                            =   0.00 sec*proc (1 test)
serializer                       =   0.01 sec*proc (1 test)
spot                             =   6.52 sec*proc (3 tests)
stateful                         =   6.12 sec*proc (2 tests)
stream                           =   0.70 sec*proc (1 test)
stream-session                   =   5.96 sec*proc (1 test)
termination                      =  11.37 sec*proc (1 test)
topology                         =   5.37 sec*proc (1 test)
yield                            =   0.48 sec*proc (1 test)

Total Test time (real) =  59.93 sec

The following tests FAILED:
	 19 - test_cpp_framework_layout_contract (Failed)
Errors while running CTest
Output from these tests are in: /home/hep7/project/zlink/framework/languages/cpp/build/Testing/Temporary/LastTest.log
Use "--rerun-failed --output-on-failure" to re-run the failed cases verbosely.
```

실패는 요청에 명시된 기존 `test_cpp_framework_layout_contract` 1건이다. full-run flake로 알려진
`test_cpp_framework_host_lifecycle`은 이번 전체 실행에서 11.37초로 통과했다. 명령 exit는 8이어서
지정된 exit 86/134 재실행 조건은 발생하지 않았다.

## 10. 남은 범위와 필요한 후속 작업

허용 범위 밖 production 코드의 직접 mutex 접근은 다음 10곳이다.

- `framework/languages/cpp/framework/src/runtime/host/app.cpp:231`: snapshot/factory 구성 판독
- `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:1711,1723`:
  `native_spots_by_id` 변경
- `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2155,2165,2171,2188,2205,2211,3364`:
  Actor type/native ownership/membership epoch 변경

이 호출자들은 호환 mutex와 주 lane 사이의 공통 직렬화가 없으므로 저장소 전체 관점에서는 같은
C2 상태의 이중 소유 경계다. 이번 요청의 “허용 파일 밖 수정 금지” 때문에 고치지 않았다. 후속
패스에서는 위 세 파일의 직접 접근을 `spot_node_runtime_t`의 lane-backed 내부 표면으로 옮기고,
관련 테스트 seam의 직접 mutex 취득도 함께 제거한 뒤 `spot_runtime.hpp:76`의 호환 선언을 삭제해야
한다.

## 11. 변경 파일

- `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp`
- `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.hpp`
- `doc/plan/concurrency-redesign/cp3-cpp-convert-spotruntime-final.ko.md`
