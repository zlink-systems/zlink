# Durable operation exhaustion kind·replay scope 교차언어 진단

## 결론

공통 sender 계약은 Framework durable lifecycle sender가 같은 `OperationId`의 terminal
envelope를 받을 때까지 남은 전체 deadline으로 replay하도록 요구한다. 한 번도 binding
request admission에 성공하지 못하면 `Unavailable`, 한 번이라도 admission된 뒤 terminal
envelope를 받지 못하면 `DeadlineExceeded`다.

현재 구현은 다음과 다르다.

| 구현 | pre-admission 소진 | admission 뒤 no-reply/handover replay | attempt deadline |
| --- | --- | --- | --- |
| .NET Actor Join/create | 최종 `DeadlineExceeded`로 합쳐진다 | native canonical Join/create 일부만 replay한다 | 고정 `ServiceTerminalRetryTimeout`으로 자른다 |
| Java Actor Join/create | 최종 `DeadlineExceeded`로 합쳐진다 | replay하지 않는다 | Join/create는 1회 남은 deadline, bound bind는 고정 2초다 |
| Java bound-session bind | 최종 `DeadlineExceeded`로 합쳐진다 | `RequestResult.TIMED_OUT`을 retry에서 명시적으로 제외한다 | 고정 2초다 |
| Node STREAM actor bind | 동기 `SubmitResult.NotConnected` 소진을 `DeadlineExceeded`로 바꾼다 | 비동기 completion을 retry하지 않고 모든 nonzero terminal을 `Unavailable`로 합친다 | 호출 시점의 남은 deadline을 전달한다 |
| C++ Actor create/bound-session bind | `Unavailable`로 올바르게 구분한다 | `route_unavailable`만 retry하고 `timed_out`은 즉시 종료한다 | 남은 deadline 전부를 전달한다 |

네 항목 모두 변경 분류는 **A — 갱신된 공통 계약 적응**이다. replay 범위는
`3a2ada8187`, exhaustion kind는 `662e1915366`에서 명시됐다. C++의 선행
exhaustion-kind 수정은 기존 error model을 어긴 결함이라 B였지만, 이 문서가 다루는 C++
잔여 범위는 새 sender replay 조항에 대한 A다. C/D 우회나 spec gap은 아니다.

조사는 2026-09-05 현재 `HEAD`를 기준으로 했다. 동시에 수정 중인 .NET mesh node와 Node
runtime/test는 working tree 대신 `git show HEAD:<path>`로 읽었다. Core·binding·runtime·spec은
수정하거나 다시 빌드하지 않았다.

## 판정 기준과 계층 소유권

- Actor model
  `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680`:
  Actor create·join과 session bind만 Framework sender가 같은 `OperationId`로 replay한다.
  route 부재, handover 1회 timeout, reply 유실은 모두 terminal envelope 부재이며, 각
  attempt는 남은 deadline 전부를 쓴다. application request는 replay하지 않는다.
- Error model
  `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:75-85`:
  사용할 route가 없으면 `Unavailable`, admitted request의 reply가 deadline 안에 없으면
  `DeadlineExceeded`다.
- **Binding 소유:** Core DONTWAIT admission, WRITABLE 재제출과 그 결과를 typed
  `SubmitResult`/submit exception으로 구분한다. `SubmitResult.Ok`가 request admission을
  표시한다. 그 뒤 reply·timeout·terminal은 typed `RequestResult`/request exception이다.
- **Framework 소유:** durable operation의 stable `OperationId`, terminal-record replay,
  전체 deadline, `admittedOnce` 누적과 최종 public error kind를 소유한다.
- Framework는 topology/monitor로 admission을 추정하거나 submit/request phase를 숫자 하나로
  다시 합치면 안 된다. Binding이 분류한 phase를 소비해야 한다.

## .NET — Actor Join과 Actor create

이 절의 `Runtime/...` 축약 경로는
`framework/languages/dotnet/src/Zlink.Framework/Runtime/`, test 파일명은
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/` 기준이다.

### Retry predicate·exhaustion mapping

Actor Join:

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs:180-209`:
  outer cancellation과 admission 전 deadline을 모두 `DeadlineExceeded`로 만든다.
