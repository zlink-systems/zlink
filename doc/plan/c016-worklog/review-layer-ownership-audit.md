# Layer ownership audit — .NET / Java runtime

> 범위: 현재 `main`의 `f7fb207fd36f`, `4bad5ac979..HEAD`, Framework runtime만 판정한다. C++/Node는 같은 문제의
> 비교 기준으로만 읽었다. 이 문서는 정적 audit 결과이며 gate를 실행하지 않았다.

## 1. 결론

사용자 관찰은 **확인됐다**. 가장 큰 반복 결함은 다음 세 덩어리다.

1. .NET RouteMesh가 Core HANDOVER의 physical survivor와 reply route를 다시 소유한다.
   `_ready*`, `_transportPairsByRid`, reciprocal two-lane settlement,
   `_nativeReplyEpochsByTransportPair`가 같은 사실을 별도 상태로 유지한다.
2. .NET/Java ClientServer가 Core의 persistent connect intent와 자동 reconnect를
   `disconnect → close 관찰 → connect → admission retry`로 다시 구현한다. .NET은 그 과정에서
   같은 socket에 두 번째 poller까지 만들어 Core command 진행을 application polling으로
   구동한다.
3. Java의 backend default async 표면과 일부 Actor/Session 경로가 binding async terminal 대신
   sync `DONT_WAIT` 결과를 future로 포장하거나 전체 request를 재제출한다. 또한 raw errno를
   두 곳에서 다시 분류한다.

반대로 service handshake, descriptor/generation 검증, logical ready set, liveness deadline,
명시적 descriptor replacement fence는 Framework 소유가 맞다. 이 상태를 없애는 것이 아니라
physical route와 completion의 판단을 Core/binding 결과에 종속시켜야 한다.

우선순위는 다음과 같다.

- **P0 — 하위 계약 불일치 확인/수정:** .NET 코드가 명시한 reciprocal loser standby,
  disconnect command의 application-poll 의존, Java가 stable pair identity를 합성해야 했던 이유,
  Java async `NOT_ADMITTED`의 coarse 분류를 Core/binding에서 재현하고 고친다.
- **P1 — 상위 재구현 제거:** .NET ClientServer second poller/manual reconnect, .NET Mesh physical
  pair re-pin/reply epoch, Java unchanged-endpoint reconnect와 fabricated connection ID를 제거한다.
- **P2 — terminal 단일화:** Java/.NET backend의 sync-to-async default와 broad request retry를
  제거하고 binding async terminal/typed result만 소비한다.
- **P3 — Java STREAM owner 정리:** lane 안 blocking close, raw socket escape, infrastructure의
  sync lane bridge를 제거한다.

## 2. 판정 기준과 계약 경계

| 사실/상태 | 단일 owner와 근거 |
|---|---|
| 같은 RID의 physical pipe 선택 | Core socket spec `core/doc/spec/core/socket/README.ko.md` §4 “rid 중복 정책”, 특히 155-165. HANDOVER가 same-direction replacement와 reciprocal RID 비교를 소유한다. |
| connect intent와 물리 재연결 | Core socket spec `zlink_connect`, 817-829. 한 번 등록된 endpoint의 remote 장애 재연결은 library 소유다. Endpoint를 구성에서 제거하는 명시적 disconnect와 구분한다. |
| linger/close와 pending completion 정리 | Core socket spec `zlink_close`, 599-619. Accepted close 뒤 신규 진입, pending operation/completion/payload 정리는 Core 소유다. |
| `DONTWAIT` wait token과 target-specific writable | Core socket spec “Part send와 pending admission”, 895-950; Core polling spec `core/doc/spec/core/05-polling.ko.md` §3, 45-81. `POLLOUT`은 aggregate이고 정확한 재제출 근거는 WRITABLE token이다. |
| async terminal의 provisional 등록, WRITABLE drain, 동일 payload 재제출 | Binding `bindings/doc/spec/async-coroutine-policy.ko.md` §3-§5 및 `async-execution-model.ko.md` §4-§5. Framework가 retry queue/waiter를 만들지 않는다. |
| completion queue drain owner | Core polling spec §4 “Completion polling”; public poller가 `POLLCOMPLETION`을 등록하면 그 poller 하나가 owner이고 아니면 binding runtime 하나가 owner다. |
| Core/native 오류의 public 분류 | Binding spec `bindings/doc/spec/README.ko.md` “오류 정책” 3972-3977, “Request-Reply 오류 정책” 4234-4252. Core가 typed result로 정규화하고 binding은 typed exception으로 옮긴다. Raw errno는 진단 정보다. |
| service handshake, descriptor/generation/security와 logical ready target | Framework topology spec `01-channel-topology.ko.md` §8-§10, 특히 505-519, 570 이하, 검증 요구 688-700; MeshNode spec `03-spot-actor/03-mesh-node.ko.md`. |
| physical event를 logical ready/not-ready로 반영 | Framework liveness spec `05-transport-liveness.ko.md` §5-§6, 215-260 및 검증 요구 345-395. 명시적 replacement는 exact pair close 관찰 뒤 연다(239-246). |
| Framework의 binding terminal 소비 | Framework execution spec `01-submit-and-completion.ko.md` §5 177-179, §9 287-299, §15 434-459, §17 511-550. Internal HWM-managed send는 async terminal 하나이며 timeout/route failure 뒤 request를 자동 재제출하지 않는다. |
| state lane | Framework execution spec `06-state-ownership-and-lanes.ko.md` §3-§6, 특히 71-91, 254-263. Long operation을 lane turn에서 기다리지 않고 sync bridge는 Framework 실행 문맥 밖에서만 쓴다. |
| Session bind/reply 재전송 | Session spec `04-session/02-session-actor-binding.ko.md` 356-362와 §12 626-644. 일반 send 위에 별도 retry를 두지 않고 request submit 뒤 실패 시 다른 route/owner로 다시 보내지 않는다. |

`OWNED-CORRECTLY`는 control이 있다는 뜻이 아니라 해당 layer의 논리 사실만 소유한다는 뜻이다.
`DUPLICATED`는 Framework 내부에 같은 사실의 owner가 둘 이상인 경우다.
`RE-IMPLEMENTS-LOWER-LAYER`는 Core/binding이 이미 결정하는 physical/completion 사실을
Framework가 다시 결정하는 경우다.

## 3. Concern 1 — ROUTER RID duplicate / reciprocal handover / one-ready-peer

### .NET runtime inventory

