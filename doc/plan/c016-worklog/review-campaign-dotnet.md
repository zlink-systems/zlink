# .NET Framework 0.17.0 DONTWAIT campaign conformance review

## 결론

검토 대상 여덟 commit 가운데 0.17.0 계약 적응으로 유지할 commit은
`7e655e3703`의 pull-completion baseline과 `4d263e66b9`의 DONTWAIT/WRITABLE 경로다.
`af7afd28e7`의 runtime 변경은 0.17.0 적응이 아니라 기존 Framework 결함의 교차 언어
수정이다. 나머지 네 commit에는 binding 우회, 원인이 입증되지 않은 monitor 기반 보정,
또는 handover 경쟁을 없애 test를 통과시키는 fixture 변경이 있다.

판정은 각 commit에 정확히 하나의 주 class를 부여했다. 한 commit 안에 성격이 다른 hunk가
있으면 주 class는 가장 위험한 runtime/test 의미 변경으로 정하고, 하위 hunk의 class를 근거에
명시했다. `b28eb24270`, `25952a76bc`, `6b77ba013f`, `ebff5b3e1b`는 hash를 직접 포함한
`doc/plan/c016-worklog/*.md`가 없었다. 따라서 각각 제목과 변경 내용을 직접 대응할 수 있는
`bucketB-dotnet-handover-resend-summary.md`, `bucketB-dotnet-cross-spotroute-summary.md`,
`bucketF-dotnet-relocated-reply-relay-summary.md`,
`bucketF-dotnet-tcp-same-endpoint-replacement-summary.md`,
`bucketF-dotnet-clientserver-readmit-summary.md`,
`core-ctx-term-teardown-hang-summary.md`,
`bucketF-dotnet-handover-liveness-hang-summary.md`,
`bucketB-dotnet-tictactoe-joingamenotify-summary.md`를 대응 worklog로 읽었다.

