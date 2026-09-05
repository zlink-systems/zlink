# .NET durable sender Stage 2 결과

승인된 A 범위의 sender 변경과 회귀 행렬은 구현했다. 새 회귀 16개는 통과한다.
Reciprocal ROUTER의 공개 API 재현에서 reply 제출 성공 뒤에도 requester가
`TimedOut`으로 끝나는 Core/binding 경계 결함이 남아 있어 전체 완료 판정은 보류한다.
Commit과 Core/binding 재빌드는 수행하지 않았다.

## 소유권과 변경 분류

- 소유 계층: Framework sender가 같은 wire를 재전송하고 전체 deadline의 남은 시간을 계산한다. Binding의 typed 결과로 admission 여부를 누적하여 소진 시 error kind를 정한다. 물리 handover와 reply 전달은 Core/binding이 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680` sender replay, `00-foundation/07-framework-error-model.ko.md:75-85` exhaustion kind, `core/doc/spec/core/socket/README.ko.md:159-167` HANDOVER의 즉시 `REQUEST_NOT_CONNECTED` completion.
- 교차언어 대조: Java `ZLinkJavaDurableRequest`와 Node `requestDurableOperation`의 동일 wire·남은 전체 deadline·typed admission 누적 방식을 적용했다. .NET의 callback completion과 별도 expiry 작업 때문에 deadline owner를 Framework의 `ZLinkDurableRequest`로 모으는 배선 변경이 필요했다.
- 변경 분류: **A — 승인된 sender replay 계약 적응**. 하위 계층의 timeout을 topology로 재분류하는 보상은 적용하지 않았다.

## Diff

아래 runtime 경로는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/` 기준이다.

| 파일 | 변경 내용 |
| --- | --- |
| `Messaging/ZLinkDurableRequest.cs` | 동일 encoded wire, monotonic deadline, typed submit/request 이력을 한 sender가 유지한다. `NotConnected`와 timeout 뒤 남은 deadline으로 재전송하고, 한 번도 admission되지 않으면 `Unavailable`, admission 뒤 reply가 없으면 `DeadlineExceeded`로 끝낸다. |
| `Service/ZLinkManagedMeshNode.cs` | canonical/native Join, Actor create, User Spot create/close를 공통 sender에 연결한다. Durable operation의 별도 expiry 경쟁과 sender의 고정 5초 cut을 제거한다. Generation fence의 즉시 거부는 유지한다. |
| `Host/ZLinkActorRemoteJoiner.cs` | legacy admission도 encoded 요청을 고정한다. Topology에 따른 typed timeout 재분류를 제거한다. Admission은 원래 deadline을 사용하고 이후 relocation 단계는 기존 deadline cancellation을 사용한다. |
| `Actors/ZLinkActorManagerService.cs` | remote 호출마다 새 operation을 만드는 반복과 transport 실패에 따른 peer epoch 제외를 제거한다. Terminal 수신 뒤 다른 target으로 새 create를 시작하지 않는다. |
| `Spots/ZLinkSpotRuntimeManager.cs` | remote create/close의 별도 반복을 제거한다. Sender의 exhaustion 결과보다 outer deadline cancellation이 먼저 반환되지 않도록 caller cancellation을 전달한다. |
| `Messaging/ZLinkSubmitFailureMapper.cs` | mapped error의 inner exception에 typed submit 결과를 보존한다. |

Receiver의 reply 제출·정리 예산은 `NativeReplySubmissionTimeout`이라는 이름으로 유지한다.
Application request는 durable sender를 호출하지 않는다.
각 호출부의 반복을 유지하면서 predicate만 확장하는 대안은 identity와 admission 이력을
여러 곳에 남기므로 채택하지 않았다. 공통 sender가 이 상태와 종료 결정을 함께 소유한다.

테스트 변경은 `tests/Zlink.Framework.UnitTests/Runtime/DurableSenderRuntimeTests.cs`,
`DurableRequestTests.cs`, 기존 `StatefulServiceRuntimeTests.cs`의 partial 선언과
`Zlink.Framework.UnitTests.csproj`의 compile 등록이다. 기존 assertion과 deadline은
완화하지 않았다. 동시 작업자가 소유한 ClientServer runtime·test는 수정하지 않았다.

## 회귀 행렬

`DurableSenderPreservesExhaustionCauseAndOriginalOperation`은 실제 managed mesh와
native request를 사용한다. Route 준비 전 제출은 같은 operation이 존재하는 상태에서
연결을 추가한다. Binding의 typed submit/request `NotConnected` 주입은 별도
`DurableRequestTests`에서 wire의 참조 동일성과 남은 전체 timeout을 검사한다.

| Operation | 전체 deadline 동안 route 없음 | Admission 뒤 reply 보류 | 제출 뒤 route 준비 |
| --- | --- | --- | --- |
| Actor Join | PASS: `Unavailable`, ingress 0 | PASS: `DeadlineExceeded`, ingress 1 | PASS: 원래 2초 안에 완료, ingress 1 |
| Actor create | PASS: `Unavailable`, create 0 | PASS: `DeadlineExceeded`, create 1 | PASS: 원래 2초 안에 완료, create 1 |
| User Spot create | PASS: `Unavailable`, create 0 | PASS: `DeadlineExceeded`, create 1 | PASS: 원래 2초 안에 완료, create 1 |
| User Spot close | PASS: `Unavailable`, close 0 | PASS: `DeadlineExceeded`, close 1 | PASS: 원래 2초 안에 완료, close 1 |