- 같은 파일 `:213-222,490-570`: `Unavailable` 또는 submit
  `NotConnected`/`Backpressured`만 reconciliation retry 대상으로 삼는다.
- 같은 파일 `:543-555`: request timeout 뒤 current `MeshStatus`/`MeshPeers`가 snapshot과
  다르면 `Unavailable`로 바꾼다. Binding phase 대신 topology로 admission을 재추정하는
  중복 분류다.
- 같은 파일 `:1050-1076`: canonical backend가 `false` 또는 `RequestResult`만 돌려준다.
  `IZLinkBackendCanonicalActorJoin.RequestCanonicalActorJoin()`도
  `Runtime/Backend/Contracts/IZLinkBackendObjects.cs:85-111`에서 submit phase를 `bool`로
  축약한다.
- `Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs:155-174`는 실제
  `SubmitResult`를 `submit == SubmitResult.Ok`로 지운다.
- `Runtime/Service/ZLinkManagedMeshNode.cs:9639-9723`은 canonical Join의
  `ZlinkRequestException(TimedOut)`만 retry한다(`:9677-9684`). 전체 소진은 무조건
  `RequestResult.TimedOut`이다(`:9652-9657`). 각 attempt는 남은 deadline이 아니라
  `min(remaining, ServiceTerminalRetryTimeout)`이다(`:9663-9669`).

Actor create:

- `Runtime/Actors/ZLinkActorManagerService.cs:459-526`은 submit `NotConnected`와
  `Backpressured`를 재선택/retry하고, `:547-577`은 `Unavailable`·capacity·deadline을 다시
  후보 선택으로 보낸다.
- 같은 파일 `:191-220,582-589`는 그 retry가 deadline을 소진하면 원인과 무관하게
  `DeadlineExceeded`로 만든다.
- `Runtime/Service/ZLinkManagedMeshNode.cs:9726-9810`은 commands 47–49의 request timeout을
  retry하지만(`:9762-9771`) 전체 소진은 `RequestResult.TimedOut`이고(`:9736-9742`),
  attempt timeout을 고정 구간으로 자른다(`:9748-9754`).
- `Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs:555-616`은 호출마다 새
  operation/correlation을 할당한다(`:570-571`). 따라서 manager-level 재선택은 native
  inner retry가 보존하는 동일 wire/operation과 달리 stable backend operation을 보장하지
  않는다.

최종 mapper 자체는 typed 값을 올바르게 번역한다.

- `Runtime/Messaging/ZLinkSubmitFailureMapper.cs:25-41`: submit `NotConnected` →
  `Unavailable`.
- `Runtime/Messaging/ZLinkRequestFailureMapper.cs:86-101`: request `TimedOut` →
  `DeadlineExceeded`, request `NotConnected` → `Unavailable`.
- Actor create coarse terminal은
  `Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs:1555-1574`에서 같은
  구분을 한다. 결함은 mapper 앞에서 phase/history를 버린 데 있다.

### Binding admission 표지

`bindings/dotnet/src/Zlink/Contracts/Errors/SubmitResult.cs:8-26`의 `SubmitResult.Ok`가
admission 성공이다. `.Async()`가 admission 전에 실패하면 `ZlinkSubmitException`과
`SubmitResult`, admission 뒤에는 `ZlinkRequestException`과 `RequestResult`가 관측된다.
`bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs:212-222`의
`RequestResult.TimedOut`은 reply가 오지 않은 completion이다.

그러나 `ZLinkManagedMeshNode.cs:2351-2361`은 native submit 실패도 managed operation을
`ToRequestResult(submit)`로 완료한 뒤 `SubmitResult.Ok`를 반환한다. 이 지점과 위 `bool`
backend 경계가 binding의 admission 분류를 잃는다.

### 3행 행렬

