# Bucket B — .NET reciprocal handover readiness

## 결론

Core의 `TimedOut(101)`은 HANDOVER 계약대로 패배 방향에 고정된 request가 자기 timeout으로
종결된 결과였다. .NET Framework의 문제는 그 방향을 유지하는 연결 intent를 닫지 않은 채 peer를
`Admitted`로 먼저 공개한 것이었다.

일반 Actor/Spot request 자동 재전송은 선택하지 않았다. 대신 reciprocal duplicate가 실제 ready까지
간 경우 패배 방향의 Application/Completion lane disconnect를 모두 처리한 뒤에만 survivor를
`Admitted`로 공개한다. 패배 outbound 연결 intent는 logical RID가 아니라 endpoint로 닫는다.

## Handover timing 근거

수정 전 기존 monitor/framework trace의 순서는 다음과 같았다.

1. 낮은 RID caller의 outbound winner가 `ConnectionReady`(`connection=6`, lane 0)였다.
2. 반대 방향 후보가 이어서 `ConnectionReady`(`connection=15`, lane 0)였고, Framework의 마지막
   transport-pair 관측도 이 후보로 바뀌었다.
3. duplicate admission은 RID 규칙으로 caller outbound/owner inbound를 골랐지만
   `mesh_peer_duplicate_retire_skip_transport`로 패배 outbound reconnect intent를 남겼다.
4. 양쪽 `AdmittedPeerCount == 1` 직후 caller가 request를 제출했다. owner의 terminal reply는
   winner Completion lane에서 성공했지만 request는 losing pair 경계에 걸려 `TimedOut(101)`로 끝났다.

`Admitted` 뒤 250 ms를 고정 대기해도 실패했으므로 시간 지연은 settlement 증거가 아니었다.
endpoint disconnect를 적용한 trace에서는 패배 pair의 lane 0/1 `Disconnected`를 처리하고 재-admission이
끝난 뒤 terminal이 도착했다. TCP 사례에서는 패배 outbound가 아예 `ConnectionReady`에 도달하지 않고
inbound survivor만 ready였으므로 기다릴 disconnect barrier가 없었다.

따라서 이 구현에서 `Admitted`는 다음을 뜻한다.

- duplicate admission이 낮은 RID outbound/높은 RID inbound survivor를 골랐다.
- Framework가 패배 outbound reconnect intent를 해제했다.
- 패배 방향이 `ConnectionReady`까지 갔다면 그 Application/Completion lane disconnect를 모두
  state lane에서 처리했다. ready가 아니었던 후보는 endpoint 취소만으로 settlement가 끝난다.

## 선택한 메커니즘과 계약 근거

- Core HANDOVER는 반대 방향 충돌에서 RID로 한 방향을 고르고, 패배 방향에 admit된 request를
  옮기지 않으며 caller가 handover 뒤 다시 보낸다고 정의한다
  (`core/doc/spec/core/socket/README.ko.md:155-165`).
- Framework topology는 automatic 연결을 낮은 RID 쪽만 시작하고, manual 양방향 경합도 duplicate-pipe
  admission 뒤 ready 연결 하나만 남겨야 한다
  (`framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md:688-700`).
- 반면 일반 Spot direct request는 실패 뒤 다른 Spot으로 자동 재전송하지 않으며 새 request는 별도
  operation이다
  (`framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:392-403`).
  Session/Actor binding도 timeout·cancellation·route failure 뒤 같은 request를 자동 재전송하지 않는다
  (`framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:626-634`).
- Actor creation처럼 durable terminal replay가 정의된 lifecycle operation은 source RID/lifecycle/
  `OperationId`로 terminal을 보존하고 재전송 때 새 correlation/reply route를 사용한다
  (`framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md:168-174`).

그러므로 이 결함에는 generic request resend가 아니라 settle-before-submit을 적용했다. Node의 반복
request도 `userSpotCreate | userSpotClose | actorCreate` lifecycle operation에만 한정되어 있다
(`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4000-4027`).

C++ parity는 다음과 같다.

- `verify_duplicate_connection_survivor_is_symmetric`은 낮은 RID outbound/높은 RID inbound를 검증한다
  (`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:515-563`).
- `verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor`는 양방향 연결 뒤 monitor와
  handshake event를 더 drain해도 선택된 survivor가 바뀌지 않는지 검증한다
  (같은 파일 `:769-850`).

