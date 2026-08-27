# Socket lock 경로별 재분류 감사

> 이 문서는 기존 CP3의 socket·dispose 분류를 Core의 send, receive, control, close 경로별
> 계약에 대조해 어떤 lock을 제거할 수 있고 어떤 framework 상태 보호가 남는지 답한다.

## 1. 결론

Core의 현재 계약은 socket 전체를 한 등급으로 취급하지 않는다. `send`는 여러 application
thread에서 동시에 호출할 수 있고, control 경로는 Core가 직렬화한다
(`core/doc/guide/11-thread-safety.ko.md:18-30`,
`core/doc/spec/core/socket/README.ko.md:44-55`). 반면 receive는 같은 socket의
single-consumer이고 close는 진행 중인 API·callback과 충돌하면 `EBUSY`를 반환하는 lifecycle
gate다(`core/doc/guide/11-thread-safety.ko.md:24,33-40`). 이 계약으로 기존 비교 수 371개를
다시 나누면 다음과 같다.

| 분류 | Java | C++ | .NET | 합계 |
|---|---:|---:|---:|---:|
| [중복-제거가능] | 31 | 0 | 0 | **31** |
| [결함-multipart누적] | 0 | 0 | 0 | **0** |
| [필요-receive] | 5 | 4 | 0 | 9 |
| [필요-프레임워크상태] | 22 | 84 | 33 | 139 |
| [필요-lifecycle] | 35 | 126 | 15 | 176 |
| [판정불가] | 0 | 16 | 0 | 16 |
| 합계 | **93** | **230** | **48** | **371** |

따라서 기존 371이라는 비교 분모에서 **확정적으로 제거 가능한 취득은 31개**다. 모두 Java
binding wrapper의 단일 send/request/reply 또는 단일 control 호출이다. close/dispose와
framework 상태 전이를 함께 지키는 gate는 제거 가능분에 넣지 않았다. 발견 7의 “외부 await를
품는 작업 프로토콜 직렬화” 판정도 그대로 유지했다.

**[결함-multipart누적]은 0건이다. 전부 단일 호출 로컬 조립이다.** Java는 호출 지역의 operation
builder에 part를 추가하고 즉시 `submit()`한다
(`ZLinkJavaSocketSupport.java:39-100`). C++ raw port도 지역 `operation`을 만든 뒤 같은 함수에서
`async()`/`submit()`한다(`raw_dealer_port.cpp:43-57,74-89,106-121`,
`raw_route_port.cpp:72-90,134-151,273-290`). .NET도 지역 `Message[]`를 만들고 같은 메서드에서
`Messages(...).Async()` 또는 `Submit()`까지 끝낸다
(`ZLinkManagedMeshNode.cs:4397-4434,9166-9198,10799-10836`). 다음 호출로 이어지는 header/body
조각이나 미완성 send builder를 field에 보관하는 구조는 발견하지 못했다.

### 계수 단위 주의

세 숫자는 원래부터 완전히 같은 단위가 아니다.

- Java 93과 C++ 230은 기존 CP3 표의 lock 취득 위치 수다.
- .NET 48은 `cp3-audit.ko.md:20-47`의 “async 경계 snapshot” 수다. 실제 lock 전체 수나
  socket/dispose lock 수가 아니다. 해당 표에서 socket/dispose로 파일 전체를 분류할 수 있는
  `ZLinkManagedMeshNode` 20, `ZLinkClientServerClientRuntime` 4, dispatch/follower/runtime dispose
  gate 7을 더해도 31개다. 이 감사는 사용자가 지정한 371 비교를 보존하기 위해 .NET 48개
  snapshot source를 재분류했지만, 48을 socket lock 재고로 다시 인용하면 안 된다.
- C++ `channel_outbound_exchange.cpp`는 기존 표가 30개라고 기록하지만 현재 source에는 RAII
  취득이 16개만 남아 있다. git과 이력 조회가 금지된 읽기 전용 범위라 사라진 14개는 현재
  source에서 역매핑할 수 없어 [판정불가]로 남겼다.

## 2. 언어별 분류표

### 2.1 Java — 93개

