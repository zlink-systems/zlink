# Bucket F — .NET relocated reply relay의 transport-pair 경합

## 결론

`RelocatedActorReplyCompletesTheOriginalRemoteCallerExactlyOnce`의 3초 timeout은 relay 대상 RID,
payload나 ACK fence 값이 틀린 결과가 아니었다. Reciprocal handover에서 outbound survivor가 선택된
뒤에도 RID별 ingress pair 인덱스가 늦은 inbound loser `ConnectionReady`로 바뀔 수 있었다. Caller는
command 33 `ReplyRelay`를 실제로 받았지만 current-source 검사를 통과하지 못해 버렸고, command 46
`ReplyRelayAck`를 보내지 않았다.

Admitted peer를 공개할 때 RID별 ingress pair도 그 peer의 survivor pair로 확정하고, 이미 admitted인
outbound survivor가 있으면 반대 방향의 늦은 ready edge가 그 인덱스를 덮지 않게 수정했다. Relay나
ACK의 인증 검사는 완화하지 않았다.

## 재현성과 determinism

| 실행 | 결과 | 판정 |
|---|---:|---|
| Supervisor 전체 unit gate | 0/1, 3초 timeout | 부하가 있는 실행에서 관측된 최초 실패 |
| 현재 source, 수정 전 단독 1회차 | 1/1 통과 | 단독 재현 안 됨 |
| 현재 source, 수정 전 단독 2회차 | 1/1 통과 | 단독 재현 안 됨 |
| 현재 source, 수정 전 단독 3회차 | 1/1 통과 | 단독 재현 안 됨 |
| 현재 source, 수정 전 단독 4회차 | 1/1 통과 | 단독 재현 안 됨 |
| 현재 source, 수정 전 단독 5회차 | 1/1 통과 | 단독 재현 안 됨 |

단독 5/5가 통과했으므로 deterministic regression이 아니라 monitor/admission 처리 순서에 따른
load-sensitive race로 분류했다. Supervisor 실패는
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/gate-v3-dotnet-unit.log:117-126`
에 있다.

## Trace와 원인

Message-flow는 application dispatch만 기록하므로 RouteMesh command 33/46은 기존
spot-discovery/task file trace로 확인했다.

- 실패 trace `/dev/shm/zlink-tmp-dotnet/relay-task-trace.K2Ct60:57`:
  caller가 `ReplyRelay`, `parts=2`, `allowed=True`까지 수신했지만 `current_source=False`였다. 이후
  `canonical_reply_relay_completion`과 `ReplyRelayAck`가 없다.
- 통과 trace `/dev/shm/zlink-tmp-dotnet/relay-stress-trace.UMlWwP:59-65`:
  같은 command 33이 `current_source=True`이고, `TerminalReceived` 뒤 command 46 ACK가 source에
  도착한다. 같은 파일 `:66-68`에서는 중복 relay가 `AlreadyTerminal`로 ACK되어 두 번째 completion을
  만들지 않는다.

Code path는 다음과 같다.

1. `ZLinkManagedMeshNode.cs:5589-5592`가 수신 RID로 `_transportPairsByRid`를 읽는다.
2. `ZLinkManagedMeshNode.cs:5178-5182`가 non-admission control의 pair를 admitted
   `peer.TransportPair`와 비교한다.
3. Spot-route ready-pair attribution 뒤 `ConnectionReady`는 outbound intent의 pair를 보존했지만,
   reciprocal inbound 후보의 늦은 ready edge가 RID별 인덱스를 다른 pair로 다시 쓸 수 있었다.
4. 따라서 relay 송신 자체는 성공해도 caller ingress에서 pair가 다르다고 판정하여 command 33을
   protocol error로 폐기했고, `RelayRelocationReplyAsync`의 pending ACK만 timeout까지 남았다.

원인은 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`의
ready-event pair attribution과 admission 완료 시점 사이에 RID별 ingress fence를 survivor에 다시
고정하지 않은 것이다.

## 수정과 회귀

- `ZLinkManagedMeshNode.cs:8599-8603`
  - Admission 또는 reciprocal settlement가 완료될 때 `_peersByRid`와 `_transportPairsByRid`를 같은
    survivor peer/pair로 함께 확정한다.
- `ZLinkManagedMeshNode.cs:8804-8835`
  - Ready event가 outbound intent에 귀속되면 해당 pair를 기록한다.
  - Admitted inbound peer의 replacement는 계속 새 pair를 받는다.
  - Admitted outbound survivor와 일치하지 않는 늦은 inbound ready edge는 readiness 증거로는
    보존하지만 RID별 current ingress pair를 덮지 않는다.

대안으로 command 33만 current-source 검사를 우회하거나 ACK 검사의 lifecycle/pair 조건을 완화할 수
있지만, retired pair의 control을 수락하게 되어 transport epoch fence를 깨므로 선택하지 않았다.
Relay를 logical RID로 재전송하는 방식도 이미 도착한 frame이 ingress에서 거부되는 원인을 해결하지
않는다.

기존 회귀 `RelocatedActorReplyCompletesTheOriginalRemoteCallerExactlyOnce`가 첫 relay의
`TerminalReceived`, caller completion 1개, 중복 relay의 `AlreadyTerminal`, 추가 completion 없음까지
검증한다. Assertion과 timeout은 변경하지 않았다. User Spot과 Instance Spot의 같은 relay 계약도
함께 반복했다.

## 검증 결과

모든 .NET 실행은 지정된 `TMPDIR`, local Core `ZLINK_LIBRARY_PATH`, package hash 기반
`NUGET_PACKAGES`, shared compilation/node reuse 비활성화와 gate lock을 사용했다.

- 집중 회귀 14개: **14/14 통과**
  - bilateral immediate request 1개
  - `RemoteActorStaleAuthority*` 3개
  - Actor/User Spot/Instance Spot relocated reply 3개
  - `ZLinkMeshPeerAdmissionTests` 7개
- Actor/User Spot/Instance Spot relocated reply 묶음 5회: **각 3/3, 합계 15/15 통과**
- `RelocationReplyRelayUsesRawCommandsAndRetriesAfterAckLoss` 5회: **5/5 통과**
- `--filter 'FullyQualifiedName~StatefulServiceRuntimeTests'`: **50/50 통과**
- `git diff --check`: **통과**

기존 범위 밖 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning은 그대로다. Core, bindings, 다른 언어,
보호된 Framework 문서와 test assertion은 수정하지 않았다.

## BLOCKERS

없음. 전체 unit suite는 supervisor가 실행하므로 이 작업에서는 다시 실행하지 않았다.