| site | 구현하는 control | 판정 |
|---|---|---|
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshPeerAdmission.cs:36-107` `FindForAdmission` | HELLO/ADMIT/UPDATE를 configured logical intent에 붙인다. | **OWNED-CORRECTLY** — Framework handshake owner. |
| 같은 파일 `:109-172` `FindDuplicate` / `FindNotRequiredDuplicate` | logical peer가 하나만 ready가 되도록 duplicate 후보를 찾는다. | **OWNED-CORRECTLY**, 단 아래 physical retirement를 호출하는 순간 경계를 넘는다. |
| 같은 파일 `:18-34` `FindReadyOutboundCandidate` | monitor RID+endpoint로 outbound physical pair를 역추론한다. | **DUPLICATED** — Core monitor의 connection/pair identity를 단일 입력으로 써야 한다. |
| `ZLinkManagedMeshNode.cs:8297-8311` | `_readyInbound*`/`_readyOutboundEndpoints`로 admission의 `transportPair`를 다시 고른다. | **RE-IMPLEMENTS-LOWER-LAYER** — Core가 active RID route를 골랐는데 Framework가 pair를 re-pin한다. |
| 같은 파일 `:8449-8501` | `SelectConnection`으로 logical survivor를 선택한 뒤 reciprocal settlement와 physical retirement를 시작한다. | survivor의 logical admission은 **OWNED-CORRECTLY**; Core pipe survivor까지 적용하는 부분은 **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:8526-8569`, `:9062-9083` | losing two lanes가 모두 `Disconnected`일 때까지 survivor의 `Admitted` 게시를 보류한다. | **DUPLICATED** — logical publication과 Core HANDOVER terminal을 별도 state machine 둘이 소유한다. |
| 같은 파일 `:8788-8863` | `_readyOutboundEndpoints`, `_readyInboundRids`, `_transportPairsByRid`를 monitor edge로 갱신한다. | readiness 관찰은 맞지만 pair/RID index는 **DUPLICATED**. |
| 같은 파일 `:8865-8893`, `:11673-11699` | reciprocal loser의 두 transport lane disconnect mask를 직접 정산한다. | **RE-IMPLEMENTS-LOWER-LAYER** — Core §4 HANDOVER가 reciprocal collapse owner다. |
| 같은 파일 `:8895-8977`, `:8998-9055` | Framework pair epoch를 attach/tombstone하고 늦은 disconnect가 current peer를 내리지 못하게 한다. | **RE-IMPLEMENTS-LOWER-LAYER** — monitor pair identity를 관찰만 해야 한다. Reply 부분은 Concern 6과도 중복된다. |
| 같은 파일 `:11701-11788` `RetireDuplicatePeer` | loser endpoint reconnect intent를 취소하고 `DisconnectRid`가 survivor를 닫을지 자체 판정한다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:11822-11886` `RemovePeer` / `DisconnectTransport` | replacement endpoint 존재 여부에 따라 endpoint disconnect와 RID disconnect를 선택하고 binding 예외를 삼킨다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:11163-11177` `ConnectPeerCore` | 구성 intent 최초 등록 시 RID option과 endpoint를 Core에 전달한다. | **OWNED-CORRECTLY** — 다만 기존 intent 재활용/교체는 Core lifecycle 결과를 따라야 한다. |

**중요한 하위 불일치:** `ZLinkManagedMeshNode.cs:11774-11777`은 “Core HANDOVER가 losing
reciprocal pipe를 standby route로 남긴다”고 명시한다. 이는 Core §4의 “양쪽이 같은 방향 하나를
선택” 계약과 맞지 않는다. Framework가 standby를 숨기기 위해 endpoint intent를 취소하는 것은
상위 규칙이 아니라 **Core bug 보상**이다. 먼저 Core reciprocal collapse를 고치거나, spec이 실제
의도와 다르면 Core spec/API 변경으로 분리해야 한다.

삭제만 하면 현재
`CanonicalActorJoinIngressReplyTests.CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`,
`RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`,
`RouterMonitor_HandoverReportsCurrentConnectionIdentity`와 cross-language RouteMesh handover가
깨질 가능성이 높다. 이 테스트들은 logical one-ready 관찰은 유지하되 Framework의 pair mask/index가
아니라 Core monitor/HANDOVER 결과만으로 통과하도록 바뀌어야 한다.

### Java runtime inventory