| commit | one-line | class | 계약·근거 | C++/Node parity | verdict |
|---|---|---|---|---|---|
| `7e655e3703` | .NET runtime을 0.16 pull-completion 모델로 전환 | **A** | 0.17 적용의 선행 baseline이다. `bindings/doc/spec/dotnet/README.ko.md` **Send·request terminal**(`:333-341`)과 **Pull completion 공개 계약**(`:692-710`)은 `Submit()`/`Async()`, opaque `ReplyToken`, public poller drain owner를 요구한다. 변경은 `ZLinkManagedMeshNode.cs`의 pull receive·reply token·poll-completion 경로와 Stream receive 단일 owner를 그 계약에 맞췄다. `_currentAdmission` guard는 별도 **B** hunk다. 반복 Hello가 live connection generation/liveness epoch를 바꾸지 않아야 한다는 `06-wire-protocol.ko.md` **§5**(`:366-371`)를 이전 코드의 monitor trigger와 retry trigger가 위반했다. 이는 0.17 때문에 생긴 결함이 아니라 0.16 API 전환이 두 trigger의 경쟁을 노출한 latent defect다. | C++ `b32d4cae64`, Node `360181172f`도 같은 pull 전환을 했다. Node는 `raw-service-mesh-runtime.ts:386-393`에서 이미 admitted peer의 Hello를 건너뛴다. C++도 topology-admitted peer와 후보 registry를 분리하므로 .NET의 `_currentAdmission`과 동일한 추가 hunk는 필요하지 않았다. | **KEEP** |
| `4644af9d03` | SUB를 public poller 대신 `ZlinkPoll.Poll`로 분기 | **C** | `ZLinkBackendSocketPoller.cs:19-38,65-99`의 socket-type 분기는 Framework 계약이 아니라 당시 .NET binding `Poller.Add`가 `PollCompletion`을 요청하지 않은 SUB에서도 completion owner를 조회한 결함을 우회한다. Core `05-polling.ko.md` **§3-4**(`:43-66,83-101`)는 SUB `POLLIN`과 completion-capable socket의 `POLLCOMPLETION`을 분리한다. Worklog D-074도 root를 binding으로 확정했다. 현재 binding은 `bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs:39-58`에서 `PollCompletion`을 요청할 때만 owner를 얻으므로 우회의 전제가 이미 사라졌다. | C++/Node Framework에는 SUB 전용 poller class가 없다. 두 binding은 read-only `POLLIN` 등록에서 completion owner를 얻지 않아 동일 변경이 필요하지 않았다. 언어 구조 차이가 아니라 .NET binding 고유 결함이었다. | **REVERT** — root binding fix 유지 후 SUB 특례 제거 |
| `4d263e66b9` | DONTWAIT send를 binding async 경로로 보내고 `POLLOUT`/completion drain | **A** | Core `socket/README.ko.md` **Part send와 pending admission**(`:930-970`)은 DONTWAIT를 1회 admission, BACKPRESSURED+wait token, payload caller 보관, WRITABLE 뒤 재제출로 정의한다. `05-polling.ko.md` **§3-4**(`:53-66,83-101`)는 unread WRITABLE 동안 `POLLOUT`과 `POLLCOMPLETION`을 level로 유지하고 단일 owner가 drain하도록 한다. .NET의 `ZLinkBackendStreamSocketWrapper.cs:125-164,341-350`, `ZLinkManagedMeshNode.cs:330-337`, `:12896-12923`은 `TrySubmit()`과 public poller path를 사용한다. 이는 binding이 wait token을 drain하고, awaitable helper 또는 Framework queue owner가 보관한 payload를 WRITABLE 뒤 재제출하게 하는 데 필요한 변경이다. | C++는 같은 commit에서 `.async()`, Node는 `submitBindingAsyncSend`, Java는 `submit()` 경로로 바뀌었다. 네 언어 모두 동등 변경이 필요했다. | **KEEP** |
| `af7afd28e7` | ZoneWorld crash boundary를 .NET/Java/Node에 port | **B** | Runtime hunk는 target lifecycle generation이 더 이상 admitted가 아니면 Actor Join timeout을 `Unavailable`로 정규화한다(`ZLinkActorRemoteJoiner.cs:527-555`, `ZLinkManagedMeshNode.cs:11797-11820`). `05-spot-actor-membership.ko.md` **§4 Failed.Kind**는 호환 target 부재를 `Unavailable`, 실제 commit deadline 만료를 `DeadlineExceeded`로 구분한다(`:316-330`). Edge flag 없는 ready-count snapshot을 새 admission으로 처리하지 않는 hunk(`ZLinkManagedMeshNode.cs:8794-8800`)는 Core `06-monitoring.ko.md` **§3.2**(`:88-94`)를 따른다. 0.17이 만든 문제가 아니다. 기존 ZoneWorld proxy가 ZMP request/reply의 8-byte sequence를 잘못 잘라 G4 경계를 가렸고, proxy parser 수정 뒤 SIGKILL fault-injection에서 latent Framework 오류 분류가 드러났다. Proxy parser 자체는 **E**다. | Golden C++ `2f1de0b56d`가 `mesh_node_runtime.cpp:2325-2331,2834-2854`에 같은 dead-generation 정규화를 하고 `raw_mesh_node_owner.cpp:3858-3870`에서 edge-less snapshot을 무시한다. Node도 `actor-local-native-join.ts:650-669`에 같은 mapping이 있고 ready edge guard는 `raw-service-mesh-runtime.ts:965,1562`에 이미 있었다. 동등 runtime 수정이 실제로 필요했다. | **KEEP** |
| `b28eb24270` | mesh admission에 reciprocal settlement, 후보 귀속, pair re-pin, endpoint ownership 추가 | **D** | Commit 전체 class는 새 `Admitted` barrier 때문에 **D**다. `ZLinkManagedMeshNode.cs:11673-11698`은 같은 lifecycle의 reciprocal duplicate에도 두 lane의 `Disconnected`를 모두 기다린 뒤 `Admitted`를 공개한다. 그러나 `03-mesh-node.ko.md` **§7.1**(`:303-313`)의 이전-pipe 종료 조건은 *manual fixed-RID의 다른 generation 선택* 조건이고, reciprocal same-generation admission barrier가 아니다. Core `socket/README.ko.md` **RID 중복 정책**(`:155-165`)은 패배 방향의 이미 admit된 request가 timeout으로 한 번 끝나고 caller가 handover 뒤 다시 보낸다고 정한다. Framework spec 어디에도 그 timeout window를 숨기기 위해 admission 공개를 늦추라는 규칙은 없다. 네 하위 부분의 개별 판정은 다음 절에 분리한다. | C++ automatic planner는 낮은 RID만 dial하지만 manual bilateral도 지원한다. `service_topology_registry.cpp:281-309`은 RID 방향으로 survivor를 고른 즉시 topology에 admit하며, `raw_mesh_node_owner.cpp:2842-2924`도 반대 lane disconnect를 기다리지 않는다. Node도 `raw-service-mesh-runtime.ts:740-767`에서 topology/liveness admission을 즉시 commit한다. C++/Node 모두 두-lane settlement나 relay pair re-pin이 없고 같은 Core gate를 통과한다. 따라서 .NET만의 구조적 필요가 아니라 .NET의 monitor→pair 추정이 Core survivor와 갈라진 다른 root의 증상이다. | **RE-FIX AT ROOT** — .NET ingress/source attribution과 admission owner. 안전한 B hunk만 분리 |
| `25952a76bc` | ClientServer reconnect가 close edge를 기다리고 generation retry를 fence | **B** | `05-transport-liveness.ko.md` **§5-6**(`:221-246,259-276`)은 old pair의 close snapshot/disconnect를 관찰한 뒤에만 같은 endpoint의 새 connection을 만들고, 이전 ready/admission state를 재사용하지 말라고 한다. 이전 코드는 `Disconnect` 뒤 고정 100 ms만 기다렸으므로 이 조항을 직접 위반했다. 현재 `ZLinkClientServerClientRuntime.cs:1018-1037,1571-1709`은 Connection 인스턴스가 단독 소유하는 DEALER/endpoint의 `Disconnected/Closed`를 기다리고 generation별 retry를 fence한다. 수정 전 focused 20/20과 class 35/35가 통과한 이유는 보통 100 ms 안에 비동기 close가 끝났기 때문이고, full-gate 부하가 그 latent race를 노출했다. Worklog에 delayed edge 자체를 찍은 trace가 없다는 증거 약점은 남지만, old code의 spec 위반과 수정 경계는 독립적으로 명확하다. | C++ `channel_outbound_exchange.cpp:803-835`에는 malformed pushed-control을 받고 물리 연결을 강제 재시작하는 경로 자체가 없고 monitor는 `:675-712`에서 ready count만 갱신한다. Node는 invalid pushed control에서 ready만 제거한다(`channel-socket-registry.ts:1253-1295`); liveness reconnect는 `:879-885`에서 즉시 `disconnect/connect`하므로 동일 상황의 보장을 구현하지 않았다. .NET만 동등 변경이 필요한 이유는 이 runtime만 malformed control을 physical reconnect+re-admission으로 복구하기 때문이다. Core D-086 보정이라기보다 명시된 Framework close fence의 구현이다. | **KEEP** — exact edge correlation 회귀를 보강 |
| `6b77ba013f` | Canonical fixture가 DEALER를 모두 소유하고 handover 순서를 변경 | **C** | `_createdSources`로 모든 DEALER를 닫는 부분(`CanonicalActorJoinIngressReplyTests.cs:1135-1148,1161-1180,1323-1328`)과 bounded teardown test는 **E**이며 필요하다. Core `socket/README.ko.md:486`의 “context 종료 전에 socket close” 계약을 지키고, dump와 public C repro가 leak을 확정했다. 그러나 같은 commit은 prior reconnect를 끄고, prior를 먼저 dispose하며, Hello를 2초 동안 반복한다(`:620-669,1172-1267`). 이로써 HANDOVER가 정의하는 live duplicate 경쟁을 제거하고 lost/wrong Admit을 retry로 숨긴다. 특히 `RouteAdmission_PriorHelloThenExactDisconnect...`는 이제 `quiescePrior:true`, liveness test는 `disconnectPrior:true`라 원래 관찰하던 admission이 아니다. 가장 유력한 별도 root는 worklog가 측정한 TCP same-RID handover churn/latency와 .NET runtime의 physical-pair attribution이다. | C++ fixture는 RAII/socket owner로 exception path에서도 handle을 닫으므로 leak 수정과 같은 runtime change가 필요 없다. C++ duplicate tests(`test_cpp_framework_m6a_runtime.cpp:515-563,769-850`)는 live bilateral 후보를 유지한 채 survivor를 검증한다. Node에는 이 .NET helper와 동등한 raw DEALER fixture가 없으며, runtime에서 retry로 HANDOVER 경쟁을 제거하지도 않는다. 따라서 ownership fix는 harness parity지만 ordering hunk는 parity가 아니다. | **RE-FIX AT ROOT** — ownership hunk만 KEEP, quiesce/disconnect/retry test 변경은 원래 경쟁을 복원 |
| `ebff5b3e1b` | auto-connect가 같은 RID/endpoint의 admitted inbound peer를 재사용 | **C** | `ZLinkManagedMeshNode.cs:379-404`는 `ConnectPeer`가 outbound를 만들지 않고 inbound intent ID를 반환하게 한다. `01-channel-topology.ko.md` **§8**(`:457-469`)은 automatic의 lower-RID initiator와 manual bilateral duplicate collapse를 정하지만 “나중의 automatic intent가 기존 inbound manual/early connection을 자기 intent로 재사용한다”는 규칙은 없다. 변경은 reciprocal 후보 뒤 .NET Framework route와 Core survivor가 갈라져 command 36이 유실되는 증상을 후보 생성 자체를 막아 회피한다. 가장 유력한 root는 `b28eb24270`의 RID→monitor pair/current-source 추정이다. | C++ `mesh_node_runtime.cpp:1891-1917`은 local endpoint intent만 중복 제거하고, raw `connect_peer`(`raw_mesh_node_owner.cpp:767-831`)는 admitted inbound를 검색하지 않고 실제 connect한다. Node `raw-service-mesh-runtime.ts:247-289`도 매번 `connectToRoutingId`를 호출한다. 두 구현은 Core handover/Framework duplicate admission으로 수렴하며 inbound intent를 반환하는 동등 변경이 없다. 구조 차이보다 .NET의 다른 pair root를 가리는 증거다. | **RE-FIX AT ROOT** — .NET pair/source fence를 고친 뒤 cross-direction reuse 제거 또는 spec화 |

