# C++ intermittent runtime 수정 결과

## 결과

D-131에서 승인한 class B runtime 결함을 C++ Framework에서 수정했다. executor owner는
`app.run()` 실행 수명에 맞춰 획득·해제하며, durable operation 종료 술어는 expectation과
admitted peer 중 어느 사실이 나중에 바뀌어도 다시 평가한다. Public API와 Core·binding·sample,
다른 언어 구현은 변경하지 않았다. 새 회귀 test, Fanout 회귀, 전체 Framework unit gate는
통과했다.

최종 sample gate는 7개 중 6개가 통과했다. ZoneWorld G4가 기대한 `Unavailable` 대신 source
node의 ActorJoin callback에서 `DeadlineExceeded`(`kind=7`)로 끝났고 client는 해당 응답을
성공 조건으로 받아들이지 않은 뒤 timeout으로 종료했다. 따라서 sample 7/7 완료 조건은
충족하지 못했다.

## 변경 파일과 동작

- `framework/languages/cpp/framework/src/runtime/host/app.cpp:2695`: 실제 `app_t::run()`이
  시작될 때 shared coroutine executor owner를 획득한다. 정상 종료의 `:2753`과 예외 종료의
  `:2725`가 같은 실행에서 획득한 owner를 해제한다. 구성만 하고 실행하지 않은 app은 owner를
  획득하지 않는다.
- `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:996`: 기존
  `end_peer_operations_if_disconnected_locked()`를 단일 종료 술어로 유지한다.
- `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3880`: monitor가
  admitted peer를 실제로 제거한 뒤 pending admission을 폐기하고 기존 종료 술어를 평가한다.
- `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3974`: liveness가
  admitted peer를 실제로 제거한 뒤 같은 종료 술어를 평가한다. topology·operation 상태 조회는
  기존 lifecycle mutex 아래에서 수행한다.
- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_executor_owner.cpp:30`:
  구성 후 실행하지 않은 app을 파괴하고 두 번째 app을 실행·정지한 뒤 public app/scheduler
  interface로 continuation scheduler가 남지 않았음을 확인한다.
- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:2737`:
  expectation을 먼저 제거한 뒤 monitor disconnect 또는 liveness tick으로 admitted peer를
  제거하는 순서를 각각 재현하고 ActorJoin이 `Unavailable`로 끝나는지 확인한다.
- `framework/languages/cpp/CMakeLists.txt:1119`, `:1508`: lifecycle 회귀와 executor owner
  회귀를 `framework-unit;framework-regression` test로 등록한다.

## 규칙 수

- Executor owner: 구성 시 획득하고 실행 시 해제하던 서로 다른 수명 규칙 2개에서,
  `app.run()` 실행이 획득과 해제를 모두 소유하는 규칙 1개로 줄였다.
- Durable operation: expectation 제거와 admitted-peer 제거 순서에 따라 종료 여부가 달라지던
  규칙 2개에서, `expectation 없음 && admitted peer 없음`을 각 입력 변경 뒤 평가하는 술어
  1개로 줄였다.

## 소유권과 계약 대조

- 소유 계층: executor 정리는 Framework host 실행 수명이 소유한다. durable operation 종료는
  Framework의 logical peer lifecycle과 operation registry가 소유한다.
- spec 조항: Framework API §10의 dispatch scope exactly-once cleanup과 Actor model §8.1의
  logical target 종료 조건, failure policy §2/§4의 단일 terminal 결과를 적용했다.
- 교차언어 대조: .NET handler dispatcher는 await가 끝날 때까지 invocation scope를 유지한다.
  C++은 process-shared executor가 있어 owner 획득을 host 실행 수명에 별도로 맞췄다. Java
  `ZLinkJavaRawMeshNode.java:7054`는 expectation과 ready peer를 함께 검사하는 같은 종료 술어를
  durable request에 전달한다. C++은 event 기반 상태이므로 두 입력 변경 지점에서 그 술어를
  호출한다.
- 변경 분류: 두 수정 모두 D-131에서 승인한 **B — 기존 결함 수정**이다.

## 검증 결과