| site | 구현하는 control | 판정 |
|---|---|---|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6411-6606` | descriptor/expected route 검사, direction 선택, `topology.admit`, one-ready peer 게시. | **OWNED-CORRECTLY** — logical topology owner. |
| 같은 파일 `:6764-6768` | edge flag 없는 `CONNECTION_READY` count snapshot을 admission edge로 쓰지 않는다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:6737-6820` | monitor event를 자체 UUID candidate queue에 매핑하고 selected logical connection을 내린다. | **DUPLICATED** — 실제 monitor connection identity가 single source여야 한다. |
| 같은 파일 `:7080-7097` `connectionIdForAdmission` | pending ID가 없으면 기존 logical ID를 재사용하거나 random UUID를 만든다. | **RE-IMPLEMENTS-LOWER-LAYER** — physical identity를 Framework가 발급한다. |
| 같은 파일 `:7136-7157`, `:7232-7270` | READY마다 UUID를 발급하고 `local|remote` 문자열 FIFO로 DISCONNECTED와 짝짓는다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:6527-6548`, `:6784-6793` | duplicate candidate가 보이면 “lost disconnect edge”를 가정해 이전 selected pair validation을 앞당긴다. | **DUPLICATED** — monitor/liveness truth와 admission heuristics가 같은 종료 사실을 소유한다. |
| 같은 파일 `:6826-6891` | intent별 `TransportIdentity` set을 관리하고 모든 edge 뒤 endpoint를 다시 disconnect한다. | **DUPLICATED** — Core intent/monitor lifecycle과 겹친다. |
| 같은 파일 `:915-951`, `:971-1005` | descriptor replacement가 이전 physical close를 관찰한 뒤 새 intent를 연다. | **OWNED-CORRECTLY** — liveness spec 239-246의 명시적 replacement fence. 단 `cleanupClosedPeerEndpoint`의 두 번째 disconnect는 중복이다. |
| 같은 파일 `:7331-7347` | NotRequired endpoint disconnect 실패를 모두 삼킨다. | logical NotRequired는 **OWNED-CORRECTLY**; broad swallow는 **DUPLICATED** terminal 정책. |

`TransportIdentity`는 이미 `connectionId`와 `transportLane`을 보관한다(`:7308-7328`)면서도
0이나 매칭 실패 때 endpoint key로 되돌아간다. binding monitor가 모든 READY/DISCONNECTED에 stable
pair identity를 보장하지 않는다면 **binding contract/API follow-up**이다. Framework UUID/FIFO가
정답을 추측해서는 안 된다. 관련 현행 테스트는
`ZLinkJavaRawMeshNodeM6ATest.monitorConnectionKeyIgnoresEventSpecificValue`,
`connectionIdForAdmissionReusesCoreSelectedRouteAcrossCommands`,
`replacementDoesNotSkipAConnectionBeforeItsReadyEvent`다. 앞의 두 테스트는 하위 identity 계약을
검증하는 integration test로 내려가야 한다.

### C++ / Node 비교

- C++ `raw_mesh_node_owner.cpp:426-476`, `:2748-2914`, `:3826-3843`은 Framework candidate와
  logical admission을 가지지만 monitor가 준 실제 `connection_id`를 key로 사용한다. random
  physical ID나 RID→pair re-pin은 없다.
- Node `channel-socket-registry.ts:1589-1661`도 monitor를 관찰해 topology state를 만든다. 이
  audit 범위에서는 .NET식 reciprocal two-lane settlement/reply-pair epoch는 발견하지 못했다.

**최종 판정:** logical one-ready admission은 **OWNED-CORRECTLY**. .NET physical settlement/index와
Java fabricated identity/FIFO는 **RE-IMPLEMENTS-LOWER-LAYER**이며 single owner는 Core HANDOVER와
stable monitor pair identity다.

## 4. Concern 2 — physical reconnect / endpoint re-registration

### .NET runtime inventory

| site | 구현하는 control | 판정 |
|---|---|---|
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:830-856` | 최초 connect 뒤 monitor edge와 별개 admission fallback을 예약한다. | 최초 connect는 **OWNED-CORRECTLY**; edge-less admission retry는 **DUPLICATED**. |
| 같은 파일 `:994-1040` | monitor event마다 Framework `physicalGeneration`과 disconnect TCS를 갱신한다. | logical fence는 **OWNED-CORRECTLY**; reconnect task의 private physical terminal로 쓰는 부분은 중복. |
| 같은 파일 `:1067-1113` | generation+attempt로 admission을 fence한다. | **OWNED-CORRECTLY** — handshake generation. |
| 같은 파일 `:1115-1188` | admission의 모든 exception을 physical restart 또는 100 ms retry로 바꾼다. | **RE-IMPLEMENTS-LOWER-LAYER** 및 오류 swallow. Binding terminal을 그대로 끝내야 한다. |
| 같은 파일 `:1191-1230` | physical generation별 admission retry timer를 소유한다. | **DUPLICATED** — 실제 새 ConnectionReady 때 새 handshake 한 번이면 된다. |
| 같은 파일 `:1349-1457` | control protocol/error에서 `RestartPhysicalConnection`을 호출한다. | protocol peer retirement는 Framework가 결정할 수 있지만 자동 re-register까지 하는 것은 **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:1461-1548` | logical liveness를 소유하고 timeout 때 physical restart; request exception은 모두 삼킨다. | deadline/ready demotion은 **OWNED-CORRECTLY**; physical restart와 broad catch는 중복. |
| 같은 파일 `:1558-1627` `FencePhysicalConnection` / `RestartPhysicalConnection` | `Disconnect(endpoint)` 후 private close edge를 기다리는 manual reconnect state machine. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:1629-1712` `ReconnectAsync` | 같은 socket의 두 번째 poller로 command를 진행시키고 `Connect(endpoint)`를 성공할 때까지 100 ms 재시도한다. | **RE-IMPLEMENTS-LOWER-LAYER**, Concern 4도 위반. |
| 같은 파일 `:109-143` `ReplaceAutomatic` | descriptor replacement successor를 만든 뒤 old owner를 retire한다. | **OWNED-CORRECTLY** — configuration/lifecycle replacement. |

`ReconnectAsync:1640-1655`의 주석은 disconnect command terminal edge가 application socket poll에
의존한다고 말한다. Core polling spec 72-81은 Core 내부 owner가 command를 처리해도 readiness와
wake를 보장한다. 실제로 별도 poll이 필요하면 **Core/binding progress bug**다. 두 번째 poller를
Framework 규칙으로 유지하면 안 된다.

삭제 시 직접 영향을 받는 테스트는
`ClientServerChannelRuntimeTests.MalformedPushedControl_ReconnectsAndReadmits`,
`ServiceRuntimeFoundationTests.ManagedNode_ReadmitsPeerAfterLivenessExpiry` 및 ClientServer
cross-language reconnect다. 바른 증명은 unchanged endpoint를 Framework가 재등록하지 않아도 Core가
재연결하고, 새 `ConnectionReady` 뒤 handshake가 딱 한 번 다시 수행되는지 확인하는 것이다.

### Java runtime inventory

| site | 구현하는 control | 판정 |
|---|---|---|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java:348-420` | transport event마다 private physical/admission generation을 회전하고 reconnect 진입점을 노출한다. | handshake fence는 **OWNED-CORRECTLY**; explicit reconnect entry는 중복. |
| 같은 파일 `:760-817`, `:980-1032` | liveness timeout, incompatible update, protocol error를 `reconnectClientServer`로 보낸다. | logical demotion은 **OWNED-CORRECTLY**; physical re-register는 **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:1077-1098` | 동일 dealer에서 `disconnect(endpoint); connect(endpoint)`하고 모든 exception을 삼킨다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:866-927`, `:931-961`, `:1138-1163` | ack/probe/update 전송 실패를 삼키거나 같은 probe ID를 timer에서 다시 보낸다. | probe ID 재전송은 liveness owner의 **OWNED-CORRECTLY** control; binding exception 무차별 swallow는 **DUPLICATED** terminal 정책. |
| `ZLinkChannelRuntime.java:828-879` | manual ClientServer dealer/monitor/initial connect를 만든다. | **OWNED-CORRECTLY** — 구성 intent 최초 등록. |
| 같은 파일 `:881-962` | admission submit/terminal의 모든 실패와 invalid reply를 physical reconnect로 바꾼다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| `ZLinkClientServerLocationRuntime.java:402-483` | desired descriptor를 reconcile하고 revision-only update는 socket을 보존한다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:485-558` | descriptor identity에 대한 새 dealer/monitor/intent를 연다. | 새/교체 descriptor면 **OWNED-CORRECTLY**. |
| 같은 파일 `:560-679` | admission failure마다 connection을 제거하여 다음 Store refresh가 새 dealer를 만든다. | unchanged physical failure면 **RE-IMPLEMENTS-LOWER-LAYER**; descriptor mismatch retirement는 Framework 소유. |

