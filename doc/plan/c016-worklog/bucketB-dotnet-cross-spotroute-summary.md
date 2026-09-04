# Bucket B — .NET client → C++ spot-route admission 후보 선택

## 결론

`Unavailable(origin=unspecified)`의 원인은 Core request submit이나 D-B85
backpressure가 아니라 .NET Framework의 RouteMesh admission 후보 선택이었다. C++ host는
listen-only라 .NET이 만든 outbound 물리 연결 하나만 존재하지만, C++가 그 연결에서 보낸
`Hello`를 .NET이 새 inbound peer로 만들고 기존 outbound intent를 reciprocal duplicate로
폐기했다. 이어 endpoint disconnect가 유일한 연결을 닫고 peer index를 제거했으므로
`ZLinkRouteClient.cs:403`의 submit 전 route 확인이 local `Unavailable`을 냈다.

분류는 **.NET Framework mesh admission/physical-candidate parity 결함(B)** 이다. Core,
binding REQUEST DONTWAIT, codec, capability, security identity와 C++ host 구현 결함이 아니다.

## 원인과 trace

실패 재현의 기존 기능 기반 monitor/admission trace는
`/dev/shm/zlink-tmp-dotnet/tmp.d5aB0VSU42/dotnet.log`에 보존했다.

- `:2-3`: configured outbound intent 뒤 C++의 정상 `Hello`가 같은 RID/endpoint로 들어왔다.
- `:4-5`: .NET은 그 `Hello`를 별도 inbound 후보로 취급하고 configured outbound를
  `mesh_peer_duplicate_retire`한 뒤 endpoint를 disconnect했다.
- `:8-14`: 그 연결의 Application/Completion lane이 disconnect되고 새 inbound peer까지
  `mesh_peer_remove`됐다.
- 같은 run의 `dotnet.events:1`은
  `spot-route-error|kind=unavailable|origin=unspecified`이고 C++ server event는 0건이었다.
  `dotnet.events.flow:1-80`도 request retry가 모두 reply terminal로 끝난 사실을 보여 준다.

최종 local exception 지점은
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeChannels.cs:257-267`의
manual target `RequiredNotConnected` 분기다. 호출은
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:403`에서 이
검사를 먼저 수행하므로 binding submit 전에 끝났다. 따라서 BACKPRESSURED/EAGAIN, wait token,
미정착 awaitable은 없으며 D-B85 dependency가 아니다.

C++의 대응 구현은
`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:445-476`이다.
`Hello`는 inbound, `Admit/Update`는 outbound를 선호하되 unilateral connection에는 존재하는 유일한
반대 방향 후보로 fallback한다. .NET matcher에는 이 마지막 규칙이 없었다.

## 수정과 계약

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshPeerAdmission.cs:18-34`
  - monitor의 RID와 endpoint가 모두 configured intent와 일치할 때만 ready outbound 후보로
    판별한다. accepted inbound의 provisional `hex:` RID를 outbound로 오인하지 않는다.
- 같은 파일 `:36-106`
  - `Hello`에 실제 ready inbound 후보가 있으면 기존 reciprocal duplicate 경로를 유지한다.
  - ready inbound가 없으면 configured outbound 후보로 fallback해 unilateral C++ 연결을 그대로
    admission한다.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8297-8311,
  8454-8501,8784-8820`
  - monitor에서 확인한 outbound 물리 pair를 해당 intent에 즉시 귀속하고, logical RID 단일
    인덱스가 나중의 inbound 후보로 바뀌어도 outbound survivor pair를 덮어쓰지 않는다.
  - reciprocal loser가 실제로 존재할 때는 이전 job의 handover settlement를 유지한다.
- 같은 파일 `:11656-11681,11763-11773`
  - provisional inbound candidate도 `ExpectedRid`로 동일 native handover route임을 확인하며,
    loser lane 정리 전 survivor를 ready로 공개하지 않는다.

적용한 계약은 다음과 같다.