## `b28eb24270` 네 부분의 별도 판정

| 부분 | class | 판정 근거 | 조치 |
|---|---|---|---|
| `BeginReciprocalHandoverSettlement` / `ObserveReciprocalHandoverDisconnect` | **D** | `ZLinkManagedMeshNode.cs:8465-8499,8841-8891,9062-9084,11673-11698`이 ready까지 간 패배 방향의 Application/Completion lane mask `0b11`을 만들고 모두 끊길 때까지 survivor를 `Connecting`으로 숨긴다. Core §RID 중복 정책은 이미 admit된 loser request의 timeout과 caller resend를 명시한다. C++/Node는 admission을 즉시 공개한다. | 제거하고 Core가 선택한 logical RID survivor를 그대로 사용한다. Framework가 정말 barrier를 소유해야 한다면 먼저 별도 spec 승인이 필요하다. |
| unilateral Hello fallback + `FindReadyOutboundCandidate` / `_readyOutboundEndpoints` | **B** | C++ listen-only peer가 같은 unilateral pipe로 보낸 Hello를 .NET이 별도 inbound로 오인하여 유일한 outbound endpoint를 끊던 결함이다. Manual 한쪽 endpoint 허용(`01-channel-topology.ko.md:467-469`)을 이전 코드가 위반했다. 0.17과 무관하며 cross-language topology가 latent defect를 노출했다. C++ `raw_mesh_connection_candidates_t::for_handshake`(`raw_mesh_node_owner.cpp:445-476`)는 선호 방향이 없으면 유일한 반대 방향 후보로 fallback한다. | KEEP. 다만 boolean RID/endpoint set 대신 C++처럼 connection candidate registry 한 곳에서 귀속을 소유하는 편이 안전하다. |
| relay RID→pair re-pin | **C** | `CompletePeerAdmissionUnderLock`과 ready handler(`ZLinkManagedMeshNode.cs:8590-8604,8801-8835`)가 `_transportPairsByRid`를 survivor로 다시 쓴다. 이는 command 33이 `current_source=False`로 drop되는 증상을 없애지만, raw `Received`에는 physical pair가 없어서 실제 ingress pair를 증명하지 못한다(`ProcessReceived`: `:5586-5605`). C++는 routed receive가 RID만 준다는 사실을 명시하고 admission command/direction 후보를 고르며(`raw_mesh_node_owner.cpp:445-476`), application/control frame을 최신 monitor pair와 비교해 버리지 않는다. Node도 connection candidate는 관리하지만 relay 수신을 RID→최근 monitor pair re-pin으로 승인하지 않는다. | `_transportPairsByRid`를 authorization fence로 쓰는 root를 재설계한다. Physical ingress identity가 필수면 binding/Core capability가 먼저다. |
| `DisconnectTransport` endpoint-vs-RID | **B** | 이전 코드는 admitted outbound intent를 제거해도 `DisconnectRid`만 호출해 endpoint reconnect intent를 남겼다. 같은 endpoint의 새 RID가 old RID를 상속한 것은 manual 한쪽 연결과 lifecycle identity(`01-channel-topology.ko.md:467-469,511-519`)를 위반한다. 현재 `:11846-11886`의 “outbound 단독 owner면 endpoint, shared/inbound면 RID” 규칙은 responsibility에 맞는다. C++도 현재 `raw_mesh_node_owner.cpp:919-944`에서 다른 owner가 없으면 `disconnect(endpoint)`와 `_outbound_endpoints.erase`를 한다. C++에는 동일 root fix `44b9b27efc`가 따로 필요했다. | KEEP. Node `disconnectPeer`는 `disconnectRid` 우선이라 same-endpoint replacement parity test가 추가로 필요하다. |