현행 `ZLinkClientServerM6ARuntimeTest.reconnectAdmissionFenceRejectsPreviousPhysicalPipeReply`는
logical stale-reply fence를 증명하므로 유지한다. 여기에 “unchanged endpoint transport loss는
`disconnect/connect/newDealer` 호출 0회, Core reconnect READY 뒤 admission 1회”를 추가해야 한다.

### C++ / Node 비교

- C++ `channel_outbound_exchange.cpp:669-718`, `:803-835`은 monitor ready count만 관찰하고
  desired endpoint set이 바뀔 때만 disconnect/connect한다. 동일 endpoint 장애는 Core intent에 맡긴다.
- Node는 완전한 모범이 아니다. `channel-socket-registry.ts:879-885`가 termination callback 요청 시
  동일 dealer를 disconnect/connect한다. 이 역시 현재 Core 계약 기준으로 후속 제거 대상이다.

**최종 판정:** descriptor replacement fence는 **OWNED-CORRECTLY**. 동일 endpoint의 장애 복구를
Framework가 재등록하는 .NET/Java 경로는 **RE-IMPLEMENTS-LOWER-LAYER**이며 single owner는 Core의
persistent connect intent다.

## 5. Concern 3 — `DONTWAIT`, BACKPRESSURED→WRITABLE, resubmit

### .NET runtime inventory

| site | 구현하는 control | 판정 |
|---|---|---|
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs:141-149` | native `.Async()`를 직접 반환한다. | **OWNED-CORRECTLY** — binding terminal이 retry/completion owner. |
| 같은 파일 `:125-165`, `:338-354` | sync surface가 lane을 block하고 `DONTWAIT`면 `TrySubmit`, `NONE`이면 `Submit`한다. | 공개 sync/immediate surface에 한정하면 **OWNED-CORRECTLY**. Infrastructure가 호출하면 위반. |
| `Runtime/Backend/Contracts/IZLinkBackendSocketContracts.cs:25-47` | default `SendAsync`가 sync `Send(...DontWait)`를 호출하고 false를 BACKPRESSURED exception으로 포장한다. | **RE-IMPLEMENTS-LOWER-LAYER** — wait token/WRITABLE을 잃는다. Default를 없애 모든 production/test backend가 true async terminal을 구현해야 한다. |
| `ZLinkManagedMeshNode.cs:11629-11664`, `:12926-12947` | routed/Session async send가 binding `.Async()`를 소비한다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:12896-12923` | sync Session send가 `TrySubmit`; 모든 `ZlinkException`을 `NotConnected`로 축약한다. | explicit immediate path만 허용. Broad mapping은 Concern 5의 **DUPLICATED** 분류다. |
| `Runtime/Streams/ZLinkSessionStreamTransport.cs:6-29`, `ZLinkStreamFrameWriter.cs:38-49` | infrastructure adapter가 `DontWait` sync bool surface를 사용한다. | **RE-IMPLEMENTS-LOWER-LAYER** — async terminal로 올려야 한다. |
| `ZLinkActorManagerService.cs:459-525` | remote create에서 `NotConnected`/`Backpressured`를 잡아 전체 submit을 loop한다. | **RE-IMPLEMENTS-LOWER-LAYER** — HWM retry는 binding terminal owner, request 재시도는 §9 위반. |

### Java runtime inventory

| site | 구현하는 control | 판정 |
|---|---|---|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:91-131`, `:144-191` | routed send/request의 binding `submit()` future를 직접 반환한다. | **OWNED-CORRECTLY**. |
| `ZLinkJavaStreamSocket.java:264-329` | state lane에서 frame을 만든 뒤 `FrameworkStreamOperations.send`의 binding-owned async terminal을 flatten한다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:204-225`, `:257-283` | explicit sync surface에서 `submit_sync`/sync framing을 사용한다. | 공개 sync/immediate caller에 한해 **OWNED-CORRECTLY**. |
| 같은 파일 `:864-883` `BoundSessionSink.sendAsync` default | sync `DONT_WAIT` 한 번을 future로 포장하고 BACKPRESSURED를 즉시 실패시킨다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| `runtime/internal/backend/ZLinkBackendStreamSocket.java:68-102`, `:125-137`, `:156-197` | send/reply/relay async default 네 계열이 sync `DONT_WAIT`을 future로 포장한다. | **RE-IMPLEMENTS-LOWER-LAYER**. 특히 reply 외 send/relay는 binding async terminal을 강제해야 한다. |
| `runtime/internal/backend/ZLinkInternalSpotNode.java:390-426` | remote bound-session/Actor async default가 sync `DONT_WAIT`을 future로 포장한다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| `ZLinkSessionActorsRuntime.java:1164-1222`, `:1334-1357` | sync와 async command-36 경로를 둘 다 유지한다. Current production async path는 binding terminal을 쓰지만 sync path는 즉시 BACKPRESSURED control을 별도로 가진다. | **DUPLICATED** — infrastructure receive owner는 async path 하나만 사용해야 한다. |
| 같은 파일 `:1360-1433` | Session-owned logical FIFO head를 binding async terminal 한 번에 위임한다. | **OWNED-CORRECTLY** — logical queue owner이며 physical retry는 하지 않는다. |

Java tests `ZLinkStreamPhysicalSubmitOwnershipTest.storedBindingRelayDelegatesTheAcceptedSequenceOnceToBackendAsync`,
`storedBindingRelayPropagatesOneBackendPhysicalErrorWithoutRetry`,
`ZLinkJavaStreamSocketAsyncTerminalTest.asyncSendReturnsBeforeTheSocketStateLaneCanStartAdmission`,
`asyncBoundSessionPushReturnsBeforeTheSocketStateLaneCanStartAdmission`이 삭제 후 기준이다.
Default fallback을 쓰는 fake backend는 테스트 편의를 위해 잘못된 계약을 유지하지 말고 명시적 async
terminal fake를 구현해야 한다.

### C++ / Node 비교

- C++ `raw_dealer_port.cpp:22-42`, `:44-157`은 poller 하나를 등록하고 send/request 모두 binding
  `.async()`를 호출한다. Framework retry queue가 없다.
- Node `node-socket-backend-adapter.ts:143-175`,
  `node-backend-adapter-support.ts:104-138`은 infrastructure send/submit을 binding `submit()`에
  맡기고, sync helper는 명시적 immediate path에만 둔다.