- manual topology는 한쪽 또는 양쪽 endpoint 등록을 허용하고, 양방향 duplicate이면 ready 연결
  하나만 유지한다
  (`framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md:467-469`).
- handshake는 MeshName/RID, lifecycle, descriptor, role, channel, endpoint/security와 protocol을
  검증한다(같은 파일 `:493-503`). 이번 trace에서 이 필드는 모두 일치했다.
- fixed RID handover는 authenticated handover와 이전 pipe 종료를 확인한 뒤 새 generation을
  선택하고, duplicate 후보에서도 ready connection 하나만 유지한다
  (`framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:303-313`).
- Node direct는 지정 RID의 ready peer만 target으로 사용하고 다른 RID로 전환하지 않는다
  (`framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:81-100`).

## 회귀 test

- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ZLinkMeshPeerAdmissionTests.cs:99-152`
  - unilateral `Hello`가 configured outbound intent를 재사용한다.
  - ready inbound가 있으면 outbound fallback하지 않는다.
  - outbound ready 판별은 RID와 endpoint를 모두 요구한다.
- 기존 uncommitted handover fix와 그 회귀
  `StatefulServiceRuntimeTests.BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`
  (`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:880-952`)
  를 보존했다.
- 추가로 relocation reply relay 두 test를 함께 반복해, `Admit`이 peer `Hello`보다 먼저 오는
  bilateral 순서에서도 loser disconnect가 survivor를 제거하지 않는지 확인했다.

수정 후 진단 run `/dev/shm/zlink-tmp-dotnet/tmp.DCeU2mL4NI`에서는 monitor가 C++ RID/endpoint를
outbound로 판별하고 그 intent에 `Hello`를 admission했다(`dotnet.log:1-7`). 결과는
`dotnet.events:1-3`의 reply/not_found/rejected와 `cpp.events:1`의 실제 server 수신이다.

## 명령과 결과

모든 .NET 명령은 요청된 `TMPDIR`, local Core `ZLINK_LIBRARY_PATH`,
`UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`, telemetry-off 환경을 사용했다.
Java peer를 포함한 cross-language runner에서는 지시대로 `ZLINK_LIBRARY_PATH`를 unset했다.

1. matcher 7개 + bilateral immediate request + user/instance relocation relay 2개:
   **10/10 통과**. 같은 filter를 debug switch 없이 10회 반복해 **100/100 통과**했고, 임시
   probe 제거 뒤 최종 실행도 **10/10 통과**했다. 기존 범위 밖
   `ZLinkSpotNodeCatalog.cs:768` CS8619 warning 1건은 그대로다.
2. `ZLINK_CPP_CROSS_LANGUAGE_STAGE=spot-route
   framework/languages/cpp/cross-language/run_cross_language_smoke.sh` 3회:
   각 **7/7 방향 통과**.
   - `/dev/shm/zlink-tmp-dotnet/tmp.6bbYBeRLTr`
   - `/dev/shm/zlink-tmp-dotnet/tmp.wkaAIv2LQU`
   - `/dev/shm/zlink-tmp-dotnet/tmp.nF5RkAnLcV`
3. `ZLINK_CPP_CROSS_LANGUAGE_STAGE=all` 1회: **32/32 결과 통과**.
   `/dev/shm/zlink-tmp-dotnet/tmp.9OxO1e4u3p`
4. `git diff --check`: **통과**.

최종 변경은 .NET Framework admission/runtime와 unit test, 이 worklog뿐이다. 이전 job의
`ZLinkManagedMeshNode.cs`, `ZLinkMeshPeer.cs`, `StatefulServiceRuntimeTests.cs` uncommitted handover
수정은 유지했다. Core, bindings, 보호된 spec/framework 문서는 수정하지 않았고 임시 flow/file
logging과 monitor probe는 모두 제거했다. commit은 만들지 않았다.

## BLOCKERS

없음. 요청된 spot-route 3회와 all-stage 1회가 모두 통과했으며 D-B85 port를 기다릴 필요가 없다.
