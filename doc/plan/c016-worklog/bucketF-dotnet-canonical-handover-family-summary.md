# Bucket F — .NET canonical actorJoin handover family

## 결론

두 실패는 같은 reciprocal admission 결함이 아니었다.

- `ManagedSource_Command28Request_ConsumesCommand20TailAndApplicationReply(true)`는
  setup admission timeout을 한 번 기록했지만 다시 재현되지 않았다. `targetDialsSource=true`
  trace에서는 target의 outbound pair와 source의 inbound pair가 각각 ready 상태가 된 뒤
  `Hello`/`Admit`과 command 28 completion이 끝났다. 같은 theory를 20회 실행한 40개 case와
  최종 family 반복에서도 모두 통과했다.
- `CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`는 test가
  `HandoverAsync()` 뒤에도 이전 pipe가 아직 연결되어 있다고 가정했다. 실제 Core handover는
  replacement `Hello`를 처리하기 전에 기존 pair의 정확한 `Disconnected` event를 보냈다.
  Framework는 해당 pair의 reply epoch를 폐기했고 pending reply를 `Terminated`로 끝냈다.
  따라서 handover 뒤에 재시도 횟수 증가를 기다리던 assertion이 test 순서와 충돌했다.

Framework의 pair 폐기 규칙은 바꾸지 않았다. Test가 이전 pipe가 유지되는 구간과 정확한
disconnect 이후 구간을 분리하도록 수정했다.

## 실패 assertion과 teardown

Supervisor log
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/gate-v4-canonical.log`
에는 다음 결과가 남았다.

- `ManagedSource_...(true)`는 기존 test `CanonicalActorJoinIngressReplyTests.cs:49`의
  setup wait에서 `Canonical actorJoin setup did not complete`로 실패했다. false case와 앞선
  case 9개는 통과했다.
- 다음 case는 xUnit failure를 출력하지 못한 채 3분 inactivity timeout에 도달했다. dump
  `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/TestResults/257faa7c-e536-4cf8-bd2f-34bde6e29130/dotnet_96534_20260905T042258_hangdump.dmp`
  의 stack은 `ConnectedRuntime.DisposeAsync` → `Context.DisposeAsync` →
  `Context.Dispose` → native `zlink_ctx_term`이다. Test 본문의 무한 대기가 아니라 실패 뒤
  teardown 정지다.

1분 blame timeout으로 handover case만 실행했을 때 test 본문의 첫 실패는 기존
`CanonicalActorJoinIngressReplyTests.cs:560`의 `attempts > 1` wait였다. 같은 실행에서
`ManagedSource`의 true/false case는 모두 통과했다.

## 보존한 trace

조사 중 기존 SpotDiscovery와 monitor event를 파일에 연결했고, 임시 file sink와 상세 로그는
조사 뒤 제거했다.

- `/tmp/bucketF-canonical-trace.GghPm1:1-16`: `targetDialsSource=true`에서 source inbound
  pair 18과 target outbound pair 17이 ready 상태가 됐다. Source는 target `Hello`를 받고
  `Admit`을 보냈으며 target은 같은 outbound pair 17에 `Admit`을 귀속했다. 마지막에 command
  28 operation completion이 기록됐다.
- `/tmp/bucketF-canonical-trace.GghPm1:48-62`: raw fixed-RID handover에서 기존 pair 63이
  ready 상태가 된 뒤 pair 69와 pair 63의 정확한 disconnect가 replacement `Hello`보다 먼저
  도착했다. `RetireTransportPair`가 pair 63을 폐기했고 pending reply는
  `valid=False submit=Terminated`로 끝났다.
- `/tmp/bucketF-canonical-trace.GghPm1:63-76`: replacement는 그 뒤 `Hello`/`Admit`을
  완료했으나 이전 DEALER의 reconnect intent가 남아 있어 같은 RID가 다시 pair를 점유하고
  disconnect하는 과정이 반복됐다.
- `/tmp/bucketF-managed-stress-trace.KyiwwH`: theory 20회에서 40개의 `Admit`과 40개의
  command 28 completion을 기록했다. setup timeout은 재현되지 않았다.

`ConnectionReady flags=None`은 disconnect 뒤 ready 수를 알리는 snapshot이며 새로운 ready
edge가 아니다. Framework는 `ZLinkManagedMeshNode.cs:8794-8801`에서
`ConnectionReadyEdge`만 새 pair로 처리한다. 정확한 disconnect는
`ZLinkManagedMeshNode.cs:8840-8844`에서 받고, `:8998-9027`에서 해당 transport pair와 reply
epoch를 폐기한다. Receive loop는 `:5017-5026`에서 monitor disconnect를 application 작업보다
먼저 적용한다.

## 계약과 원인

Fixed RID는 이전 pipe 종료가 확인된 뒤 replacement를 target selection에 포함해야 하고,
중복 후보가 생기면 ready connection 하나만 유지해야 한다
(`framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:303-313`).
Reply는 별도 handover 규칙을 갖지 않고 logical RID의 현재 ready pipe로 이동한다
(`doc/plan/c016-worklog/decisions.ko.md:522-531`, D-072).

기존 test는 `HandoverAsync()`를 먼저 실행한 뒤 pending reply 재시도를 관측했다. 그러나
Core가 이 호출 안에서 기존 pair를 정확히 끊으므로, Framework가 epoch를 계속 유효하게 두면
오히려 끊어진 물리 pair로 reply를 제출하게 된다. 원인은 runtime의
`RetireTransportPair`가 아니라 test의 관측 순서였다.

Family 반복에서 별도 fixture race도 확인했다. 기존 `SendIdempotentHelloAsync`는 받은 frame의
종류를 확인하지 않아 liveness frame이나 이전 `Admit`을 현재 admission 완료로 오인할 수
있었다. 그 상태에서 `AdmittedPeerCount`를 읽으면 0일 수 있었다.

## 수정과 회귀

변경 파일은
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs`
하나다.