**최종 판정:** production async terminal 직접 소비는 **OWNED-CORRECTLY**. 두 언어의
sync-to-async default와 Framework submit loops는 **RE-IMPLEMENTS-LOWER-LAYER**이며 single owner는
binding async terminal이다.

## 6. Concern 4 — poller / `POLLCOMPLETION` ownership

### .NET poller inventory

| site | 등록 내용 / owner | 판정 |
|---|---|---|
| `ZLinkManagedMeshNode.cs:295-337` | mesh ROUTER 하나에 `IN|ERR|OUT|COMPLETION`; 같은 loop가 completion을 drain한다. | **OWNED-CORRECTLY** — public completion poller 하나. |
| `Runtime/Service/ZLinkRawRouterServicePort.cs:40-52` | service ROUTER에 `IN|ERR`만 등록한다. Binding runtime이 completion owner다. | **OWNED-CORRECTLY**. |
| `Runtime/Backend/DotNet/Wrappers/ZLinkBackendSocketPoller.cs:19-39` | 일반 receive wrapper는 `POLLIN`만 등록한다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:65-99` | SUB는 public poller 대신 `ZlinkPoll.Poll(POLLIN)`만 사용한다. | **OWNED-CORRECTLY** — SUB completion owner를 claim하지 않는다. |
| `Runtime/Channels/ZLinkChannelReceiveLoop.cs:19`, `:288`, `:389` | ClientServer ROUTER / 두 SUB loop에 receive poller 하나씩. | **OWNED-CORRECTLY**. |
| `ZLinkClientServerClientRuntime.cs:1349-1375` | DEALER control receive poller 하나. | 자체로는 **OWNED-CORRECTLY**. |
| 같은 파일 `:1640-1655` | 위 poller가 살아 있는 같은 DEALER에 reconnect poller를 하나 더 등록하고 command progress를 위해 wait한다. | **RE-IMPLEMENTS-LOWER-LAYER** — 즉시 제거 대상. |
| `ZLinkBackendStreamSocketWrapper.cs:41-42`, `ZLinkStreamNodeRuntime.cs:268-318` | STREAM receive poller 하나. | **OWNED-CORRECTLY**. |

### Java poller inventory

| site | 등록 내용 / owner | 판정 |
|---|---|---|
| `ZLinkJavaSocketReceivePoller.java:29-61` | 기본 `POLLIN|POLLOUT|POLLCOMPLETION`; `ownsCompletionQueue=false`면 `POLLIN`만 등록한다. | **OWNED-CORRECTLY**, socket당 인스턴스 하나라는 전제. |
| 같은 파일 `:64-96` | public poller wait가 binding completion drain을 함께 진행하고 Framework에는 readable만 반환한다. | **OWNED-CORRECTLY**. |
| `ZLinkJavaRawServicePort.java:61-88` | 각 owned ROUTER에 위 default poller 하나. | **OWNED-CORRECTLY**. |
| `ZLinkJavaRawMeshNode.java:745-749` | submit 전에 service ROUTER의 sole receive/completion poller를 등록한다. | **OWNED-CORRECTLY**. |
| `ZLinkJavaDealerSocket.java:22`, `ZLinkJavaRouterSocket.java:23`, `ZLinkJavaStreamSocket.java:96` | socket wrapper마다 default completion poller 하나. | **OWNED-CORRECTLY**. |
| `ZLinkJavaSubscriberSocket.java:18` | SUB에 `ownsCompletionQueue=false`, `POLLIN`만. | **OWNED-CORRECTLY**. |

Java에서는 현재 같은 socket에 두 public poller를 만드는 production site를 찾지 못했다. .NET의
second reconnect poller만 명백한 위반이다. 증명은 Core
`test_polling_contract.cpp::test_poller_socket_registration_error_contracts`, Java binding
`CompletionOwnershipContractTest`,
`SocketPollingContractTest.completionPollerOwnsDealerRequestProgress`, .NET binding
`test_pull_completion_contract.public_poller_owns_completion_and_transfers_it_back`를 하위 gate로 두고,
Framework에는 “ClientServer socket당 poller factory call 1회” test를 추가한다.

### C++ / Node 비교

- C++ `raw_dealer_port.cpp:22-42`은 owned/shared poller 중 하나만 선택해
  `IN|OUT|COMPLETION`을 등록한다.
- Node `node-backend-adapter-factory.ts:167-203`, `node-raw-binding-port.ts:122-131`은 readable
  poller에 `POLLIN`만 등록하고 promise binding runtime이 completion을 소유한다.

**최종 판정:** 대부분 **OWNED-CORRECTLY**. .NET reconnect poller만
**RE-IMPLEMENTS-LOWER-LAYER**이며 Core/binding sole completion/progress owner를 침범한다.

## 7. Concern 5 — errno 분류와 binding exception swallow

### .NET runtime inventory

| site | control | 판정 |
|---|---|---|
| `Runtime/Messaging/ZLinkRequestFailureMapper.cs:86-199` | typed `RequestResult`/`SubmitResult`를 Framework error kind로 한 곳에서 매핑한다. | **OWNED-CORRECTLY** — domain error projection. |
| 같은 파일 `:23-83` | wire의 Framework fine error code를 domain kind로 매핑한다. | **OWNED-CORRECTLY** — OS errno가 아니라 Framework protocol code다. 이름 `failureErrno`는 오해 소지가 있다. |
| `ZLinkManagedMeshNode.cs:10351-10381` | reply submit exception 종류 대부분을 `Terminated`, backpressure만 별도 값으로 축약한다. | **DUPLICATED** — binding typed result를 보존한 공통 mapper 하나여야 한다. |
| 같은 파일 `:11608-11627`, `:11846-11886`, `:12896-12918` | best-effort control/transport teardown에서 `ZlinkException`을 삼키거나 모두 NotConnected로 바꾼다. | best-effort 정책 자체는 Framework 소유이나 broad catch는 **DUPLICATED**; unexpected typed result를 diagnostics/terminal에서 잃는다. |
| `ZLinkClientServerClientRuntime.cs:1115-1188`, `:1441-1457`, `:1504-1513`, `:1605-1610`, `:1672-1700` | admission/control/liveness/reconnect exception을 모두 retry/continue/observed로 바꾼다. | **DUPLICATED** 및 일부 **RE-IMPLEMENTS-LOWER-LAYER**. |

.NET Framework에서 native errno 값으로 transport 의미를 재분류하는 production branch는 찾지
못했다. 문제는 typed terminal을 broad exception으로 다시 합치는 데 있다.

### Java runtime inventory

| site | control | 판정 |
|---|---|---|
| `runtime/internal/calls/ZLinkOneWayCalls.java:63-113` | `NOT_ADMITTED`를 native errno 101/107/111/113 및 Windows 값으로 다시 나눠 Unavailable/Backpressured를 고른다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| `runtime/actors/ZLinkActorSubmitFaults.java:14-56` | retryable typed 결과 외에 errno 11/16으로 BUSY/already-bound를 별도 판정한다. | **RE-IMPLEMENTS-LOWER-LAYER**이며 위 표와 두 번째 분류 owner다. |
| `ZLinkJavaStreamSocket.java:818-837` | close cleanup에서 typed route-unavailable 집합을 다시 정의해 failure를 삼킨다. | cleanup idempotency는 Framework 소유지만 표가 분산되어 **DUPLICATED**. |
| `ZLinkChannelSocketRegistry.java:842-861`, `:893-914`, `:931-961`, `:1077-1098`, `:1138-1163` | liveness/control/teardown의 sync submit 및 lifecycle exception을 다수 삼킨다. | best-effort control은 허용 가능하나 broad swallow는 **DUPLICATED**; physical reconnect로 바꾸는 부분은 Concern 2 위반. |
| `runtime/streams/ZLinkStreamRuntime.java:1492-1525` | heartbeat/session-close async terminal 실패를 FINE log 후 all-settled 처리한다. | **OWNED-CORRECTLY** — 한 session 실패가 liveness loop를 죽이지 않는 Framework 정책. Typed 원인은 log에 보존된다. |

`ZLinkOneWayCalls` 주석 자체가 binding async terminal이 exact route loss를 `NOT_ADMITTED`로
collapse한다고 적는다. 그렇다면 Framework errno table이 아니라 **Core/binding typed-result bug**다.
필요한 구분이 public typed enum에 없으면 binding follow-up을 먼저 만들고 Framework는 그 값만
매핑한다. 테스트 `ZLinkJavaRawMeshNodeM6ATest.oneWayAdapterDistinguishesRouteLossFromAdmissionTimeout`은
errno 숫자 대신 새 typed 결과를 주입하도록 바뀌어야 한다. `ZLinkActorSubmitFaultsTest`도 같은
공통 typed 정책만 검증해야 한다.

### C++ / Node 비교

- C++ `runtime/backend/raw_binding_adapter.hpp:79-107`은 errno 예외가 있더라도 한 adapter에
  집중되어 있다. 그러나 Core/binding typed result가 충분하다면 이 한 곳도 제거 대상이지 Java의
  여러 errno table을 정당화하지 않는다.
- Node adapter의 string/code 보정도 완전한 모범은 아니다. 기준은 “한 곳”보다 강한
  “binding typed result만 소비”다.

**최종 판정:** .NET central typed mapping은 **OWNED-CORRECTLY**, broad swallow는
**DUPLICATED**. Java raw errno 두 표는 **RE-IMPLEMENTS-LOWER-LAYER**이며 single owner는 Core의
result normalization과 binding typed exception이다.

## 8. Concern 6 — reply routing / request replay

### .NET runtime inventory

| site | control | 판정 |
|---|---|---|
| `ZLinkManagedMeshNode.cs:10289-10349` | 받은 opaque `ReplyOperation`을 캡처하고 reply wire/payload를 보관한다. | raw reply가 sync/backpressured인 계약에서 retry용 payload 보관은 **OWNED-CORRECTLY**. |
| 같은 파일 `:10351-10445` | 첫 Core submit 뒤 BACKPRESSURED만 queue하지만 재시도 가능 여부를 Framework peer/pair epoch로 제한한다. | backpressure 재제출 자체는 Framework reply wrapper 소유; pair epoch gate는 **RE-IMPLEMENTS-LOWER-LAYER**. |
| 같은 파일 `:8895-8977`, `:8998-9027` | physical disconnect에서 opaque reply capability를 invalid로 간주한다. | **RE-IMPLEMENTS-LOWER-LAYER** — Core는 exact reply token validity owner다. |
| `Runtime/Streams/ZLinkSessionActorCoordinator.cs:454-537` | bind confirm request를 any retry advice 또는 NotFound에 50 ms마다 재제출한다. | **RE-IMPLEMENTS-LOWER-LAYER** / Framework §9 위반. Pre-admission이 typed result로 증명된 경우만 새 attempt가 가능하다. |
| `ZLinkActorManagerService.cs:452-526` | actor create request를 NotConnected/Backpressured에서 loop한다. | **RE-IMPLEMENTS-LOWER-LAYER**. |

Core의 현재 contract tests는 반대 의미를 이미 고정한다.

- `test_phase3_request_reply_contract.cpp::test_admitted_request_survives_physical_detach_and_same_rid_reconnect_without_replay`
- `test_router_physical_disconnect_preserves_token_for_same_rid_reconnect`
- `test_router_reply_final_waits_for_same_rid_reconnect`
- `test_router_explicit_logical_rid_removal_invalidates_reply_token`

따라서 `_nativeReplyEpochsByTransportPair`를 삭제하고 opaque token submit 결과만 따라야 한다.
`CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`는 “exact physical
disconnect 뒤에도 same logical RID reconnect에서 Core token reply가 완료됨”으로 기대값을 뒤집어야
한다. `CanonicalActorJoinRequest_RetriesPreservedNativeReplyAfterBackpressure`와
`StatefulServiceRuntimeTests.RemoteActorRequest_RetriesPreservedNativeReplyAfterBackpressure`는
backpressure resubmit 검증으로 유지할 수 있다.

### Java runtime inventory

| site | control | 판정 |
|---|---|---|
| `runtime/actors/ZLinkActorRetryScheduler.java:52-87` | predicate가 true인 한 supplier 전체를 deadline까지 재호출한다. | **DUPLICATED** retry owner. |
| `ZLinkActorClientRuntime.java:169-203` | actor request를 route-not-connected에서 위 scheduler로 재제출한다. | terminal이 request completion까지 포함하므로 **RE-IMPLEMENTS-LOWER-LAYER** / §9 위반. |
| `ZLinkBoundSessionRuntime.java:163-174` | bind request를 `retryableBoundSessionBindFailure`로 재제출한다. | **RE-IMPLEMENTS-LOWER-LAYER**. |
| `ZLinkActorSubmitFaults.java:28-55` | NOT_CONNECTED/NOT_FOUND/TIMED_OUT request completion까지 retryable로 둔다. | **RE-IMPLEMENTS-LOWER-LAYER** — target 실행 여부를 모른 채 replay한다. |
| `ZLinkActorSpotJoinCall.java:682-717` | canonical join은 request를 한 번 제출하고 terminal만 domain error로 바꾼다. | **OWNED-CORRECTLY**. |

`ZLinkActorClientRuntimeTest.staleRequestInvalidatesCacheWithoutRetryingTheSameOperation`,
`ZLinkSessionActorBindingContractTest.staleStoredRouteFailsOnceWithoutLookupOrHiddenRetry`,
`ZLinkStreamPhysicalSubmitOwnershipTest.storedBindingRelayPropagatesOneBackendPhysicalErrorWithoutRetry`
가 removal proof다. 반대로 `ZLinkActorRetrySchedulerTest.bindRetryExhaustionSurfacesDeadlineExceededWithLastAttemptAsCause`
등 broad bind retry를 고정한 테스트는 pre-admission-only retry로 좁히거나 제거해야 한다.

### C++ / Node 비교와 별도 drift

- C++ raw request는 `raw_dealer_port.cpp:113-157`에서 binding async request terminal 한 번만
  기다린다. physical disconnect 뒤 reply token validity를 Framework epoch로 덮지 않는다.
- Node는 이 concern의 깨끗한 reference가 아니다. `service-stateful-runtime.ts:3870-3963`가 target
  operation terminal을 5분 보관하고, `:4000-4027`이 모든 request error에서 남은 deadline의 절반씩
  source request를 자동 replay한다. `m6b-user-spot-terminal-replay.contract.ts:68`,
  `m6b-runtime.contract.ts:860`이 이를 고정한다. Target dedup이 duplicate execution을 줄이더라도
  현재 Framework execution §9/§17과 Session §12는 timeout/route failure 후 자동 request replay를
  금지한다. 이는 lower-layer 보상이 아니라 **Framework spec drift**다. 별도 승인된 contract 변경이
  없다면 retry loop를 제거해야 한다.

**최종 판정:** opaque reply token과 binding request terminal이 single owner다. .NET pair epoch와
두 언어의 broad whole-request retry는 **RE-IMPLEMENTS-LOWER-LAYER**. Framework operation-ID dedup은
명시적 caller retry를 안전하게 받을 수는 있어도 자동 replay 권한을 만들지 않는다.

## 9. Concern 7 — Java STREAM state/thread ownership

Owner 계약은 Framework state-lane spec §3-§6이다. Socket access는 `ZLinkJavaStreamSocket`의
state lane 하나가 소유하고, receive readiness는 그 socket의 sole poller가 소유하며, 장기 binding
operation은 lane 밖에서 await해야 한다.

| site | control | 판정 |
|---|---|---|
| `ZLinkJavaStreamSocket.java:57`, `:102-115` | socket mutable state용 lane 하나와 sync bridge를 둔다. | owner 자체는 **OWNED-CORRECTLY**; bridge 호출 문맥을 제한하지 않아 아래 위반을 허용한다. |
| 같은 파일 `:117`, `ZLinkJavaSocketBacked.java:7-16` | lane에서 raw `Socket` 참조를 꺼낸 뒤 caller가 options를 직접 읽는다. | **DUPLICATED** authority / encapsulation leak. `admissionTimeoutOnLane` 같은 value query만 노출해야 한다. |
| `ZLinkJavaStreamSocket.java:120-147`, `:171-212`, `:257-283` | public sync methods가 `.join()` bridge로 socket lane에 들어간다. | public external sync surface면 **OWNED-CORRECTLY**; infrastructure call site에서는 금지. |
| 같은 파일 `:307-329` | async send가 lane turn에서 binding operation을 시작하고 turn 밖 future로 flatten한다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:567-640` | `close()`가 lane에 들어간 뒤 unbind futures 전체를 `.get(500ms)`로 기다린다. | **DUPLICATED** completion owner이자 lane §3 위반. Deadlock/500 ms stall 가능. |
| 같은 파일 `:864-883` | BoundSessionSink default async가 sync send를 호출한다. | **RE-IMPLEMENTS-LOWER-LAYER**, Concern 3과 동일. |
| `ZLinkJavaRawServicePort.java:44`, `:61-246`, `:314-325` | Router/socket access는 한 lane이 소유하나 open/send/request/reply/receive가 모두 sync `.join()` bridge다. Receive poll wait를 lane 밖에 둔 `:239-246`은 옳다. | owner는 **OWNED-CORRECTLY**; mesh pump infrastructure가 호출하는 sync bridge는 **DUPLICATED** execution owner. Async entry로 바꿔야 한다. |
| `ZLinkJavaRawMeshNode.java:183-187`, `:7615-7635` | descriptor, spot-node, user-terminal의 서로 다른 상태 집합에 lane 셋을 둔다. | 상태 집합이 분리돼 있으므로 자체로는 **OWNED-CORRECTLY**. 모든 helper가 `.join()`이고 pump/callback에서 호출 가능한 점은 위험 경계다. |
| `runtime/streams/ZLinkStreamRuntime.java:1492-1525` | heartbeat/control을 async terminal로 보내고 completion에서 log/정리한다. | **OWNED-CORRECTLY**. |
| `runtime/actors/ZLinkSessionActorsRuntime.java:1193-1222`, `:1346-1433` | command 36과 queued push가 async STREAM terminal을 사용하고 lane에는 claim/settlement만 수행한다. | **OWNED-CORRECTLY**. |
| 같은 파일 `:1164-1190`, `:1334-1344` | 병렬 sync entry가 current session send를 sync `DONT_WAIT`으로 수행한다. | production infrastructure에서 reachable하면 **DUPLICATED**. Async entry 하나로 수렴해야 한다. |

