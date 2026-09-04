# Bucket B — .NET TicTacToe `JoinGameNotify` 전달 복구

## 결론

`JoinGameNotify`의 STREAM codec, session binding 순서나 `DONTWAIT`/WRITABLE 처리는 원인이
아니었다. `play-b`가 먼저 만든 정상 peer 연결이 이미 `play-a`에서 inbound로 admitted된 뒤,
location auto-connect가 같은 RID·endpoint를 outbound 방향으로 다시 연결했다. .NET
`ZLinkManagedMeshNode.ConnectPeer`가 live peer 재사용 대상을 outbound로만 제한해 reciprocal
physical candidate를 만들었고, 그 handover 뒤 `play-b`의 Framework route와 Core survivor가
달라졌다. 따라서 remote session push의 source-local submit은 성공했지만 `play-a` ingress에는
도달하지 않았다.

같은 RID·endpoint·security identity로 이미 admitted된 peer는 방향과 관계없이 기존 intent를
재사용하도록 수정했다. 새 timeout, retry, codec 또는 sample 우회는 추가하지 않았다.

## 실패 trace와 중단 지점

첫 재현은 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`로 보존했다.

1. `play-a`는 `play-b`가 시작한 연결을 이미 admit했다
   (`/dev/shm/zlink-tmp-dotnet/tmp.1Y0XtoAVnQ/logs/play-a.log:10-13`).
2. 그 뒤 auto-connect가 같은 `play-b` RID·endpoint를 dial 대상으로 정하고 새 intent 2를 열었다
   (같은 로그 `:17-21`). 수정 전
   `ZLinkManagedMeshNode.ConnectPeer`의 live-peer 검색은 `Direction == Outbound`만 허용했다
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:389-404`,
   수정 전 조건은 `:394`). 따라서 기존 admitted inbound intent를 재사용하지 못했다.
3. game Spot actor는 `JoinSpot`을 처리하고 reply한 뒤 current binding에 push했다
   (`tmp.1Y0XtoAVnQ/logs/play-b.log:130-139`). 코드 경로는
   `ZLinkActorBoundSessionCoordinator.SendCurrentAsync`
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorBoundSessionCoordinator.cs:966-1043`)
   → `ZLinkFrameworkRuntime.RelayRemoteSessionPushAsync`
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:4882-4904`)
   → `ZLinkManagedMeshNode.SendToNodeDirectAsync`
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9255-9273`)이다.
4. source 로그에는 `session_push_relay_async ... submit=Submitted`와
   `phase=sent ... packet=JoinGameNotify`가 남았지만(`play-b.log:137-139`), `play-a`에는
   `$zlink.session.push-relay.v1` ingress 및 `session_push_one_way_admission`이 없었다. client는
   heartbeat를 계속 받은 뒤 `JoinGameNotify` wait에서 timeout했다
   (`tmp.1Y0XtoAVnQ/logs/client.log:18-32`). 즉 중단 경계는 `play-b`의 RouteMesh local transport
   admission 이후, `play-a`의 remote relay handler 이전이었다.

`Submitted`를 remote delivery로 읽으면 안 된다. one-way remote target의 완료 경계는 local
transport queue이며 remote handler나 subscriber 수신을 기다리지 않는다
(`framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:100-112`).
그러므로 server의 `phase=sent`는 client 수신 증거가 아니었다.

## 계약과 수정

- session/actor binding 정상 흐름은 actor handler가 current bound session으로 one-way push를
  보내는 것이다
  (`framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:34-44`).
  Push는 bind 때 저장한 route를 사용하고 message마다 Location Store에서 다른 owner를 추측하지
  않는다(같은 문서 `:259-264`). Command 36도 Actor owner → Session owner send로 정의된다
  (같은 문서 `:283-294`). 따라서 push 경로에서 재조회·재전송하는 수정은 선택하지 않았다.
- topology는 manual 한쪽/양쪽 연결을 허용하고, automatic/manual 경합과 duplicate pipe 뒤에는
  ready 연결 하나만 남겨야 한다
  (`framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md:457-469`,
  `:688-700`). 이미 같은 logical peer에 admitted된 physical lifetime이 있을 때 동일 auto-connect
  요청으로 두 번째 candidate를 만든 것이 이 전제를 깨뜨렸다.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:389-404`:
  admitted peer 검색에서 outbound 방향 제한을 제거하고, expected RID 또는 admitted routing RID,
  endpoint, security identity가 모두 같은 기존 peer의 intent를 반환한다.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:954-979`:
  높은 RID node가 먼저 만든 연결이 낮은 RID node에서 admitted inbound가 된 뒤, 낮은 RID의 같은
  `ConnectPeer`가 기존 intent를 반환하며 peer/admitted count를 늘리지 않는 회귀를 추가했다.

## 수정 후 전달 trace

성공 evidence는 `/dev/shm/zlink-tictactoe-fix-evidence-4/TicTacToe/`에 보존했다.

1. `play-a` auto-connect는 여전히 `play-b:dial`을 계산하고 claim하지만
   (`logs/play-a.log:17-18`), 수정 전과 달리 새 `mesh_peer_connect ... intent=2`가 없다.
2. `play-b` actor push는 `bound_session_send_async` →
   `session_push_relay_async ... submit=Submitted`로 제출된다(`logs/play-b.log:125-127`).
3. `ZLinkRemoteSessionPushRelayHandler.HandleAsync`
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkRemoteSessionPushRelay.cs:150-163`)
   가 session owner에서 `AdmitRemoteSessionPushOneWayAsync`로 넘기고
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:4837-4851`),
   binding fence 검증 뒤 session context에 쓴다
   (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorBoundSessionCoordinator.cs:93-173`,
   `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkSessionActorBindingTable.cs:253-287`).
   `play-a`는 `session_push_one_way_admission ... result=Delivered`를 기록했다
   (`logs/play-a.log:60`).
4. client는 host `JoinGameNotify`를 실제 수신했다(`logs/client.log:6`; guest `:11`, reconnect host
   `:28`).

## 검증 결과

모든 `dotnet test`와 sample 실행은 지정된 환경과
`flock -w7200 /tmp/zlink-dotnet-gate.lock` 안에서 수행했다.

- 집중 unit filter:
  `ConnectPeerReusesAdmittedInboundPeerForSameIdentityAndEndpoint` +
  `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`: **2/2 통과**.
- TicTacToe 최종 실행 3회: **3/3**, 모두 `tictactoe-placement=completed`, exit **0**.
  Evidence: `/dev/shm/zlink-tictactoe-fix-evidence-{4,5,6}/TicTacToe/`.
- SupportChat 실행 1회: **1/1**, `supportchat-placement=completed`, exit **0**.
  Evidence: `/dev/shm/zlink-supportchat-fix-evidence/SupportChat/`.
- build는 위 unit/sample runner에 포함되어 모두 성공했다. 첫 debug 실행에서 기존
  `ZLinkSpotNodeCatalog.cs:768` CS8619 warning 1건이 보였고 이번 변경과 무관해 수정하지 않았다.

## BLOCKERS

없음. Core, bindings, 보호된 spec/framework 문서, 다른 언어, shared sample은 수정하지 않았고
commit도 만들지 않았다.