| 파일 · 취득 범위 | 수 | 분류 | 근거 |
|---|---:|---|---|
| `runtime/actors/ZLinkBoundActor.java:483` | 1 | [필요-프레임워크상태] | exact-once `disconnect` future를 설치한다. |
| `runtime/actors/ZLinkDeferredActorJoinScope.java:200,307,315` | 3 | [필요-프레임워크상태] | seal, intent, claim과 activation 상태를 함께 지킨다. |
| `runtime/binding/ZLinkJavaDealerSocket.java:27-49` | 9 | [중복-제거가능] | control 5개와 send/request의 바깥·안쪽 lock 4개다. send/request의 multipart builder는 한 호출 안에서 submit된다. |
| `runtime/binding/ZLinkJavaDealerSocket.java:56-57` | 2 | [필요-receive] | 같은 DEALER receive의 single-consumer를 지킨다. 중첩 lock 둘이 모두 최소 필요하다는 뜻은 아니며 receive 경로 자체에는 gate 하나가 필요하다. |
| `runtime/binding/ZLinkJavaDealerSocket.java:68-69` | 2 | [필요-lifecycle] | poller close와 socket close를 같은 wrapper 수명 전이로 묶는다. |
| `runtime/binding/ZLinkJavaRouterSocket.java:28-42,58-80` | 17 | [중복-제거가능] | option/query/endpoint 14개와 send/request/reply 3개다. 모두 Core가 허용·직렬화하는 단일 호출이다. |
| `runtime/binding/ZLinkJavaRouterSocket.java:48` | 1 | [필요-receive] | ROUTER receive single-consumer다. |
| `runtime/binding/ZLinkJavaRouterSocket.java:84` | 1 | [필요-lifecycle] | poller와 socket close를 묶는다. |
| `runtime/binding/ZLinkJavaSocketReceivePoller.java:34` | 1 | [필요-프레임워크상태] | `registered`와 poller registration을 exact-once로 전이한다. |
| `runtime/binding/ZLinkJavaSocketReceivePoller.java:46` | 1 | [필요-receive] | 재사용 `PollEvents`와 poll wait의 단일 소비자를 지킨다. |
| `runtime/binding/ZLinkJavaSocketReceivePoller.java:72` | 1 | [필요-lifecycle] | `closed` 전이와 poller close를 묶는다. |
| `runtime/binding/ZLinkJavaSubscriberSocket.java:23-27` | 5 | [중복-제거가능] | bind/connect/disconnect, validation, subscription control 단일 호출이다. |
| `runtime/binding/ZLinkJavaSubscriberSocket.java:33` | 1 | [필요-receive] | SUB receive single-consumer다. |
| `runtime/binding/ZLinkJavaSubscriberSocket.java:44` | 1 | [필요-lifecycle] | poller와 socket close를 묶는다. |
| `runtime/channels/RuntimeEndpointConnections.java:20-60` | 5 | [필요-프레임워크상태] | endpoint 목록과 현재 attached socket을 함께 갱신한다. control 호출만 감싼 lock이 아니다. |
| `runtime/channels/ZLinkChannelSocketRegistry.java:1064,1092` | 2 | [필요-lifecycle] | monitor/dealer detach·close와 reconnect를 exact transport 수명에 묶는다. |
| `runtime/host/ZLinkRelocationShutdownGate.java:12-29` | 4 | [필요-lifecycle] | relocation unit과 shutdown admission 전이를 지킨다. socket thread-safety와 무관하다. |
| `runtime/internal/backend/ZLinkBackendActorReceived.java:200`; `ZLinkBackendReceived.java:299` | 2 | [필요-프레임워크상태] | accepted journal bytes의 lazy exact-once materialization이다. |
| `runtime/internal/backend/ZLinkBackendStreamReceived.java:41` | 1 | [필요-lifecycle] | retained receive의 `closed`와 release action을 exact-once로 만든다. |
| `runtime/internal/channels/ZLinkClientServerRuntimeConfiguration.java:50`; `ZLinkFanoutRuntimeConfiguration.java:50` | 2 | [필요-프레임워크상태] | lifecycle 구현을 한 번만 설치한다. |
| `runtime/locations/ZLinkLocationAutoConnectHost.java:377,418` | 2 | [필요-프레임워크상태] | connect routing ID·probe·endpoint의 세 control 호출을 한 target 설정으로 묶는다. 개별 Core control 직렬화만으로는 호출 간 조합을 보장하지 않는다. |
| `runtime/spots/ZLinkRelocationPayloadTransfer.java:153,167,254,286` | 4 | [필요-프레임워크상태] | byte budget waiter와 relocation chunk ordinal/checksum assembly 상태다. socket multipart가 아니다. |
| `locations-redis/ZLinkRedisLocationConnection.java:75,107,152,172` | 4 | [필요-lifecycle] | Redis connection future의 생성·실패 제거·close 수명을 지킨다. |
| `spring/internal/runtime/ZLinkFrameworkLifecycle.java:150-233` | 6 | [필요-lifecycle] | runtime start/stop task와 callback terminal을 직렬화한다. |
| `http-client/ZLinkHttpRequestBuilder.java:241,254` | 2 | [필요-프레임워크상태] | shared client lease의 acquire/release count를 지킨다. |
| `stream-connector/DefaultZLinkStreamConnector.java:296`; `ZLinkStreamConnectionLifecycle.java:84-471` | 7 | [필요-lifecycle] | connect attempt, retry, cancellation과 close terminal을 묶는다. |
| `stream-connector/ZLinkTlsTransportConnection.java:126,174,186`; `ZLinkWebSocketTransportConnection.java:87,128,140` | 6 | [필요-lifecycle] | transport close, pending read/write와 terminal signal을 묶는다. Core socket lock이 아니다. |
| **합계** | **93** | **31 제거 / 5 receive / 22 상태 / 35 lifecycle** | |