최근 `424b15684c`, `7b0590183d`, `efad74ef94`가 command 36, control frame, caller-thread
bound-session push를 async로 옮긴 방향은 맞다. 남은 sync bridge는 그 수렴을 완성하지 못한 잔여다.
증명 테스트는 `ZLinkJavaStreamSocketAsyncTerminalTest` 두 test,
`ZLinkStreamPhysicalSubmitOwnershipTest`의 async-only tests에 더해 다음 두 개가 필요하다.

- `MISSING`: close가 lane turn을 점유하지 않은 채 unbind terminal을 기다리고 timeout/close 경쟁에서
  socket owner가 계속 진행하는 test.
- `MISSING`: `ZLinkJavaRawMeshNode` pump/completion callback에서 RawServicePort operation을 시작해도
  어떤 `.join()`도 호출되지 않는 test.

**최종 판정:** async send/control의 current production path는 **OWNED-CORRECTLY**. Raw socket escape,
lane 안 blocking close, infrastructure sync bridge는 **DUPLICATED** owner이며 binding/state-lane
terminal을 따라야 한다.

## 10. 제거 순서와 증명 계획

| 순서 | 제거/수렴 대상 | 먼저 신뢰해야 할 Core/binding 계약과 하위 test | Framework removal proof |
|---:|---|---|---|
| 1 | .NET reciprocal standby 보상, lane mask, endpoint/RID disconnect 선택 | Core §4 HANDOVER. **MISSING:** real reciprocal ROUTER pair에서 loser 두 lane이 사라지고 양쪽 active direction이 동일하며 monitor pair identity가 terminal되는 test. 현재 동작이 standby라면 Core bug를 먼저 고친다. | `CanonicalActorJoinIngressReplyTests` handover 3종 + cross-language RouteMesh one-ready sample/test. |
| 2 | Java random connection ID, endpoint-key FIFO, lost-edge validation heuristic | Stable monitor `(connectionId,lane)` contract. **MISSING:** READY/DISCONNECTED/CLOSED가 같은 nonzero pair identity를 모든 transport에서 제공하는 Core+Java binding test. | `ZLinkJavaRawMeshNodeM6ATest` replacement tests를 real pair ID assertion으로 변경; one-ready peer contract. |
| 3 | .NET ClientServer `RestartPhysicalConnection`/`ReconnectAsync`/second poller와 admission timer; Java unchanged-endpoint disconnect/connect/new dealer | Core `zlink_connect` auto reconnect, polling lost-wake/progress contract. **MISSING:** unchanged endpoint 장애에서 app의 disconnect/connect/poll 없이 reconnect READY가 오고 socket request가 다시 가능해지는 binding test. | .NET `MalformedPushedControl_ReconnectsAndReadmits`, Java `reconnectAdmissionFenceRejectsPreviousPhysicalPipeReply`를 “Framework re-register 0회” 조건으로 재작성. |
| 4 | .NET/Java sync-to-async backend defaults와 infrastructure sync DONTWAIT paths | Binding async terminal owns provisional registration, WRITABLE drain, exact resubmit. Core `test_dealer_router_hwm_request_uses_writable_retry`, `test_router_writable_completion_is_scoped_to_the_drained_rid`; Java `DontWaitBackpressureContractTest.asyncSendRetriesExactPacketAfterWritableCompletion`; .NET `test_routed_async_admission.async_terminal_waits_for_writable_without_blocking_for_credit`. | Java `ZLinkStreamPhysicalSubmitOwnershipTest`, `ZLinkJavaStreamSocketAsyncTerminalTest`; .NET `EntrySpotActorDispatchTests.BoundSession_Uses_BindingOwned_Async_Admission_Without_Framework_Retry`. |
| 5 | Java errno tables, .NET/Java broad typed-terminal collapse | Binding error policy 3972-3977, 4234-4252. **MISSING:** async exact-route loss와 admission-capacity failure가 raw errno 없이 서로 다른 portable typed result로 나온다는 Java binding test. Coarse `NOT_ADMITTED`만 나오면 binding bug를 고친다. | Java one-way adapter와 ActorSubmitFaults tests를 typed-only로 변경; .NET RequestFailureMapping tests. |
| 6 | .NET reply pair epoch invalidation | Core reply token tests 네 개(Concern 6 목록). Physical disconnect는 token을 무효화하지 않고 explicit logical RID removal만 무효화한다. | Handover test 기대를 same-RID reconnect reply 성공으로 변경; existing preserved-reply backpressure tests 유지. |
| 7 | .NET `ConfirmBindingWithRetryAsync`/Actor create submit loop, Java `retryRouteUntil` request/bind replay | Framework execution §9/§17와 Session §12. Binding terminal은 한 request의 terminal-once만 소유한다. Pre-admission/accepted를 구별할 typed receipt가 없다면 **MISSING → binding/API follow-up**, Framework 추정 retry 금지. | Java `staleRequestInvalidatesCacheWithoutRetryingTheSameOperation`, `staleStoredRouteFailsOnceWithoutLookupOrHiddenRetry`; .NET `DirectNoBindTerminalDoesNotRetryTransientSubmitFailure` 계열. Broad retry를 기대하는 tests는 삭제/축소. |
| 8 | Java STREAM `nativeSocket`, `closeOnLane().get`, RawServicePort infrastructure `.join`, sync command-36 path | State-lane spec §3-§6; Core close 599-619. **MISSING:** Java binding async close/terminal이 lane 점유 없이 pending op을 정리하는 adapter-level test가 필요하면 binding follow-up. | 기존 async-terminal tests + 위 Concern 7의 두 MISSING Framework tests. |