| 상황 | Actor Join 현재 | Actor create 현재 | Spec |
| --- | --- | --- | --- |
| route absent가 전체 deadline 동안 지속 | reconciliation/outer deadline이 최종 `DeadlineExceeded`; canonical `bool false`도 phase를 남기지 않는다 | submit `NotConnected`는 중간에 `Unavailable`이지만 manager가 재선택하다 outer deadline에서 `DeadlineExceeded`; 호출별 operation도 바뀔 수 있다 | 같은 `OperationId`로 replay, `admittedOnce=false`이므로 `Unavailable` |
| admitted 뒤 reply 없음 | canonical native path는 timeout을 replay하고 최종 `DeadlineExceeded`; legacy routed path는 timeout 자체는 replay하지 않으며 topology가 바뀐 경우에만 `Unavailable`로 재분류 | native inner path는 같은 wire를 retry하고 최종 `DeadlineExceeded` | terminal envelope 전까지 replay, 최종 `DeadlineExceeded` |
| handover 1회 timeout | canonical native path만 retry; legacy path는 위 topology 추정 또는 즉시 deadline failure | native inner path는 retry | 같은 `OperationId`로 남은 deadline 전부를 사용해 retry; 이후 소진 시 이미 admitted이므로 `DeadlineExceeded` |

### 최소 수정과 회귀

소유자는 .NET Framework의 durable Actor lifecycle sender다. Public API를 늘리지 말고 내부
attempt 결과에 binding의 submit/request phase를 보존한다. Join/create 모두 하나의 stable
`OperationId`와 encoded request를 사용하고, `admittedOnce`를 누적하며, timeout slicing을
제거한다. terminal envelope만 retry를 중단한다. `ZLinkActorRemoteJoiner.cs:543-555`의
topology 기반 재분류는 제거하고 binding phase를 소비한다. 이 helper를 application request에
적용하면 안 된다.

회귀는 다음 세 경계를 각각 고정해야 한다.

1. `CanonicalActorJoinIngressReplyTests.cs:133-230`의 same-operation lost-reply 회귀에
   attempt timeout이 매번 남은 전체 deadline인지 추가한다.
2. submit `NotConnected`만 지속되는 Join/create가 `Unavailable`이고 target ingress가 0회인지,
   한 번 admitted된 뒤 withheld reply는 `DeadlineExceeded`인지 검사한다.
3. handover timeout 뒤 두 번째 attempt가 같은 `OperationId`/wire를 쓰고 terminal envelope를
   받으면 더 retry하지 않는지 검사한다. create는
   `StatefulServiceRuntimeTests.cs:3290-3348`의 remote create fixture를 확장할 수 있다.

분류: **A — 갱신된 durable sender 계약 적응**.

## Java — Actor Join, Actor create, bound-session bind