## 변경 파일

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`
  - `:70-72`: monitor에서 ready까지 간 outbound endpoint/inbound RID를 보존한다.
  - `:8453-8483`: reciprocal duplicate 결정 시 survivor를 settlement 대기로 전환한다.
  - `:8542-8588`: settlement 전에는 `Admitted`를 공개하지 않고, 완료 경로를 하나로 모은다.
  - `:8767-8849`: `ConnectionReady`/`Disconnected`로 실제 패배 방향과 두 lane을 추적한다.
  - `:8955-9039`: disconnect queue를 먼저 처리한 뒤 survivor를 admit하고 monitor event를 공개한다.
  - `:11628-11722`: ready까지 간 패배 방향만 barrier를 만들고, 패배 outbound intent는
    `Disconnect(endpoint)`로 취소한다. `DisconnectRid`는 shared logical RID survivor를 닫으므로 쓰지 않는다.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshPeer.cs:38-41`
  - reciprocal handover settlement 상태, lane mask, 패배 방향/endpoint를 peer epoch에 둔다.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:880-952`
  - `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`를 추가했다. caller-first manual
    bilateral connect에서 양쪽 admission 직후 delay 없이 Actor request를 보내고 `Ok` terminal이 정확히
    한 번 오는지 검증한다. 기존 세 stale-authority assertion은 수정하지 않았다.

Core, bindings, 보호된 spec/site 문서는 수정하지 않았다. 임시 file sink와 monitor probe도 제거했다.

## 검증

모든 명령은 지정된 `TMPDIR`, `ZLINK_LIBRARY_PATH`, package hash 기반 `NUGET_PACKAGES`,
`UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1` 환경과
`flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용했다.

- 집중 회귀 7개(새 bilateral test, stale-authority 3개, relocation relay 2개, TCP framework-host
  bilateral admission): **7/7 통과**.
- `--filter 'FullyQualifiedName~StatefulServiceRuntimeTests.RemoteActorStaleAuthority'` 3회:
  **각 3/3, 합계 9/9 통과**.
- `--filter 'FullyQualifiedName~StatefulServiceRuntimeTests'`: 최종 **50/50 통과**. 직전 실행 한 번은
  `RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`의 최초 submit이
  `NotConnected`여서 49/50이었으나, 해당 test 단독 3회는 3/3 통과했고 suite 재실행도 50/50이었다.
- `ManagedNode_ReadmitsPeerAfterLivenessExpiry` 단독: **1/1 통과**.
- `tests/Zlink.Framework.UnitTests` 전체: 구현 중 candidate에서 한 번 실행했으나 최종 집계를 만들지
  못했다. 39분 44초까지 아래 4건을 출력한 뒤 45분 이상 testhost가 종료되지 않아 SIGINT로
  중단했다. 이 결과 뒤 settlement 범위를 ready까지 간 패배 방향으로 좁혔으므로 최종 구현의 full
  gate 결과로 간주하지 않는다. 1.5시간 작업 상한 안에서 최종 코드는 위 집중/Stateful gate로 검증했다.
  - `ProviderLocationRepositoryAuthorityTests.SharedOpaqueProvider_ConcurrentDistinctCreationCompletionsRetryCapacityContention`:
    expected `Created`, actual `Stale`.
  - `ServiceRuntimeFoundationTests.ManagedNode_Tcp_SameEndpoint_Replacement_RemainsAdmitted_Across_Repeated_Lifecycles`:
    admission timeout.
  - `ServiceRuntimeFoundationTests.ManagedNode_ReadmitsPeerAfterLivenessExpiry`: admission timeout. 최종 구현의
    단독 재검증에서는 1/1 통과했다.
  - `CanonicalActorJoinIngressReplyTests.ActorCreateCompletion_AfterHandoverHello_UsesCapturedReplyRoute`:
    route admission/reply timeout. 이 test 하나가 36분 3초를 소비했다.

기존 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 nullability warning은 계속 보였으며 범위 밖이라 수정하지
않았다. request submit에서 D-B85 관련 `Backpressured`는 관찰하지 않았다.

## BLOCKERS / spec gap 제안

- 전체 unit gate는 위 장기 hang 때문에 최종 passed/failed count가 없다. 요청 범위의 최종 집중 gate와
  Stateful suite는 모두 통과했지만 전체 gate 완료 여부는 blocker로 남는다.
- topology의 “ready 연결 하나”는 Core가 anonymous standby를 내부에 보존할 수 있는지, Framework가
  패배한 local reconnect intent를 언제 취소해야 하는지, `Admitted`를 어느 monitor barrier 뒤에 공개할지
  명시하지 않는다. 보호된 spec에는 변경하지 않았으며 이 세 항목을 명문화하는 것을 제안한다.
- Core 문구의 “Caller는 handover 뒤 다시 보낸다”와 Framework의 generic request 자동 재전송 금지 사이에
  해석 여지가 있다. generic Actor/Spot/Session은 application이 새 operation으로 재시도하고, durable
  terminal replay가 있는 lifecycle operation만 Framework가 같은 operation을 재전송할 수 있다고
  명시하는 보완을 제안한다.