## `ZLinkManagedMeshNode.cs`의 C++ 비대칭 monitor 기반 결정

다음은 단순 관찰/진단이 아니라 Core monitor event가 Framework의 admission, message acceptance,
liveness 또는 reply 결과를 바꾸며 C++가 같은 결정을 하지 않는 지점이다.

| .NET 위치 | monitor event로 내리는 결정 | C++ 차이 |
|---|---|---|
| `:8294-8311`, `:8801-8835` | `ConnectionReady`의 RID·remote endpoint로 outbound/inbound를 분류하고, `_ready*` set과 단일 `_transportPairsByRid[RID]`를 갱신한다. 이 값으로 admission candidate와 ingress pair를 정한다. | C++ `raw_mesh_node_owner.cpp:3880-3910`은 event를 여러 physical candidate registry에 추가할 뿐 admitted peer나 단일 RID current-source를 바로 바꾸지 않는다. Wire command가 도착할 때 `for_handshake`가 모든 후보에서 방향을 고른다. |
| `:5128-5192`, `:5586-5605` | raw receive에는 RID만 있는데 최신 monitor-derived pair를 붙여 `HasCurrentInfrastructureControlSource`/`HasCurrentApplicationSource`가 control과 application record를 accept/drop한다. | C++는 application receive를 admitted logical RID로 검사하며 “현재 monitor pair와 같은가”를 추가 veto로 쓰지 않는다. Physical ID가 없는 receive에 pair identity가 있다고 가정하지 않는다. |
| `:6133-6153`, `:8895-8976`, `:10437-10455` | monitor pair를 `ZLinkNativeReplyPeerEpoch`에 붙이고 교체/Disconnected에서 invalidation하여 captured reply를 허용·거부한다. `ConnectionReady`만으로 admitted transport가 바뀌면 liveness connection generation도 회전한다(`:8937-8946`). | C++는 opaque Core reply token에 reply route를 맡기고 Framework monitor pair epoch로 terminal reply를 veto하지 않는다. Liveness는 wire admission이 성공할 때 `service_liveness_registry::admit`한다(`raw_mesh_node_owner.cpp:2858-2870`). |
| `:8465-8499`, `:11673-11698` | ready set에 loser가 있었다는 사실로 reciprocal settlement를 시작하고 survivor의 `Admitted` 공개를 보류한다. | C++ `service_topology_registry.cpp:281-309`은 canonical direction을 고른 즉시 admit한다. Node도 즉시 topology/liveness에 반영한다. |
| `:8841-8891`, `:8998-9084` | `Disconnected`의 lane 0/1을 bit mask로 세어 둘 다 관찰해야 `CompletePeerAdmissionUnderLock`과 `PeerAdmitted`를 실행한다. 늦은 pair disconnect인지도 monitor pair equality로 판정한다. | C++는 exact candidate의 disconnect를 topology/liveness에서 제거하지만 두 lane을 admission barrier로 합치지 않고, disconnect가 새 admission의 공개 trigger도 아니다. |