추가 회귀 4개는 typed submit/request `NotConnected` 각각의 동일 wire 재전송,
후속 submit 실패 뒤에도 admission 이력 유지, caller cancellation과 untyped 오류의
즉시 종료를 검사한다. 8초 operation의 첫 attempt가 5초보다 큰 남은 deadline을
받는 assertion도 포함한다.

## 검증 결과

모든 build/test와 sample runner는 `/tmp/zlink-dotnet-gate.lock` 안에서 실행했다.
지정된 TMPDIR·package hash별 NUGET_PACKAGES·환경 변수를 사용했다.
Package SHA-256은 `e0d59ad1f17cf9c911db1c6e32170c37b8cba83849743ae0f98f03d859ea1a07`이다.
Package native와 `core/build-dev/lib/libzlink.so`의 SHA-256은 모두
`a19fc2194633424b117bed9e9aa8352ea6dd4310ab3bfa9554898bbfa388eda3`이다.

| 검증 | 결과 |
| --- | --- |
| 새 sender 회귀 | 16 passed / 0 failed |
| `ServiceRuntimeFoundationTests` | 59 passed / 0 failed |
| `ZLinkMeshPeerAdmissionTests` | 8 passed / 0 failed |
| Focused suite 합계 | `durable-focused.trx`: 123 passed / 7 failed / 130 total |
| `StatefulServiceRuntimeTests` | 같은 TRX의 해당 class만 집계하면 56 passed / 7 failed. Generation fence 수정 뒤 해당 test와 새 행렬은 통과한다. 알려진 5개 red와 public Spot manager red는 아래에 기록한다. |
| 변경 경계 최종 재검증 | 17 passed / 1 failed: `PublicSpotManagerUsesReserveAndRemoteUserSpotCommands` |
| Canonical family | 154 passed / 2 failed |
| 전체 unit gate 1회 | 1932 passed / 7 failed / 1939 total, 6분 50초. 요청한 `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` filter 사용 |
| TicTacToe 1회 | PASS, exit 0, `tictactoe-placement=completed` |
| GameQuest 1회 | PASS, exit 0, `gamequest-placement=completed` |

TRX와 console log는 `scratchpad/stage2-dotnet-durable/` 및
`scratchpad/stage2-dotnet-durable-*.log`에 보존한다. Sample evidence는
`scratchpad/stage2-dotnet-durable-samples/`에 보존한다.

## BLOCKERS

### Reciprocal request/reply의 Core/binding 결함

`scratchpad/dotnet-durable-public-repro/Program.cs`는 `Systems.Zlink` 공개 API만 사용한다.
같은 Context의 ROUTER 두 개에 `Handover=true`, `Mandatory=true`를 설정한다.
Source를 bind하고 아직 bind되지 않은 target endpoint로 connect한 다음 target을
bind하고 source endpoint로 connect한다. 양쪽 hello를 받은 뒤 source가 2초 request를
제출한다. Target은 `Received.Reply().Message(...).Submit()`으로 바로 응답한다.

`scratchpad/stage2-dotnet-durable-public-repro.log` 결과:

- Target이 5.58ms에 `Request`, RID `retention-source`를 받는다.
- Captured reply의 `Submit()`이 6.99ms에 성공한다.
- Source는 terminal reply나 즉시 `NotConnected`를 받지 못하고 2.009초에 typed `TimedOut`으로 끝난다.

따라서 Framework sender가 원래 deadline을 유지하면서 이 결과를 보상할 수 없다.
`RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`도 여전히
`CreateCount=0`이며, 보존한 임시 진단에서 약 2초 뒤 binding의 typed `TimedOut`을
관측했다. 임시 runtime 로깅은 제거했다.

감독이 이미 Core reciprocal 문제로 분류한 `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`,
`RemoteActorStaleAuthorityReturnsOneTerminalForTheOriginalOperation`,
`RemoteActorStaleAuthorityUsesTheActiveFollowerBeforeAStaleTerminal`,
`RemoteActorStaleAuthorityDisposesRejectedFollowerPartsAndReturnsOneStaleTerminal`과
Canonical `HandoverLeavesReplyRouteToCore`도 재현됐다.
`PublicSpotManagerUsesReserveAndRemoteUserSpotCommands`는 remote create를 기다리다
fixture의 cancellation으로 끝난다. 같은 하위 reply 문제인지 추가 확인이 필요하며,
별도 Framework 보상은 추가하지 않았다.
전체 gate의 추가 실패 `FrameworkHostAutomaticallyExecutesRemoteUserSpotCreateAndCloseAgainstAuthorityStore`
는 `StatefulServiceRuntimeTests.cs:2705`의 reciprocal peer admission 대기에서 30초 뒤
끝난다. Focused 실행에서는 통과했으며 durable operation 제출 전 실패다. 이 실패의
하위 계층 원인은 미확정으로 남긴다. 전체 gate의 실패 7개는 이 절의 Stateful test 7개다.

### 기존 lost-reply fixture의 계약 불일치

`CanonicalActorJoinIngressReplyTests.ManagedSource_Command28Request_Retries_Same_Correlation_After_Lost_Reply`
는 첫 reply를 보류하고, 8초 operation의 deadline보다 이른 7초 안에 새 ingress를
기다린다. 이 fixture는 기존 5초 attempt cut에 의존한다. 남은 전체 deadline을
사용하는 sender에서는 실제 `NotConnected` completion 없이 두 번째 attempt를
시작할 수 없다. 기존 assertion을 유지했으며 해당 test는 실패로 보고한다.
Core handover completion이 정상화되면 그 공개 동작으로 첫 attempt를 종료하는
fixture가 필요하다.