### 2.2 C++ — 기존 표 230개

| 파일 · 취득 범위 | 수 | 분류 | 근거 |
|---|---:|---|---|
| `runtime/backend/raw_dealer_port.cpp:49,80,112,148,178` | 5 | 1 [필요-receive], 4 [필요-lifecycle] | send/request lock도 `_socket` null 검사와 close의 pointer detach를 함께 지킨다. receive는 poller·`_received`의 단일 소비자다. |
| `runtime/backend/raw_route_port.cpp:78,140,197,229,275,299` | 6 | 2 [필요-receive], 4 [필요-lifecycle] | poll/recv는 single-consumer이고 send/request/reply는 close가 reset하는 `_socket` pointer와 같은 gate를 쓴다. |
| `runtime/channels/channel_outbound_exchange.cpp:381-830` | 16 | `:381,479,571,636,652,721` 6 [필요-프레임워크상태]; `:411,419,499,506,561,600,686,695,796,830` 10 [필요-lifecycle] | readiness set/condition과 selected transport를 지키며, socket pointer·monitor·close 및 per-call timeout 설정/복구를 함께 묶는다. |
| 같은 파일의 기존 표 30과 현재 16의 차이 | 14 | [판정불가] | 현재 source에 취득 위치가 없어 경로를 확인할 수 없다. 해당 CP3 시점 source 또는 당시 line 목록이 필요하다. |
| `runtime/channels/channel_runtime_bundle.cpp:12-61` | 6 | [필요-프레임워크상태] | manual endpoint set, version, round-robin cursor다. |
| `runtime/channels/route_channel_runtime.cpp:215,309` | 2 | [판정불가] | lock 안에는 captured `std::function` backend 호출만 있다. 설치 가능한 backend의 동시 호출 계약이 type에 없으므로 Core send로 항상 귀결되는지 정적으로 확정할 수 없다. |
| `runtime/client_server/client_server_location_runtime.cpp:722-758,1052-1064,1653` | 7 | [필요-프레임워크상태] | descriptor publish task/epoch와 location connection state를 지킨다. |
| `runtime/fanout/raw_fanout_owner.cpp:52-464` | 16 | `:313` 1 [필요-receive]; `:87,94,157,227,431,445,457,464` 8 [필요-프레임워크상태]; `:52,72,121,164,219,271,289` 7 [필요-lifecycle] | receive cursor/connection registry/beacon deadline와 socket 생성·detach·close가 함께 있다. publish lock도 timeout option을 설치·복구하고 close와 경합한다. |
| `runtime/host/app.cpp:526,534,636,651,2680-3819` | 17 | `:636,651` 2 [필요-프레임워크상태]; `:526,534,2680,2720,2765,3121,3145,3228,3240,3520,3529,3587,3596,3807,3819` 15 [필요-lifecycle] | observer claim 2개와 relocation/termination operation·teardown gate다. Core socket send의 재직렬화가 아니다. |
| `runtime/http/http_listener.cpp:226,254,292,303,319` | 5 | [필요-프레임워크상태] | worker stop과 accepted socket owner registry를 지킨다. |
| `runtime/locations/location_runtime.hpp:336` | 1 | [필요-프레임워크상태] | heartbeat worker의 stop/wake condition이다. |
| `runtime/mesh/raw_mesh_node_owner.cpp:615-3952` | 49 | [필요-lifecycle] | lifecycle generation, active operation, socket ownership과 close/drain을 함께 지킨다. `_socket_mutex` 취득도 lifecycle mutex로 선정한 current socket을 보호한다. |
| `runtime/stateful/raw_stateful_dispatch.cpp:1051,1130,1162` | 3 | [필요-lifecycle] | active dispatch drain과 quiescence wait다. |
| `runtime/stateful/stateful_object_runtime.cpp:93` | 1 | [필요-lifecycle] | quiescence 전이와 wait다. |
| `runtime/stateful/stream_session_registry.cpp:436,1088` | 2 | [필요-lifecycle] | session registry change wait와 shutdown wake를 묶는다. |
| `runtime/streams/stream_host_service.cpp:211-3947` | 68 | `:211-349,671,681,1009-1240,1461,1477,1802-1937,3001-3181`의 취득 37개 [필요-프레임워크상태]; `:1503,1514,1589,1788,2223,2414-2964,3248-3947`의 취득 31개 [필요-lifecycle] | operation/error/readiness gate, core-session map, pending disconnect, write queue가 37개이고, socket/session owner detach·worker/io shutdown·close가 31개다. submit 위치 `:1589,1788,2223,3122`도 current owner/generation 또는 write queue terminal을 함께 지킨다. |
| `runtime/streams/stream_runtime.cpp:78-1749` | 12 | [필요-프레임워크상태] | dispatch executor/queue, serial log, transport-writer function과 captured writes를 지킨다. 직접 Core socket을 감싼 lock이 아니다. |
| **합계** | **230** | **4 receive / 84 상태 / 126 lifecycle / 16 판정불가** | 확인 가능한 214개에서 Core 단일 send/control만 감싼 취득은 0개다. |