Edge flag 없는 `ConnectionReady` snapshot을 무시하는 `:8794-8800`은 C++
`raw_mesh_node_owner.cpp:3858-3870`과 동일하므로 위 비대칭 목록에서 제외했다. Disconnected로 exact
candidate를 제거하고 stale disconnect가 새 peer를 내리지 않게 하는 기본 동작도 양쪽에 있으나,
.NET만 그것을 two-lane admission barrier와 reply/application source fence로 확장한다.

## 재수정 권고

1. **P0 — `b28eb24270`의 physical-source 추정과 settlement를 분리한다.**
   `BeginReciprocalHandoverSettlement`와 RID→pair re-pin을 제거한 상태에서 bilateral,
   relocated reply relay, spot-route를 같은 Core로 다시 대조한다. unilateral fallback과
   endpoint-owner disconnect는 독립 B fix로 보존한다. C++처럼 admission 후보 registry가
   monitor connection을 소유하되, raw application receive에는 존재하지 않는 physical identity를
   만들어 붙이지 않아야 한다.

2. **P0 — `6b77ba013f`를 ownership fix와 scenario 변경으로 분리한다.** `_createdSources`와
   `HandoverAdmissionFailure_DisposeClosesUnpublishedReplacement`는 유지한다. 기존 live prior와
   replacement가 실제 경쟁하는 RouteAdmission test를 복원하고, default helper가 Admit을 반복
   전송하지 않게 한다. TCP same-RID latency는 별도 Core test로 판정한다.