## 11. 삭제 전 반드시 root에서 고칠 항목

다음은 Framework code를 먼저 지우면 단순 regression이 나는 항목이다. 그렇다고 현 보상 코드를
정본으로 승격하면 안 된다.

1. **Core reciprocal HANDOVER:** .NET 주석이 말하는 loser standby가 재현되면 Core §4 위반.
2. **Core/binding command progress:** disconnect terminal이 별도 application poller가 있어야 진행되면
   Core polling §3 lost-wake/progress 위반.
3. **Monitor pair identity:** Java가 endpoint FIFO/random UUID 없이는 READY와 DISCONNECTED를 짝지을
   수 없으면 binding monitor contract가 부족하다.
4. **Portable typed error:** Java가 `NOT_ADMITTED` 안에서 errno 숫자를 보지 않으면 route loss와
   capacity/admission refusal을 구별할 수 없다면 Core/binding result model이 부족하다.

이 네 항목은 모두 Core/binding follow-up이다. Framework workaround를 영구 규칙으로 유지하는
근거가 아니다.

## 12. History 관찰

`4bad5ac979..HEAD`에서 위 control이 추가된 대표 commit은 .NET
`b28eb24270`(HANDOVER settlement), `25952a76bc`(ClientServer close 관찰 후 re-register),
`ebff5b3e1b`(admitted inbound reuse), Java `424b15684c`, `7b0590183d`, `efad74ef94`
(STREAM async 수렴), `36c310ffef`/`.NET 4644af9d03`(SUB completion owner 회수),
`6682ae0db1`/`dc9fe76100`(exception/close 보상)이다.

이 기록은 두 방향을 동시에 보여 준다. STREAM send와 SUB polling은 lower-layer owner로 수렴하고
있지만, Mesh handover와 ClientServer reconnect는 gate failure를 닫는 과정에서 Framework에 새
physical state machine을 추가했다. 다음 campaign은 실패별 patch보다 위 제거 순서대로 owner를
하나씩 없애는 편이 맞다.