| 검증 | 결과 |
|---|---|
| `cmake --preset linux-ninja-debug`와 두 신규 target build (`--parallel 2`) | 통과 |
| 신규 test `--repeat until-fail:20` | 두 test 각각 20/20 통과 |
| `test_cpp_framework_store_location_resolvers --repeat until-fail:5` | 5/5 통과; 기존 Fanout scope 생성/해제 동등성 assertion 유지 |
| 전체 preset build (`--parallel 2`) | 85/85 build step 통과 |
| `ctest -L framework-unit --parallel 1 --output-on-failure` | 53/53 통과 |
| `bash samples/run_samples.sh` | 6/7 통과; ZoneWorld G4 실패 |
| `ZLINK_CPP_MESH_TRACE=1 bash samples/ZoneWorld/run_sample.sh --g4-child` | G4 실패 재현; fresh Actor 24개는 새 RID에서 통과 |
| `git diff --check` | 통과 |

모든 build와 test는 `/tmp/zlink-cpp-gate.lock`을 사용했다. sample gate는
`/tmp/zlink-samples-gate.lock`도 함께 사용했다. 각 gate 전에 load average가 10 미만이고
`lto1` process가 없음을 확인했다.

## ZoneWorld G4 결과

- 전체 sample log: `/tmp/zlink-cpp-d131-samples.log`
- 보존된 run directory: `/tmp/tmp.gBsbLFar1J`
- 기존 mesh trace를 켠 G4 단독 log: `/tmp/zlink-cpp-d131-zoneworld-g4-trace.log`
- traced G4의 보존된 run directory: `/tmp/tmp.Shxay5Bu7g`
- runner 결과: `zoneworld-g4=failed reason=boundary-or-fresh-actor-proof`
- source node 결과: `zoneworld-join-failed player=player-g4-crash kind=7`
- `kind=7`: `DeadlineExceeded`
- client 결과: `stream connector wait timed out`
- client의 waiter는 `Unavailable`인 typed `CrashRelocationProbeRes`만 완료 조건으로 받는다.
  client log에는 수신한 다른 error kind가 기록되지 않으므로, client가 decode한 kind를 직접
  관찰했다고 보고할 수 없다. 확인 가능한 terminal kind는 source node callback의
  `DeadlineExceeded(kind=7)`이다.
- traced G4에서도 replacement는 새 RID로 기동했고 fresh Actor 24개를 수락했다. 실패는
  crash boundary에 한정됐다.
- source node trace는 target RID의 두 physical connection에서 monitor disconnect를 먼저
  기록했다. 그 뒤 ActorJoin correlation 11은 transport의 `route_unavailable` 결과를 199회
  받았고, durable retry가 deadline까지 계속된 뒤 `DeadlineExceeded`로 끝났다.

## BLOCKERS

- ZoneWorld G4 때문에 요구된 sample 7/7 gate가 완료되지 않았다. traced G4도 같은 결과였다.
  `raw_mesh_node_owner.cpp:199-210`은 한번 admitted된 durable request가 transport
  `route_unavailable`을 받으면 deadline까지 다시 제출하고 `:273-276`에서 `timed_out`으로
  끝낸다. source trace는 monitor disconnect 뒤 이 경로가 계속됐음을 확인했다.
- 승인된 D-131 회귀는 expectation을 먼저 제거하고 admitted peer를 나중에 제거하는 순서다.
  실제 G4는 peer를 먼저 제거했지만 expectation이 남아 있어 술어가 operation을 유지했다.
  `ZoneNode/main.cpp:83`의 request timeout과 `contracts/locations/options.hpp:21`의 owner lease
  TTL은 모두 15초이며 Location은 `:22`의 1초 polling 주기로 제거를 관찰한다. 이번 run에서는
  request deadline이 expectation 제거보다 먼저 도달했다.
- timeout, lease, retry, error remap 또는 sample predicate를 바꾸는 것은 이번 승인 범위를
  벗어난다. 이 동일-deadline 경계를 별도 결함으로 확정하고 class B 승인을 받지 않았으므로
  세 번째 runtime 변경이나 sample 우회는 적용하지 않았다.

Commit과 push는 수행하지 않았다.