3. **P1 — `ebff5b3e1b`는 pair root 수정 뒤 재평가한다.** C++/Node와 같은 실제 reciprocal
   connect에서도 command 36이 전달되면 inbound intent reuse를 되돌린다. 재사용 정책이 제품
   의도라면 mixed manual/automatic topology 규칙과 intent ownership을 먼저 spec에 추가한다.

4. **P1 — `4644af9d03`의 SUB 특례를 제거한다.** 현행 binding의 조건부 completion-owner
   획득을 contract test로 고정한 뒤 ordinary `IPoller` `POLLIN` 경로로 합친다.

5. **P1 — `25952a76bc`의 증거를 보강한다.** 동일 DEALER/endpoint에서 old pipe의
   `connection_id`와 기다린 `Disconnected/Closed`를 대응시키는 회귀를 추가한다. C API에서도
   close edge 뒤 재등록이 old pipe를 다시 선택한다면 그때는 D-086 계열 Core 결함으로 분리한다.
   Node의 malformed pushed-control 복구 부재와 immediate liveness reconnect도 별도 parity 항목이다.

6. **P1 — unknown RID 결과 mapping을 다시 감사한다.** 이번 리뷰 전제의
   `NOT_FOUND+ENOENT`를 권위로 삼는다면 `ZLinkSubmitFailureMapper`와 stream/session wrapper가
   `NotConnected`로 평탄화하지 않는지 focused contract test를 추가한다.