### 2.3 .NET — 기존 snapshot source 48개

아래 수는 lock 전체가 아니라 기존 감사가 lock 해제 뒤 async 경계를 추적한 source 수다.

| 파일 · 기존 source | 수 | 분류 | 근거 |
|---|---:|---|---|
| `ZLinkSerialExecutionQueue.cs` | 10 | [필요-프레임워크상태] | queue admission, drain, dispose task와 worker ownership이다. |
| `ZLinkSpotSerialExecutor.cs` | 8 | [필요-프레임워크상태] | serial execution/barrier state다. |
| `ZLinkWorkerPool.cs` | 3 | [필요-프레임워크상태] | bounded worker queue와 stop state다. |
| `ZLinkMeshCompletionTable.cs` | 5 | [필요-프레임워크상태] | pending completion map과 exact claim이다. |
| `ZLinkCompletionDispatcher.cs` | 1 | [필요-프레임워크상태] | callback dispatcher ownership이다. |
| `ZLinkSerialWorkItem.cs` | 1 | [필요-프레임워크상태] | payload memoization/terminal이다. |
| `ZLinkManagedMeshNode.cs:2829,2837,2897,4425,5037,9024,9052,9188,9261,9420,11234` | 11 | [필요-lifecycle] | `_disposeTask`, `_socket`, `_activeSocketGeneration`, poller/monitor ownership을 함께 지킨다. send/request/reply는 지역 multipart submit이지만 lock의 사유는 current generation과 close detach다. |
| `ZLinkClientServerRuntimeService.cs` | 2 | [필요-프레임워크상태] | observer registration과 exact-identity removal이다. |
| `ZLinkActorHandoffState.cs` | 3 | [필요-프레임워크상태] | mutable handoff authorization state다. 기존 CP3의 stale snapshot 결함 판정은 이 재분류로 해소되지 않는다. socket/multipart 항목은 아니다. |
| `ZLinkEntrySpotDispatchPump.cs:273`; `ZLinkActorMessageFollower.cs:766` | 2 | [필요-lifecycle] | terminal retirement claim 뒤 queue dispose를 exact-once로 수행한다. |
| `ZLinkFrameworkRuntimeState.cs:172`; `ZLinkSpotNodeBundleRegistry.cs:17` | 2 | [필요-lifecycle] | disposal task를 한 번만 설치한다. |
| `ZLinkClientServerClientRuntime.cs:831,916,1502,1549` | 0 | 48개 분모 밖의 [필요-lifecycle] 4개 | `_socketLifecycleGate` 취득은 기존 snapshot 표에서 async 경계 0으로 기록됐다. disposed/physical generation과 disconnect→재connect 작업 프로토콜을 묶으므로 제거 가능하지 않다. |
| **합계** | **48** | **33 상태 / 15 lifecycle** | Core 단일 send/control만 감싼 snapshot source는 0개다. |

