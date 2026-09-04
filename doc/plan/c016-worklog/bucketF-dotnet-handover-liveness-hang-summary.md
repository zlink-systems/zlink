# Bucket F — .NET fixed-RID handover liveness hang

## 결론

`RouteAdmission_HandoverStartsFreshLivenessDeadline`의 실패와 뒤이은 inactivity hang은
reciprocal settlement mask가 남은 결과가 아니었다. 이 test의 source는 raw DEALER 하나뿐이라
반대 방향 Framework peer가 없고, `BeginReciprocalHandoverSettlement` 호출 조건인 direction
inequality에 도달하지 않았다.

실제 원인은 test fixture가 새 DEALER의 같은 RID handover를 시작한 뒤 이전 DEALER의 reconnect
intent를 유지한 것이다. 새 Hello가 Admit을 받아도 이전 DEALER와 replacement가 같은 RID를
반복해서 다시 점유했고, target은 exact disconnect마다 현재 inbound peer를 제거했다. 이는 fixed-RID
handover가 이전 pipe 종료를 확인해야 한다는 계약
(`framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:303-313`)의 세 번째
조건과 맞지 않았다.

Framework의 reciprocal settlement 구현은 바꾸지 않았다. 실제 bilateral trace에서는 loser가
ready인 쪽만 barrier를 시작했고 Application/Completion lane 0과 1의 disconnect 뒤
`mesh_peer_handover_settled`가 기록됐다. 따라서 same-RID raw handover를 reciprocal barrier로
분류하는 변경은 bilateral caller-first 동작을 다시 열 위험이 있었다.

## 재현 stack과 trace

지정된 `RouteAdmission` filter와 3분 blame timeout으로 재현했다.

- `RouteAdmission_HandoverStartsFreshLivenessDeadline` 본문은 16초에
  `AdmittedPeerCount expected 1, actual 0`으로 먼저 실패했다
  (`CanonicalActorJoinIngressReplyTests.cs:652`).
- 그 뒤 testhost가 종료되지 않았고 3분 dump
  `tests/Zlink.Framework.UnitTests/TestResults/dcd36b29-6d26-4c73-8c15-761673518a99/dotnet_33690_20260905T035608_hangdump.dmp`
  를 만들었다.
- dump의 managed stack은
  `ConnectedRuntime.DisposeAsync` → `Systems.Zlink.Context.DisposeAsync` →
  `Context.Dispose` → native `zlink_ctx_term`에서 정지했다. 즉 20분 inactivity는 assertion의
  3초 대기가 아니라 실패 후 context teardown 정지였다.
- 같은 stack은 supervisor dump
  `tests/Zlink.Framework.UnitTests/TestResults/b1610869-dc8d-4955-b796-7242e7eaf9f9/dotnet_62877_20260905T034650_hangdump.dmp`
  에서도 확인했다.

임시 file sink로 보존한 타임스탬프 trace는
`/tmp/bucketF-liveness-timed-trace.log`다.

- `:1-5`: 최초 Hello가 admission되고 pair `(10, lane 0)`이 ready가 됐다.
- `:8-12`: handover 중 replacement 후보 `(16, lane 0)`과 기존 pair `(10, lane 0)`이 모두
  disconnect되고 기존 inbound peer가 제거됐다.
- `:13-17`: Hello 재전송은 새 peer를 Admit하고 `(22, lane 0)`을 ready로 만들었다.
- `:18-22`: 0.1초 뒤 같은 RID 연결 경합이 다시 두 disconnect를 만들고 새 peer도 제거했다.
- trace 전체에 `mesh_peer_handover_settled` 또는 settlement begin은 없었다.

대조 trace `/tmp/bucketF-bilateral-trace.log:14-31`은 reciprocal case가
`BeginReciprocalHandoverSettlement`에 들어간 뒤 lane 0/1을 모두 관측하고 정상 settle한 사실을
보여 준다. 관련 구현 지점은
`ZLinkManagedMeshNode.cs:8469-8499`, `:8865-8891`, `:11673-11698`이다.

## 수정과 회귀

변경 파일은
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs`
하나다.

- `:630`: stale prior-Hello 회귀는 이전 raw DEALER의 reconnect를 잠시 중지한 상태에서 replacement
  Admit을 확인한다. 이후 prior Hello를 보낼 때 reconnect를 명시적으로 다시 켜므로 기존 stale
  ingress 검증을 유지한다.
- `:649`: liveness 회귀는 이전 DEALER를 종료한 뒤 replacement Hello/Admit을 다시 확인한다. 따라서
  fresh deadline assertion은 계약의 “이전 pipe 종료 확인” 뒤에 실행된다.
- `:1149-1183`: 기본 handover 경로는 변경 전 동작을 그대로 유지한다. opt-in
  `quiescePrior`/`disconnectPrior` 경로만 retired reconnect intent를 중지한다.
- `:1210-1240`: handover 중 도착할 수 있는 liveness probe를 Admit으로 오인하지 않고, 실제
  RouteAdmission `Admit`을 받을 때까지 2초 예산 안에서 Hello를 재전송한다.

assertion과 timeout은 낮추지 않았다. Core, bindings, 다른 언어, 보호된 spec 문서는 수정하지
않았고 임시 file sink와 monitor trace 코드는 제거했다.

## 검증 결과

모든 실행은 지정된 `TMPDIR`, local Core `ZLINK_LIBRARY_PATH`, package hash 기반
`NUGET_PACKAGES`, shared compilation/node reuse 비활성화와 gate lock을 사용했다.

- `--filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests.RouteAdmission'` 3회:
  각 **2/2**, 합계 **6/6 통과**. 최종 cleanup 축소 뒤 추가 실행도 **2/2 통과**했다.
- `--filter 'FullyQualifiedName~ZLinkMeshPeerAdmissionTests'`: **7/7 통과**.
- `--filter 'FullyQualifiedName~StatefulServiceRuntimeTests'`: **50/50 통과**. 이 묶음에
  `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`와 세
  `RemoteActorStaleAuthority*`가 포함된다.
- `git diff --check`: **통과**.

기존 범위 밖 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning은 그대로다.

## BLOCKERS

- 요청된
  `FullyQualifiedName~CanonicalActorJoinIngressReplyTests&FullyQualifiedName!~ActorCreateCompletion_AfterHandoverHello`
  3회 gate는 완료하지 못했다. 첫 실행은 14개 중 13개가 통과하고
  `CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`가 `:560`의 2초
  wait에서 실패했다. 별도 focused 재실행은 45초 inactivity 뒤 context teardown dump를 만들었다.
  이 test는 이전 reply epoch를 exact disconnect까지 유지하기 위해 prior reconnect를 의도적으로
  유지하는 별도 경로다. 한 원인 작업 범위를 넘기지 않기 위해 assertion이나 runtime reply-epoch
  동작을 바꾸지 않았다.
- 문서화된 36분 sibling
  `ActorCreateCompletion_AfterHandoverHello_UsesCapturedReplyRoute`는 지시대로 실행하지 않았다.
  해당 test가 사용하는 기본 `HandoverAsync()` 분기는 변경 전 send/receive/retained-prior 동작을
  유지하므로 이번 opt-in liveness fix가 그 실행 시간을 바꾼다는 증거는 없다.