## 스펙 gap 후보

1. **Unknown RID 결과가 리뷰 전제와 repository spec에서 충돌한다.** 본 리뷰 전제는 ROUTER
   submit의 unknown RID를 `ZLINK_SUBMIT_NOT_FOUND+ENOENT`로 정하지만, 현행
   `core/doc/spec/core/socket/README.ko.md:936,949-950`과
   `07-router.ko.md:173-178,202-205,215-222`는 DONTWAIT SEND/REQUEST를
   `NOT_CONNECTED+EHOSTUNREACH`로 적는다. 구현 판정 전에 하나로 확정해야 한다.

2. **Framework physical pair fence와 binding 공개 계약이 양립하지 않는다.**
   `05-transport-liveness.ko.md:239-246`은 `transportPairId`와
   `transportPairGeneration`으로 exact close를 요청·관찰하라고 하지만,
   `bindings/doc/spec/dotnet/README.ko.md:797-805`는 그 property를 공개하지 않고 monitor
   `connection_id`를 진단/correlation 전용이며 reconnect fence가 아니라고 한다. 실제 contract
   test도 `CanonicalActorJoinIngressReplyTests.cs:673-677`에서 두 property가 없음을 확인한다.

3. **Routed receive의 physical source identity가 정의되지 않았다.** Core receive는 logical RID와
   opaque reply token만 주는데 .NET Framework는 monitor event의 최신 pair를 수신 frame에 사후
   결합해 stale/current를 판정한다. C++는 이 판정을 하지 않는다. Physical ingress fence가
   필요하면 receive capability를 명시해야 하고, 필요하지 않으면 Framework spec은 logical RID
   handover 뒤 frame acceptance를 정의하고 .NET pair veto를 금지해야 한다.

4. **Reciprocal HANDOVER에서 `Admitted` 공개 시점이 비어 있다.** 현재 Framework spec은 ready
   connection 하나만 남긴다고만 하고, Core는 loser request timeout과 caller resend를 정한다.
   패배 lane의 두 `Disconnected` 뒤에만 `Admitted`를 공개한다는 .NET 규칙은 없다. C++/Node처럼
   즉시 공개할지, barrier를 둘지, barrier가 있다면 누가 lane completeness를 보장할지 명시해야 한다.

5. **Caller resend의 주체와 operation 종류가 불명확하다.** Core §RID 중복 정책의 “Caller는
   handover 뒤 다시 보낸다”가 application의 새 operation인지, binding의 pre-admission resubmit인지,
   Framework durable lifecycle operation의 동일-ID replay인지 구분해야 한다. 일반 Actor/Spot/Session
   request의 자동 재제출 금지와 함께 한 절에서 닫아야 settlement workaround가 반복되지 않는다.

6. **Mixed manual/automatic connection의 기존 inbound 재사용 규칙이 없다.** 낮은 RID의 automatic
   planner가 동작하기 전에 높은 RID의 manual/early connection이 inbound로 admitted된 경우,
   lower node가 canonical outbound를 실제로 만들어 HANDOVER할지, 기존 inbound를 automatic intent로
   채택할지 정의가 없다. C++/Node와 .NET이 현재 다르게 동작한다.

7. **ClientServer same-endpoint 재등록의 완료 경계가 binding으로 표현되지 않는다.** Framework
   liveness spec은 exact pair close 관찰을 요구하지만 .NET public surface에서는 exact pair를
   reconnect fence로 쓸 수 없다. `disconnect(endpoint)`가 terminal event 전에 같은 socket의
   `connect(endpoint)`를 허용하는지, 아니면 physical generation마다 새 socket을 만들어야 하는지
   Core/binding/Framework 책임을 정해야 한다.

이 검토에서는 source와 기존 worklog만 읽었고 build·test·gate는 실행하지 않았다.