## 3. [결함-multipart누적] 상세

**0건이다.** 세 언어 모두 part 목록 또는 builder가 함수 지역 변수이고, 같은 공개 Framework
호출에서 terminal submit까지 진행한다. Core 계약이 금지하는 “한 논리적 multipart sequence를
여러 thread/호출로 나눔”은 발견하지 못했다. relocation의 chunk ordinal assembly
(`ZLinkRelocationPayloadTransfer.java:254-286`)는 여러 독립 wire message의 application payload를
재조립하는 상태이며 socket multipart send 누적이 아니다.

## 4. 제거 가능분의 예상 효과

제거 가능 31개는 모두 Java binding wrapper다.

- **hot path 7개:** Dealer send/request의 바깥·안쪽 취득 4개와 Router send/request/reply 3개다.
  message submit마다 Java monitor 진입을 피하므로 제거 효과가 가장 직접적이다. Dealer는 한 호출에
  중첩 monitor가 두 번 들어가므로 특히 불필요한 비용이 겹친다.
- **control path 24개:** Dealer 5, Router 14, Subscriber 5다. bind/connect/disconnect,
  option/query와 subscription은 저빈도라 처리량 효과는 작지만 불필요한 wrapper 직렬화와
  callback 재진입 제약을 줄인다.
- **lifecycle 제거 0개:** close/dispose, current socket/generation, poller/monitor, pending map이나
  handler/queue terminal을 함께 지키는 취득은 효과 계산 대상에서 제외했다.

이 수치는 source 취득 위치 제거 수이지 동적 contention 횟수나 성능 향상률이 아니다. 성능 수치는
이번 읽기 전용 감사 범위에서 측정하지 않았다.

## 5. 판정불가 항목과 확인 방법

1. `channel_outbound_exchange.cpp`의 14개는 기존 표와 현재 source의 차이다. CP3 표를 만든 정확한
   source snapshot과 acquisition line 목록을 제공한 뒤 각 임계 구역을 다시 읽어야 한다.
2. `route_channel_runtime.cpp:215,309`의 backend callback은 type에 동시 호출 계약이 없다. 모든
   production 설치 지점이 thread-safe Core send 한 호출로만 귀결된다는 계약 또는 closed/current
   state를 concrete backend가 자체적으로 지킨다는 증명이 있으면 [중복-제거가능]으로 바꿀 수 있다.
   임의의 사용자/내부 callback도 설치할 수 있다면 현재 직렬화가 필요하다.
3. .NET의 48을 실제 socket lock 수로 비교하려면 별도 모집단을 다시 만들어야 한다. 현재
   `_socketGate`, `_disposeGate`, `_socketLifecycleGate`의 실제 취득 위치를 source 단위로 세고,
   기존 async snapshot 48과 섞지 않아야 한다.

## 6. 수행/미수행 범위

- 수행: 지정한 Core guide/spec, 세 CP3 감사표, `rules.ko.md` 발견 7, Java 93개 대상 파일,
  C++ 표의 socket·dispose 열 대상 파일, .NET CP3 snapshot 파일과 명시된 socket/dispose gate의
  정적 source 읽기.
- 수행: send/control/receive/close 경로, 함께 보호하는 framework state, multipart builder의
  함수 밖 누적 여부 확인.
- 미수행: source·spec 수정, git 명령과 이력 조회, build/test/benchmark, runtime contention 측정.
- 생성 파일: 이 문서 하나뿐이다.