- `:558-574`: handover 전에 pending reply가 두 번 이상 제출되고 protocol error 없이 기존
  peer가 admitted 상태임을 확인한다. 그 뒤 `HandoverAsync(disconnectPrior: true)`로 이전
  DEALER를 명시적으로 종료한다. 정확한 disconnect가 protocol error를 만들고 이전 epoch의
  재시도가 멈추며 replacement peer 하나가 admitted 상태로 유지되는지 확인한다.
- `:576-604`: 새 request와 application reply가 replacement route에서 계속 완료되는 기존
  assertion을 유지한다.
- `:620-636`: stale prior `Hello`와 exact disconnect가 현재 peer를 바꾸지 않는 검증은
  6회 반복 RouteAdmission 회귀가 담당한다.
- `:1130-1136`: `SendIdempotentHelloAsync`는 기존
  `SendHelloUntilAdmittedAsync`를 사용해 실제 RouteAdmission `Admit`을 디코딩하고 target의
  admitted 상태까지 기다린다.

Assertion 값과 timeout은 낮추지 않았다. Core, binding, 다른 언어, 보호된 spec 문서는
수정하지 않았다.

## 검증 결과

모든 실행은 지정된 local Core `ZLINK_LIBRARY_PATH`, package hash 기반 `NUGET_PACKAGES`,
`TMPDIR`, shared compilation/node reuse 비활성화와 `/tmp/zlink-dotnet-gate.lock`을 사용했다.

- `ManagedSource_Command28Request_ConsumesCommand20TailAndApplicationReply` theory 20회:
  **40/40 통과**.
- `CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect` focused:
  **1/1 통과**.
- 요청된 canonical family filter, 최종 코드의 성공 반복
  (`bucketF-canonical-final-4.trx`, `-6.trx`, `-7.trx`): 각 **14/14**, 합계 **42/42 통과**.
- `StatefulServiceRuntimeTests` (`bucketF-stateful-final-v2.trx`): **50/50 통과**.
- `git diff --check`: **통과**.
- 조사용 `ZLINK_DEBUG_FRAMEWORK_LOG`, `native_reply_retry`, `mesh_peer_monitor`,
  `mesh_peer_pair_attach`, `mesh_peer_pair_retire` 코드는 최종 source에 남아 있지 않다.

Build에는 기존 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning 하나가 남았다.

## BLOCKERS

- Canonical family의 추가 반복 `bucketF-canonical-final-5.trx`는 assertion failure 없이 7개
  case를 통과한 뒤 `RouteAdmission_HandoverStartsFreshLivenessDeadline` cleanup에서 3분
  inactivity timeout으로 중단됐다. Dump
  `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/TestResults/96755edf-d582-4416-b0bc-42f1133ce3f9/dotnet_79917_20260905T045529_hangdump.dmp`
  도 `ConnectedRuntime.DisposeAsync` → `Context.Dispose` → native `zlink_ctx_term` 정지를
  가리킨다. 사용자 facts에서 별도로 보고된 Core teardown 후보와 같은 blocker이며 이번
  허용 범위에서는 수정할 수 없다.
- Supervisor의 단발성 `ManagedSource_...(true)` setup timeout에는 재현 가능한 실패가 남지
  않았다. Stress 40/40과 최종 family 42/42가 통과했으므로 근거 없이 runtime 경로를 바꾸지
  않았다.
- 실행 시간이 긴
  `ActorCreateCompletion_AfterHandoverHello_UsesCapturedReplyRoute` sibling은 지시대로 family
  filter에서 제외했다.