이 절의 runtime 파일명은
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/`,
test 파일명은 같은 module의 `src/test/java/systems/zlink/framework/runtime/` 기준이다.

### Retry predicate·exhaustion mapping

Actor Join:

- `ZLinkActorSpotJoinCall.java:225-248,692-717`은 admission 전 deadline을 바로
  `DeadlineExceeded`로 만들고 canonical request를 한 번만 보낸다.
- 같은 파일 `:701-715`는 admitted request의 `TIMED_OUT`을 current topology로 다시 검사해
  target이 더 이상 admitted가 아니면 `Unavailable`로 바꾼다. Binding의 request phase를
  topology로 재분류하는 중복 상태다.
- `ZLinkJavaRawMeshNode.java:1373-1418`은 매 호출 새 correlation을 할당하고(`:1385`)
  `requestApplication()`을 한 번만 호출한다(`:1414`).
- entry path도 `ZLinkActorEntrySpotJoinCall.java:98-140`에서 resolve와 request가 1회이며,
  deferred deadline은 `:143-151`에서 무조건 `DeadlineExceeded`다.

Actor create:

- `ZLinkActorCreationCoordinator.java:141-168,184-200,775-804`는 route/target을 기다리다가
  deadline이 끝나면 `deadlineFailed()`로 간다. `:1054-1058`은 원인과 무관하게
  `DeadlineExceeded`다.
- 같은 파일 `:272-300`은 stable creation identity를 intent에 넣지만
  `node.requestActorCreate()`를 한 번만 호출한다. 실패 뒤 terminal record를 한 번 읽고 없으면
  원래 실패를 그대로 끝낸다.
- `ZLinkJavaRawMeshNode.java:3410-3500`도 request를 한 번만 보낸다(`:3485-3499`). route
  preflight 실패를 typed submit failure가 아닌 `IllegalStateException`으로 만든다
  (`:3449-3466`).

Bound-session bind:

- `ZLinkActorRetryScheduler.bindRelayUntilAccepted()`
  (`ZLinkActorRetryScheduler.java:148-192`)이 retry owner이고, `:171-180`에서 모든 retryable
  소진을 `DeadlineExceeded`로 만든다.
- `ZLinkActorSubmitFaults.java:40-56`은 submit `NOT_CONNECTED` 등을 retry하지만 admitted
  completion인 `RequestResult.TIMED_OUT`은 제외한다. 반대로 sibling session-actor bind는
  `:28-38`에서 timeout을 retry한다.
- `ZLinkBoundSessionRuntime.java:124-143`은 route-ready 전 소진도 `DeadlineExceeded`로
  만들고, `:163-174`는 각 attempt에 남은 deadline 대신 고정 2초를 전달한다.

`ZLinkBackendRequestResult.java:30-46`은 coarse `TIMED_OUT` → `DeadlineExceeded`,
`NOT_CONNECTED` → `Unavailable`을 올바르게 번역한다. 결함은 그 전에 admission history가
없거나 one-shot으로 끝나는 데 있다.

### Binding admission 표지

`bindings/java/src/main/java/systems/zlink/contracts/sockets/SocketEnums/SubmitResult.java:7-25`의
`OK`가 admission 성공이다.
`bindings/java/src/main/java/systems/zlink/contracts/messaging/RequestSubmitOperation.java:30-42`는
각 admission attempt가 Core DONTWAIT이고, immediate
admission 뒤에 reply timeout이 시작되며 returned stage가 typed request terminal로
완료된다고 명시한다. 따라서 `ZlinkSubmitException(SubmitResult)`은 미수락,
`ZlinkRequestException(RequestResult)`은 admission 뒤 completion이다
(`bindings/java/src/main/java/systems/zlink/contracts/sockets/SocketEnums/RequestResult.java:7-21`).

Java raw port는 이 구분을 유지한다. `ZLinkJavaRawServicePort.java:144-192`에서
`.submit()` 전후의 typed exception이 그대로 stage에 남는다. 이를 상위 Join의 topology
재분류, create의 `IllegalStateException`, bind helper의 단일 `Throwable` predicate가
소비하지 못한다.

### 3행 행렬

| 상황 | Actor Join 현재 | Actor create 현재 | bound-session bind 현재 | Spec |
| --- | --- | --- | --- | --- |
| route absent가 전체 deadline 동안 지속 | pre-admission guard가 `DeadlineExceeded`; request 자체는 one-shot | target 선택/ready 대기 소진이 `DeadlineExceeded`; raw preflight는 untyped `IllegalStateException` | route-ready 또는 bind retry 소진이 `DeadlineExceeded` | 같은 `OperationId` replay 후 `Unavailable` |
| admitted 뒤 reply 없음 | `RequestResult.TIMED_OUT` 뒤 replay 없음; completion kind는 deadline 계열 | request 1회 뒤 terminal-record read만 하고 replay 없음; timeout kind는 deadline 계열 | `TIMED_OUT`을 predicate가 제외하므로 replay 없음 | terminal envelope 전까지 replay, 소진은 `DeadlineExceeded` |
| handover 1회 timeout | replay 없이 topology가 바뀌었으면 `Unavailable`로 재분류할 수도 있다 | replay 없음 | replay 없음 | 같은 `OperationId`와 남은 전체 deadline으로 replay, 최종 `DeadlineExceeded` |

### 최소 수정과 회귀

소유자는 Java Framework의 Actor lifecycle sender와 bound-session durable sender다. 공통 내부
attempt outcome에 submit/request phase와 `admittedOnce`를 보존하고, Join/create/bind가 stable
`OperationId`와 encoded request를 재사용하게 한다. `TIMED_OUT`을 terminal-envelope 부재로
retry하되 terminal envelope는 즉시 중단한다. `ZLinkBoundSessionRuntime`의 고정 2초를 남은
전체 deadline으로 바꾸고, Join의 topology timeout 재분류와 create의 untyped preflight를
typed phase 소비로 대체한다. Generic application request에는 연결하지 않는다.

회귀는 다음 위치가 적합하다.

1. `ZLinkActorRetrySchedulerTest.java:102-122,141-177`의 “모든 소진은 deadline” 기대를
   never-admitted=`Unavailable`, admitted-timeout=`DeadlineExceeded` 두 case로 분리한다.
2. `ZLinkActorSubmitFaultsTest.java:32-45`의 bound-session `TIMED_OUT` non-retry 기대를 durable
   replay 기대와 stable operation 검증으로 교체한다.
3. `ZLinkJavaRawMeshNodeCanonicalActorJoinTest`와
   `ZLinkActorCreationCoordinatorTargetSelectionTest`에 세 행을 추가해 동일 operation identity,
   full-remaining timeout, target 실행 0/1회, terminal envelope 뒤 retry 중단을 검사한다.

분류: **A — 갱신된 durable sender 계약 적응**.

## Node — STREAM actor bind

이 절의 package·test 축약 경로는 `framework/languages/node/`, binding 경로는 repository
root 기준이다.

### Retry predicate·exhaustion mapping

- `packages/framework/src/runtime/streams/managed-stream.ts:442-485`가 STREAM native actor-bind
  retry loop다. `isBackendNotConnectedError()`만 retry하며(`:469-472`) deadline 소진을
  무조건 internal `DeadlineExceeded`로 만든다(`:473-480`). 각 호출에는 남은 deadline을
  전달하는 점(`:459-468`)은 계약과 같다.
- `return completions.submit(...)`을 `await`하지 않으므로(`:459`) `try/catch`는 동기
  `service.bindActor()` failure만 본다. Promise rejection이나 completion terminal은 retry
  predicate에 도달하지 않는다.
- 같은 파일 `:421-440`은 모든 nonzero `terminalResult`를 internal
  `RouteNotConnected`로 바꾼다. 따라서 admitted `RequestResult.TimedOut`도 public
  `Unavailable`로 잘못 축약된다.
- `runtime/backend/runtime-values.ts:44-69`의 `ZLinkBackendResultError`는 원래
  `operation: 'submit' | 'request'`를 갖지만, `isBackendNotConnectedError()`의 structural
  fallback은 operation을 보지 않고 submit 값 2와 request 값 109를 같은 predicate로 묶는다.
- remote bind는 `service-stateful-runtime.ts:1623-1753`에서 매 호출 새 pending operation과
  binding generation을 만들고, `:1716-1722`의 header를
  `submitRequest(..., 'streamBind')`에 1회 넘긴다. `:4189-4229`의 remote request도 1회다.
  따라서 `managed-stream` 바깥에서 단순 재호출하면 stable `OperationId`를 잃는다.
- `backend/node/node-raw-mesh-backend.ts:2479-2492`는 backend error의
  submit/request `operation`을 버리고 숫자 `terminalResult`만 남긴다. 이는 binding
  classification을 중복 저장하는 대신 실제 phase 정보를 소실한다.

### Binding admission 표지

`bindings/node/src/zlink/contracts/errors/results.ts:4-38`의 `SubmitResult.Ok`가 admission
성공이고, 뒤의 `RequestResult.TimedOut`은 request completion이다. 더 직접적으로
`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:289-365`는 native
`SubmitResult.Ok`일 때만 request completion ID를 publish한다(`:317-333`); 그 전 실패는
`SubmitError`다(`:336-341`). admission 뒤 native terminal은 같은 파일 `:143-164`에서
`RequestError`가 된다. Binding의 WRITABLE retry도 `:596-615`에서 `SubmitResult.Ok` 뒤에만
completion으로 전환한다.

즉 Node Framework는 binding의 `SubmitError`/`RequestError` 또는
`ZLinkBackendResultError.operation`을 admission 표지로 소비해야 한다. 숫자
`terminalResult`만으로 재구성하면 안 된다.

### 3행 행렬

| 상황 | 현재 | Spec |
| --- | --- | --- |
| route absent가 전체 deadline 동안 지속 | 동기 submit `NotConnected`는 retry하지만 최종 `DeadlineExceeded`. 비동기 `RequestResult.NotConnected` 모양으로 오면 retry 없이 `Unavailable`라서 같은 원인의 경로별 결과도 다르다 | same `OperationId` replay 후 `admittedOnce=false`이므로 `Unavailable` |
| admitted 뒤 reply 없음 | 비동기 `TimedOut`은 loop 밖으로 빠지고 `requireSuccessfulCompletion()`이 `RouteNotConnected`/`Unavailable`로 합친다 | replay 후 소진은 `DeadlineExceeded` |
| handover 1회 timeout | 위와 같이 replay하지 않고 `Unavailable`로 합친다 | 같은 `OperationId`와 남은 전체 deadline으로 replay; 소진은 `DeadlineExceeded` |

### 소유권·최소 수정·회귀

소유자는 `service-stateful-runtime`의 durable `streamBind` sender여야 한다. 여기가 이미
pending ID와 encoded header를 소유하므로 같은 header로 replay하며 `admittedOnce`를 누적할
수 있다. `managed-stream`에서 `service.bindActor()`를 반복 호출해 새 pending ID/generation을
만드는 수정은 금지 패턴에 해당한다. Binding의 typed phase를 보존해 terminal envelope 없는
submit failure와 admitted timeout을 모두 replay하고, 전체 소진에서만 phase에 따라 public
kind를 정한다. `managed-stream.ts:421-440`의 blanket `RouteNotConnected` mapping도 typed
request terminal mapper로 교체한다. application request 경로는 건드리지 않는다.

회귀는 `test/contract/stream-runtime.test.js:133-245,279-335`를 기반으로 한다.

1. 현재 `:279-335`의 pre-admission 소진 기대를 `DeadlineExceeded`에서 `Unavailable`로
   바로잡고 bind target ingress가 없었음을 검사한다.
2. admitted `RequestResult.TimedOut` 뒤 성공 terminal을 주어 두 attempt가 같은
   operation ID/header/binding generation인지 검사한다.
3. handover 1회 timeout 뒤 성공과, terminal envelope 뒤 추가 submit이 없는 case를 추가한다.
   각 attempt에 전달된 timeout은 호출 시점의 남은 전체 deadline이어야 한다.

분류: **A — 갱신된 durable sender 계약 적응**.

## C++ — `raw_mesh_node_owner` replay scope

이 절의 `framework/...`와 test 파일명은 `framework/languages/cpp/`, binding 경로는
repository root 기준이다.

### Retry predicate·exhaustion mapping

- `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:193-243`의 retry state는
  `raw_request_result_t::route_unavailable`만 retry한다(`:227-230`). `timed_out`은 즉시
  `operation_terminal_t::timed_out`으로 끝낸다(`:232-243`).
- 같은 파일 `:193-204`는 각 attempt에 남은 deadline 전부를 전달하고,
  `:268-292`는 route retry가 전체 deadline을 소진하면 `route_unavailable`을 보낸다.
- Actor create는 같은 파일 `:1967-2060`, bound-session bind는 `:1869-1931`에서 이 helper를
  사용한다. 둘 다 retry 동안 같은 registered operation, correlation과 encoded request를
  유지한다.
- public mapping은 Actor create에서
  `framework/src/runtime/mesh/mesh_node_host_service.cpp:350-365`와
  `user_spot_terminal_mapping.hpp:83-102`, bound-session bind에서
  `mesh_node_runtime.cpp:3816-3857`에 있다. `timed_out` → `DeadlineExceeded`,
  `route_unavailable` → `Unavailable`이다.

### Binding admission 표지

`bindings/cpp/include/zlink/Contracts/Sockets/results.hpp:72-89`의
`submit_result_t::ok`가 admission 성공이고,
`Contracts/Messaging/request_result.hpp:10-25`는 그 뒤 completion이다.

C++ adapter는 이 경계를 이미 가장 정확히 보존한다.

- `raw_route_port.hpp:41-69`의 `raw_request_failure_t`가
  `initial_admission`/`completion_terminal`, submit result와 request result를 함께 가진다.
- `raw_route_port.cpp:133-177`은 `.async()`의 submit exception과 `co_await` 뒤 request
  exception을 분리한다.
- `raw_binding_adapter.hpp:94-108`은 initial typed submit+errno만
  `route_unavailable`로 분류한다. retry owner는 errno나 topology를 다시 읽지 않으므로
  binding classification 중복은 없다.

현재 문제는 이 phase를 잃은 것이 아니라 retry predicate가 completion timeout을 허용하지
않고 `admittedOnce`를 누적하지 않는 것이다. 여기서 admission 근거는 단순히
`completion_terminal` phase가 아니라 그 안의 typed `request_result`다. WRITABLE wait가
admission 전에 끝나면 completion-phase `submit_result`일 수 있으므로 admitted로 세면 안 된다.

### 3행 행렬

| 상황 | 현재 | Spec |
| --- | --- | --- |
| route absent가 전체 deadline 동안 지속 | 같은 operation/correlation로 replay하고 `route_unavailable` → `Unavailable` | 일치 |
| admitted 뒤 reply 없음 | `timed_out`에서 즉시 종료; 최종 kind만 `DeadlineExceeded`로 일치하고 replay scope는 불일치 | terminal envelope 전까지 replay, 소진은 `DeadlineExceeded` |
| handover 1회 timeout | `timed_out`에서 즉시 종료해 두 번째 attempt 없음 | 같은 `OperationId`와 남은 전체 deadline으로 replay, 소진은 `DeadlineExceeded` |

### 소유권·최소 수정·회귀

소유자는 기존 `infrastructure_request_retry_state_t`다. typed
`request_result_t::timed_out`을 terminal-envelope 부재로 retry predicate에 포함하고, 처음
`failure.request_result`가 관측될 때 `admittedOnce=true`를 누적한다. 이후 route 부재만 이어지다 전체 deadline이
끝나도 `admittedOnce`가 true면 `timed_out`, 끝까지 false면 `route_unavailable`로 완료한다.
기존 `_operation`, `_correlation`, `_request_parts`를 그대로 재사용하고 application request
helper에는 적용하지 않는다. Adapter의 phase/errno 표는 바꿀 필요가 없다.

회귀는 다음 기존 경계를 확장한다.

1. `test_cpp_framework_m6a_runtime.cpp:371-406`의 Actor-create permanent route absence는
   동일 request identity와 최종 `Unavailable`을 계속 검사한다.
2. `test_cpp_framework_m6b_runtime.cpp:1305-1376`의 bound-session withheld-reply case는
   admitted 소진의 최종 `DeadlineExceeded` 경계를 계속 검사한다.
3. 별도 deterministic handover fixture에서 전체 deadline 전 발생한 1회 typed timeout 뒤
   두 번째 ingress의 operation/correlation이 같고, terminal envelope가 도착하면 추가
   attempt가 없으며 target execution이 terminal record에 의해 1회인지 검사한다. attempt
   timeout은 매번 남은 전체 deadline이어야 한다.

분류: **A — 새 sender replay 범위에 대한 기존 C++ retry owner의 계약 적응**.

## Stage 2 경계

Stage 2는 네 언어 모두 binding/Core를 바꾸지 않고 Framework durable sender에서 해결할 수
있다. 공통 최소 조건은 다음과 같다.

1. Binding typed phase만 admission 근거로 사용한다.
2. 같은 `OperationId`와 encoded request를 유지한다.
3. terminal envelope가 없는 route absence·handover timeout·reply loss만 replay한다.
4. 매 attempt에 남은 deadline 전부를 준다. timeout/budget/attempt 횟수를 늘리지 않는다.
5. `admittedOnce=false` 소진은 `Unavailable`, true 소진은 `DeadlineExceeded`다.
6. Application request에는 이 replay를 적용하지 않는다.

여기서 진단을 종료한다. 코드·assertion·deadline·fixture는 변경하지 않았다.
