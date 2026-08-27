# java L2 전환 순서 조사 (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다.

# Java L2 전환 순서 조사 — 2026-08-26

## 결론

`framework/languages/java/zlink-framework-core/src/main/java`의 Java 파일 757개를 정적으로 조사했다.

- 동기화 수단 보유 클래스: **94개**, 74파일
- 실제 monitor 취득 지점: **683곳**
  - `synchronized` 블록·메서드 683곳
  - `ReentrantLock` 0곳
  - `Semaphore` 0곳
- monitor 조건 대기:
  - `wait()` 0곳
  - `notify()` 0곳
  - `notifyAll()` 2곳
- 취득 지점 10개 이상인 클래스: **23개**
- 10개 미만이지만 교차 불변식·외부 콜백·공유 gate가 뚜렷한 추가 후보: **9개**
- 이미 전환된 `ZLinkSessionActorsRuntime`는 현재 0곳이며 목록과 전환 순서에서 제외했다.
- 상위 후보는 모두 클래스 단위로 **C2**다. 단일 map처럼 보이는 클래스도 monitor 안에서 lifecycle 결정을 내린 뒤 async/native/callback 동작을 시작하므로 C1이 아니다.
- `ZLinkAsyncSerialQueue`, `ZLinkApplicationJobQueue`, service mailbox 계열은 상태 형태는 C2지만 직렬화·admission primitive다. 일반 L2 state-owner 전환과 분리해야 한다.

계수는 `synchronized` 메서드 선언과 `synchronized (...)` source 위치를 각각 한 취득 지점으로 셌다. `ZLinkActorJoinPrewarmRegistry.java:33`의 Javadoc 한 건은 제외했다. 구체 타입 파급은 생성, 형식이 보존된 receiver 호출, class-qualified 참조를 센 근삿값이다. 인터페이스로 지워진 호출은 포함하지 않았다. CompletionStage 비율은 그 지점이 `CompletionStage` 또는 `CompletableFuture` 반환 메서드 안에 있는 비율이다.

판정 기준은 [design §4–5](/home/hep7/project/zlink/doc/plan/concurrency-redesign/design.ko.md:46), [state ownership spec §4–6](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:75), inline continuation은 진행 문서 §7-5·§7-6을 적용했다. POSDDD는 [POSDDD 성능 절](/home/hep7/project/zlink/doc/principal/dev/posddd.ko.md:717)의 할당·복사·경합 관찰만 기록했다.

Git 명령 금지 때문에 branch는 확인하지 않았으며 사용자 제공값 `refactor/lane-ownership-concurrency`를 기준으로 했다.

## 전수 목록

표기는 `동기화 수단/취득 지점 수`다. 별도 표기가 없으면 모두 intrinsic monitor의 `synchronized`다.

| 영역 | 동기화 수단 보유 클래스 |
|---|---|
| actors | `ZLinkActorDispatchSerials` sync/27; `ZLinkActorRuntime` sync/23; `ZLinkActorRuntime.ActorRegistry` sync/14; `ZLinkActorTransferHandoff` sync/12; `MessageFollowSource` sync/7; `ZLinkBoundActor` sync/1; `ZLinkDeferredActorJoinScope` sync/1; `Scope` sync/2 |
| binding | `ZLinkJavaDealerSocket` sync/13; `ZLinkJavaRawMeshNode` sync/6; `ZLinkJavaRawServicePort` sync/11; `ZLinkJavaRawSpotNode` sync/10; `ZLinkJavaRouterSocket` sync/19; `ZLinkJavaSocketReceivePoller` sync/3; `ZLinkJavaStreamSocket` sync/17; `ZLinkJavaSubscriberSocket` sync/7 |
| channels | `RuntimeEndpointConnections` sync/5; `ZLinkChannelReceiveLoops` sync/4 + notifyAll/1; `ZLinkChannelRouteDispatcher` sync/1; `ZLinkChannelSocketRegistry` sync/44; `ZLinkClientServerLocationRuntime` sync/24; `ZLinkFanoutLocationRuntime` sync/10; `ZLinkManualFanoutRuntime` sync/25; `ZLinkSpotRouteBridgeDrainer` sync/2 |
| configuration | `ZLinkCodecRegistration` sync/6; `ZLinkInboundDispatchRegistration` sync/15 |
| execution | `ZLinkAsyncSerialQueue` sync/26; `RetainedCommit` sync/6; `Entry` sync/1; `ZLinkSpotDispatchQueue` sync/7; `ZLinkWorkerPool` sync/2 |
| host | `ZLinkFrameworkRuntime` sync/3; `ZLinkRelocationShutdownGate` sync/4; `ZLinkRouteMeshRuntimeView.SignalHub` sync/3 |
| internal | `ZLinkBackendActorReceived` sync/1; `ZLinkBackendReceived` sync/1; `ZLinkBackendStreamReceived` sync/1; `ZLinkClientServerRuntimeConfiguration` sync/1; `ZLinkFanoutRuntimeConfiguration` sync/1; `ZLinkApplicationJobContext.QueuedOwnership` sync/2; `ZLinkApplicationJobQueue` sync/11; `ZLinkApplicationJobReceiveFlowController` sync/4; `Target` sync/4; `ZLinkMeshDrainCoordinator` sync/4; `Claim` sync/1; `ZLinkActorHandlerInstances` sync/3; `ZLinkHandlerInstanceOwner` sync/2; `ZLinkStatusPublisher` sync/3; `SnapshotSubscription` sync/3; `ZLinkCompositeRelocationBarrier` sync/9; `ZLinkRetainedSerialQueueCommit` sync/2; `ZLinkClassicFanoutLiveness` sync/7; `ZLinkInMemoryLocationAuthority` sync/4; `ZLinkServiceCompletionDispatcher` sync/6; `ZLinkServiceLivenessRegistry` sync/8; `ZLinkServiceMailbox` sync/6; `ZLinkServiceMailboxScheduler` sync/8; `ZLinkServiceOperationRegistry` sync/5; `ZLinkServiceTopologyRegistry` sync/13 |
| locations | `ZLinkInMemoryAuthorityStore` sync/21; `ZLinkInMemoryLocationStore` sync/18; `ZLinkInMemoryProviderLocationStore` sync/3; `RouteSocketExecutor` sync/2; `ZLinkLocationRuntime` sync/7; `ZLinkStatefulAuthorityRouteRuntime` sync/2 |
| mesh | `MeshNodeRegistration` sync/1 |
| spots | `ZLinkActorJoinPrewarmRegistry` sync/6; `ZLinkCanonicalRelocationStateMachine` sync/3; `RelayBatch` sync/4; `RelayBoundary` sync/3; `TargetAttempt` sync/4; `DefaultSpotContext` sync/4; `ZLinkInstanceSpotActivation` sync/7; `ZLinkRelocationPayloadTransfer.Budget` sync/2; `Assembler` sync/2; `ZLinkSpotPublisherRuntime` sync/2; `MulticastFuture` sync/5; `ZLinkSpotRelocationReplyRoutes` sync/18; `Registration` sync/2; `LazyRegistration` sync/3; `ZLinkSpotRetireControl.Target` sync/3; `ZLinkSpotTimerRegistry` sync/9; `ManagedTimer` sync/3; `ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource` sync/8; `ZLinkStandaloneActorRelocationStagingOwner` sync/7; `ZLinkUserSpotAggregateStagingOwner` sync/6; `ZLinkUserSpotRelocationBarrier` sync/13; `Seal` sync/5; `ZLinkUserSpotRetireSourceBuilder` sync/3; `PreparedSource` sync/10 |
| streams | `ZLinkStreamReceiveBuffer.RetainedOwner` sync/5; `ZLinkStreamRuntime` sync/14; `StreamReceiveLoop` sync/2 + notifyAll/1; `StreamReceiveState` sync/5 |

## 상위 후보 공통 파급

Kotlin production source에서 후보들의 구체 타입 참조는 모두 **0파일/0지점**이다. `ZLinkAsyncSerialQueue`만 `ZLinkStateLane.kt` KDoc에 한 번 등장한다. 따라서 public/interface 형식을 유지하면 직접 Kotlin 수정은 없다. Java 형식 변경이 Kotlin wrapper가 소비하는 interface까지 번질 때만 파급된다.

| 후보 | Java 구체 파급 | CompletionStage 계열 |
|---|---:|---:|
| `ZLinkAsyncSerialQueue` | 38파일/234지점 | 80/234, 약 34% |
| `ZLinkActorDispatchSerials` | 2/32 | 23/32, 약 72% |
| `ZLinkActorRuntime` | 21/186 | 93/186, 50% |
| `ActorRegistry` | 1/89 | 47/89, 약 53% |
| `ZLinkActorTransferHandoff` | 1/34 | 8/34, 약 24% |
| `ZLinkJavaDealerSocket` | 1/1 | 0% |
| `ZLinkJavaRawServicePort` | 1/105 | 28/105, 약 27% |
| `ZLinkJavaRawSpotNode` | 3/84 | 23/84, 약 27% |
| `ZLinkJavaRouterSocket` | 1/1 | 0% |
| `ZLinkJavaStreamSocket` | 2/5 | 0% |
| `ZLinkChannelSocketRegistry` | 5/89 | 11/89, 약 12% |
| `ZLinkClientServerLocationRuntime` | 2/11 | 0% |
| `ZLinkFanoutLocationRuntime` | 3/12 | 0% |
| `ZLinkManualFanoutRuntime` | 3/12 | 0% |
| `ZLinkInboundDispatchRegistration` | 2/8 | 0% |
| `ZLinkApplicationJobQueue` | 11/40 | 4/40, 10% |
| `ZLinkServiceTopologyRegistry` | 1/154 | 39/154, 약 25% |
| `ZLinkInMemoryAuthorityStore` | 1/20 | 16/20, 80% |
| `ZLinkInMemoryLocationStore` | 0/0 | 해당 없음 |
| `ZLinkSpotRelocationReplyRoutes` | 6/42 | 8/42, 약 19% |
| `ZLinkUserSpotRelocationBarrier` | 4/28 | 10/28, 약 36% |
| `UserSpotRetireSourceBuilder.PreparedSource` | 4/22 | 22/22, 100% |
| `ZLinkStreamRuntime` | 4/11 | 3/11, 약 27% |

## 상위 후보

### `ZLinkAsyncSerialQueue`

[ZLinkAsyncSerialQueue.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkAsyncSerialQueue.java:31)

- sync **26곳**. application/continuation/lifecycle pending queue, active turn, retained byte·message 수, relocation map과 quiescence waiter를 함께 보호한다. 직접 여러 컬렉션 접근은 1/26이지만 전체 aggregate는 **C2**다.
- 파급 38파일/234지점, CompletionStage 약 34%, Kotlin code 0.
- 재진입: enqueue의 monitor 구간에서 `startNext()`→`invoke()`→주입된 `executor.execute`로 이어진다. direct executor이면 operation이 monitor 안에서 실행되어 다시 queue에 진입할 수 있다.
- 유형 ③: helper를 펼치면 synchronized enqueue/start/abort 경로에서 caller executor와 operation supplier가 호출된다.
- 장기 작업: turn drain, continuation drain, relocation boundary·commit 처리.
- POSDDD: queue snapshot, entry·future 할당, relocation journal 복사가 보인다.
- 판정: **C2지만 lane primitive**다. 일반 state-owner 전환 대상에서 제외하고 primitive 자체 최적화로 분리한다.

### `ZLinkActorDispatchSerials`

[ZLinkActorDispatchSerials.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorDispatchSerials.java:21)

- sync **27곳**, 여러 컬렉션 직접 접근 **5/27**. Actor별 queue, active Actor, teardown, admission gate를 함께 전이하므로 **C2**다.
- 파급 2/32, CompletionStage 약 72%, Kotlin 0.
- 재진입: `this`와 Actor별 admission monitor가 중첩된다. `beginTeardown`에서 barrier completion을 붙인 뒤 다시 상태 monitor로 돌아오는 경로가 있다.
- 유형 ③: admission monitor 안의 `queue.enqueue*` 3계열, `beginTeardown`의 queue barrier 등록과 `whenComplete` 부착.
- 장기 작업: Actor별 queue/executor 생성, teardown barrier, 전체 quiescence aggregate.
- POSDDD: Actor별 queue/executor와 snapshot 할당, 네 상태 구조의 central monitor 경합.

### `ZLinkActorRuntime`

[ZLinkActorRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:103)

- 외부 클래스 sync **23곳**. registry, dispatch, transfer handoff, creation tail, session/authority/lifecycle 상태를 묶으므로 직접 다중 컬렉션 계수와 무관하게 **C2**다.
- 파급 21/186, CompletionStage 50%, Kotlin 0.
- 확정 monitor 재진입: `publishPreparedTransferredActor(prepared)`→동일 이름 overload([L1518](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:1518)).
- 유형 ③:
  - transferred Actor 게시 중 `spotNode.registerTransferredActor`, authority 기억, registry 호출
  - detach 중 `spotNode.closeActorBoundSession`
  - dispatch prepare/remove, location session-route 제거, context state 갱신
  - 해당 호출들은 L1518–1587, L3908–4068, L4435–4555의 synchronized 구간에 집중된다.
- `serializeActorCreation`의 `reservation.complete(null)`은 monitor 밖이지만 dependent가 inline 실행되어 component로 재진입할 수 있다. CURRENT lane scope를 남긴 채 complete하면 안 된다.
- 장기 작업: delayed retry, handoff drain retry, Actor creation chain, Actor별 dispatch/teardown·relocation.
- POSDDD: 거대 aggregate, snapshot·future·message 복사가 많다. 이번 전환에서 의미 없는 클래스 분해를 병행하면 안 된다.

### `ZLinkActorRuntime.ActorRegistry`

[ZLinkActorRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:4563)

- sync **14곳**, 여러 컬렉션 직접 접근 **3/14**. `byId`, `byActor`, entry flag와 handoff operation을 함께 보호하는 **C2**다.
- 파급 1/89, CompletionStage 약 53%, Kotlin 0.
- 확정 재진입: `actorTypeOrDefault()`→synchronized `actorType()`([L4594](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:4594)).
- 유형 ③과 자체 장기 작업은 확인되지 않았다.
- POSDDD: 두 index는 같은 aggregate이므로 분리 대상이 아니다. `List.copyOf`와 stream snapshot 비용이 있다.
- 외부 `ZLinkActorRuntime` monitor가 registry를 호출하므로 단독 전환 시 monitor→lane 순서가 생긴다. 외부 runtime과 같은 batch가 안전하다.

### `ZLinkActorTransferHandoff`

[ZLinkActorTransferHandoff.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorTransferHandoff.java:31)

- sync **12곳**, 여러 컬렉션 **5/12**. backlog, message-follow source, retirement, closed 상태가 하나의 **C2** aggregate다.
- 파급 1/34, CompletionStage 약 24%, Kotlin 0.
- monitor 메서드 중첩보다 외부 객체가 monitor 안에서 실행되는 재진입 위험이 크다.
- 유형 ③ 전수:
  - `fail`: `packet.fail`·`packet.close`
  - `retain`: 교체 항목 `expire`, scheduler `schedule`
  - `takeMessageFollowSource`: `expire`, future `cancel`
  - `close`: `shutdownNow`, retire callback, packet 실패 처리
- 장기 작업: retention scheduled task와 retirement executor.
- POSDDD: packet 정렬·snapshot과 retention별 scheduled task가 있다.

### `ZLinkJavaDealerSocket`

[ZLinkJavaDealerSocket.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:15)

- sync **13곳**. socket+poller lifecycle 및 transport capability를 보호하므로 컬렉션은 없어도 **C2**다.
- 파급 1/1, CompletionStage 0%, Kotlin 0. 실제 소비는 interface로 지워져 있다.
- 모든 send/request/receive/close 계열 synchronized 지점이 다시 `synchronized(socket)`를 잡고 native socket·poller를 호출한다. 이것이 유형 ③ 전수다.
- 동일 monitor 재호출은 확인되지 않았지만 binding callback 또는 synchronous completion이 들어오면 재진입 가능하다.
- 자체 background task는 없다. request/send가 native async 작업을 시작한다.
- POSDDD: 얇은 adapter 자체는 타당하지만 이중 monitor가 option과 I/O 전체를 직렬화한다.

### `ZLinkJavaRawServicePort`

[ZLinkJavaRawServicePort.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:33)

- sync **11곳**, 여러 컬렉션 **2/11**. router 목록, poller identity map, closed 상태의 **C2**다.
- 파급 1/105, CompletionStage 약 27%, Kotlin 0.
- 확정 재진입: `send` overload, `request` overload, `receive`→`receivePoller`→`ensureOwned`.
- 유형 ③: `open`, send/request/reply/receive 계열, poller 조회, `close`에서 binding Context·RouterSocket·poller 호출. 사실상 주요 11지점 전부다.
- 자체 background task는 없지만 request/send가 native async 작업을 시작한다.
- POSDDD: frame/list materialization과 router list+poller map의 동일 lifecycle 소유는 응집되어 있다.

### `ZLinkJavaRawSpotNode`

[ZLinkJavaRawSpotNode.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:59)

- sync **10곳**, 직접 여러 컬렉션 **4/10**, helper 포함 약 7/10. entry Spot, stream route·sequence·generation의 **C2**다.
- 파급 3/84, CompletionStage 약 27%, Kotlin 0.
- 같은 monitor 메서드의 확정 중첩은 없다.
- 유형 ③: entry-Spot 생성 구간에서 `createSpot`이 authority/owner supplier를 호출한다. stream route 전이는 대부분 내부 map/fence 작업이며 remote notification은 monitor 밖이다.
- 장기 작업: `delayedExecutor` 기반 재시도·만료가 최소 5개 논리 시작점이다.
- POSDDD: 다수 concurrent map과 monitor 전이를 혼합한다. route+sequence invariant는 map atomicity만으로 보존할 수 없으며 message copy가 많다.

### `ZLinkJavaRouterSocket`

[ZLinkJavaRouterSocket.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:16)

- sync **19곳**. socket+poller+option lifecycle의 **C2**다.
- 파급 1/1, CompletionStage 0%, Kotlin 0.
- 동일 monitor 재호출과 자체 장기 작업은 확인되지 않았다.
- 유형 ③: 19개 synchronized API 모두 binding/native socket 또는 options/poller를 monitor 안에서 호출한다.
- POSDDD: 단일 monitor가 option 변경과 모든 I/O를 직렬화한다. 동기 interface 유지가 필요하면 lane 전환 전 compatibility boundary 결정이 필요하다.

### `ZLinkJavaStreamSocket`

[ZLinkJavaStreamSocket.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:41)

- sync **17곳**. socket/raw mesh/callback/sink/binding map/session service/closed의 **C2**다.
- 파급 2/5, CompletionStage 0%, Kotlin 0.
- 유형 ③:
  - native socket/options와 handler 등록
  - `onTransportError`의 외부 callback 등록
  - `close`에서 binding snapshot, async unbind, raw node discard, caller 제공 `nativeClose.run()`
- 특히 `close`는 monitor 안에서 `CompletableFuture.allOf(...).get`으로 최대 500ms 대기한다. inline completion과 callback이 같은 socket으로 재진입할 수 있다.
- 장기 작업: close cleanup/unbind aggregate.
- POSDDD: monitor 안 blocking wait가 최우선 위험이다. binding마다 snapshot과 CF를 만든다.

### `ZLinkChannelSocketRegistry`

[ZLinkChannelSocketRegistry.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java:47)

- sync **44곳**, 여러 컬렉션 직접 접근 약 **15/44**. registration, kind별 socket, client/server connection·descriptor·selection, owned socket, peer, receive-flow를 보호하는 **C2**다.
- 파급 5/89, CompletionStage 약 12%, Kotlin 0.
- 확정 재진입:
  - `clientServerTransportReady(String)`→overload
  - terminated overload→overload
  - tick 경로→synchronized probe ID 할당
- 유형 ③:
  - `connection.transportLock` 안의 monitor/dealer close
  - 동일 lock 안의 dealer disconnect/connect
  - control 처리 및 listener endpoint 조회 중 RouterSocket 상태·option 접근
  - 일부 등록 detach는 이미 registry monitor 밖으로 반출되어 있다.
- 장기 작업: 자체 scheduler는 없지만 liveness tick, ack/probe send, connection lifecycle을 소유한다.
- POSDDD: Java 최대 lock owner다. selection snapshot, identity set, 다수 map이 central gate에서 결합된다.

### `ZLinkClientServerLocationRuntime`

[ZLinkClientServerLocationRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkClientServerLocationRuntime.java:45)

- sync **24곳**, 직접 여러 컬렉션 1/24. published/connection/lifecycle/tick 상태의 **C2**다.
- 파급 2/11, CompletionStage 0%, Kotlin 0.
- 확정 재진입: `stop`의 monitor 구간→synchronized `cancelScheduledTick()`.
- 유형 ③ 전수:
  - start 중 monitoring adapter 생성
  - tick 중 server weight 조회
  - connection monitor 안 add/register, `monitor.onEvent`, `dealer.connect`
  - remove 중 socket registry 제거
  - runtime monitor 안 scheduler `schedule`과 future `cancel`
- `monitor.onEvent`가 즉시 callback하면 connection/runtime monitor에 재진입할 수 있다. `settlement.complete`는 monitor 밖이다.
- 장기 작업: polling scheduler, infrastructure executor, connection admission request.
- POSDDD: desired/current snapshot과 connection object, 두 monitor의 획득 순서가 비용·교착 추론점이다.

### `ZLinkFanoutLocationRuntime`

[ZLinkFanoutLocationRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkFanoutLocationRuntime.java:53)

- sync **10곳**. concurrent published/connection/channel map과 lifecycle·tick completion을 함께 전이하므로 **C2**다.
- 파급 3/12, CompletionStage 0%, Kotlin 0.
- 유형 ③:
  - start/stop의 schedule-at-fixed-rate와 future cancel
  - connection monitor 안 `liveness.connect`, `monitor.onEvent`, subscriber connect
  - receive의 wait-for-readable/subscribe/liveness
  - expire와 connection close의 liveness/monitor/subscriber 호출
- monitor callback이 즉시 connection monitor로 돌아올 수 있다. settlement completion은 monitor 밖이다.
- 장기 작업: fixed-rate tick과 infrastructure executor.
- POSDDD: connection snapshot, message/frame 복사, concurrent map+monitor 혼합.

### `ZLinkManualFanoutRuntime`

[ZLinkManualFanoutRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkManualFanoutRuntime.java:36)

- sync **25곳**, 여러 컬렉션 **2/25**. desired/connection map, tick/running/cursor의 **C2**다.
- 파급 3/12, CompletionStage 0%, Kotlin 0.
- 유형 ③:
  - start의 fixed-rate scheduling
  - connection monitor 안 close/liveness/monitor callback/subscriber connect
  - receive, expire, remove·close의 binding 및 liveness 호출
  - close의 future cancel
- callback이 connection monitor로 재진입할 수 있다.
- 장기 작업: fixed-rate tick과 infrastructure executor.
- POSDDD: connection snapshot과 frame 복사, runtime/connection 이중 monitor 경합.

### `ZLinkInboundDispatchRegistration`

[ZLinkInboundDispatchRegistration.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/configuration/ZLinkInboundDispatchRegistration.java:13)

- sync **15곳**. profile/threshold/max 옵션과 freeze-on-first-build queue의 **C2**다.
- 파급 2/8, CompletionStage 0%, Kotlin 0.
- 재진입은 확인되지 않았다.
- 유형 ③: `applicationJobQueue(Executor)`가 monitor 안에서 caller executor를 검사하고 queue를 생성한다. 작업 실행은 하지 않는다.
- 장기 작업 없음.
- POSDDD: startup-only aggregate여서 runtime 경합은 낮다. public 동기 설정 표면을 유지해야 하므로 lane보다 immutable freeze/build 전환이 더 자연스러울 수 있다.

### `ZLinkApplicationJobQueue`

[ZLinkApplicationJobQueue.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobQueue.java:34)

- sync **11곳**. waiter deque, permit·pressure·byte/message counter와 close 상태의 **C2**다.
- 파급 11/40, CompletionStage 10%, Kotlin 0.
- 유형 ③: 주입된 `LongSupplier nanoTime`이 acquire/reset/evaluate/snapshot/release/cancel helper에서 monitor 안에 호출되고, close가 `receiveFlow.beginClose`를 호출한다.
- future complete/cancel은 monitor 밖으로 반출되어 있어 현재 구조는 inline continuation 재진입을 피한다.
- 장기 작업: `acquireBlocking` 대기는 monitor 밖이다.
- POSDDD: host-wide hot path, waiter별 future와 pressure snapshot 할당.
- 판정: **C2 admission primitive**. 일반 state-owner 전환에서 제외한다.

### `ZLinkServiceTopologyRegistry`

[ZLinkServiceTopologyRegistry.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceTopologyRegistry.java:19)

- sync **13곳**. peer map, selection map/cursor, local descriptor의 **C2**다.
- 파급 1/154, CompletionStage 약 25%, Kotlin 0.
- 확정 재진입: `admit` overload, channel/placement selection overload, helper의 synchronized `peers()` 재호출.
- 유형 ③: caller 제공 `Predicate<Peer>`를 synchronized channel selectable·placement 선택 메서드 안에서 평가한다. predicate가 registry를 호출하면 monitor 재진입에 의존한다.
- 장기 작업 없음.
- POSDDD: selection마다 sort/list/set snapshot을 만든다. public predicate를 monitor 안에서 실행하는 것이 가장 큰 설계 위험이다.

### `ZLinkInMemoryAuthorityStore`

[ZLinkInMemoryAuthorityStore.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkInMemoryAuthorityStore.java:28)

- sync **21곳**, 여러 컬렉션 **9/21**. row/reservation/terminal/aggregate/mutation/capacity counter와 revision/generation을 보호하는 **C2**다.
- 파급 1/20, CompletionStage 80%, Kotlin 0.
- 유형 ③: `Clock`, owner-lease predicate, descriptor lookup, Spot-claim predicate가 claim/read/prepare/commit/reject/abort/terminal/capacity 경로의 monitor 안에서 호출된다.
- callback은 같은 store 또는 공유 gate의 location store로 재진입할 수 있다. 이미 완료된 future는 외부에 반환되기 전이므로 caller continuation이 monitor 안에서 실행되지는 않는다.
- 자체 background task는 없지만 scan/sort/aggregate 계산이 긴 임계구역이다.
- POSDDD: global gate, 반복 sort/list/byte copy/digest와 여러 counter 갱신.

### `ZLinkInMemoryLocationStore`

[ZLinkInMemoryLocationStore.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkInMemoryLocationStore.java:77)

- sync **18곳**. lease, mesh row/generation, stamp, entry claim, fanout/client row의 **C2**다.
- 파급은 구체 타입 0/0이며 interface로 소비된다. Kotlin 0.
- `ZLinkInMemoryAuthorityStore`와 동일 `gate`를 공유한다.
- 확정 공유-monitor 재진입:
  - gate 안에서 synchronized `isExactOwnerLeaseLive`
  - mesh 갱신 중 authority `containsAuthority`
  - authority store의 callback이 다시 synchronized find/claim 검사
- 유형 ③: `Clock.instant`, authority component 호출, authority에서 주입받은 location callback 전부.
- 장기 작업: paging/sort/removeAll scan.
- POSDDD: provider와 authority 전체를 같은 gate가 직렬화한다. 두 클래스는 독립 전환하면 안 된다.

### `ZLinkSpotRelocationReplyRoutes`

[ZLinkSpotRelocationReplyRoutes.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRelocationReplyRoutes.java:22)

- sync **18곳**. route map 하나지만 route의 committed/fence/relay/delivered 상태와 async delivery 결정을 함께 보호하므로 **C2**다.
- 파급 6/42, CompletionStage 약 19%, Kotlin 0.
- 확정 재진입: committed/fence overload 3계열.
- 유형 ③: `register`가 monitor 안에서 `received.reply()`를 호출한다. 실제 stored delivery callback은 monitor 밖에서 실행한 뒤 exact route를 다시 확인하므로 올바른 ③ 회피 형태다.
- 자체 timer/장기 작업은 없고 만료는 호출 시 지연 정리한다.
- POSDDD: `Instant.now`, decode/copy/list materialization과 24시간 retained route map.

### `ZLinkUserSpotRelocationBarrier`

[ZLinkUserSpotRelocationBarrier.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkUserSpotRelocationBarrier.java:24)

- sync **13곳**. active/sealing/committing, composite barrier, timer, Actor lane inventory의 **C2**다.
- 파급 4/28, CompletionStage 약 36%, Kotlin 0.
- 직접 monitor 재호출은 확인되지 않았다. continuation은 대체로 monitor 밖에서 부착된다.
- 유형 ③:
  - seal-at-turn-boundary monitor 구간에서 Actor 목록과 relocation lane 조회
  - synchronized `freezeIngress`에서 composite barrier 호출
- admission predicate와 `beforeLaneResume` callback은 monitor 밖으로 반출되어 있다.
- 장기 작업: lane seal/capture/abort와 선택적 relocation-ready 대기.
- POSDDD: lane snapshot/map 복제 비용. 이미 turn A→callback 밖→turn B 형태를 상당 부분 따른다.

### `ZLinkUserSpotRetireSourceBuilder.PreparedSource`

[ZLinkUserSpotRetireSourceBuilder.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkUserSpotRetireSourceBuilder.java:1087)

- sync **10곳**. final journal, relocation commit, capture/source/terminal flag의 **C2**다.
- 파급 4/22, CompletionStage 100%, Kotlin 0.
- 확정 §7-5 위험: synchronized `abortPrecommit` 안에서 시작한 `exceptionallyCompose`/`thenCompose`가 이미 완료된 stage이면 inline 실행되고, continuation이 다시 `synchronized(PreparedSource.this)`를 획득한다. 현재 monitor는 허용하지만 non-reentrant lane은 거부한다.
- 유형 ③:
  - barrier commit 유지 및 expected relocation forward 설치
  - synchronized `completeSourceBarrierCommit`의 `relocationCommit.complete`
  - synchronized `abortPrecommit`의 session-route abort chain 시작
- 장기 작업: captured record 전체 relay, abort session-route chain, local cleanup.
- POSDDD: journal snapshot과 중첩 CF chain. 지점 수보다 재진입 위험 때문에 우선 조사 대상이다.

### `ZLinkStreamRuntime`

[ZLinkStreamRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java:80)

- 외부 클래스 sync **14곳**, 모두 `sessions` monitor. map은 하나지만 session lifecycle 결정 후 user handler·close·liveness 비동기를 시작하므로 **C2**다.
- 파급 4/11, CompletionStage 약 27%, Kotlin 0.
- 유형 ③ 확정: `getOrCreateSessionState`가 `sessions` monitor 안에서 `createSessionState`를 호출하며, 이것이 user session `sessionFactory.create`와 context 등록을 수행한다.
- 직접 monitor 재호출은 확인되지 않았다. future complete는 대체로 monitor 밖이다.
- 장기 작업: liveness schedule, reply-close schedule, receive-loop executor, handler executor.
- POSDDD: session snapshot, session별 queue/context 할당. user handler 생성이 global sessions monitor 안에 있는 것이 핵심 위험이다.

## 10개 미만 추가 후보

### `ZLinkActorTransferHandoff.MessageFollowSource`

- sync 7곳. suppression key, pending counter, 시작/만료 flag의 **C2**다.
- `beginMessageFollowNotice`와 expire 경로가 suppression component를 monitor 안에서 호출한다.
- 외부 `ZLinkActorTransferHandoff`와 같은 batch로 전환해야 한다.

### `ZLinkActorJoinPrewarmRegistry`

[ZLinkActorJoinPrewarmRegistry.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkActorJoinPrewarmRegistry.java:42)

- sync 6곳. relocation/object 두 index와 parked queue, installed sink의 **C2**다.
- 유형 ③: `parkOrDeliver`의 installed sink, `completeMigration`의 delivery consumer와 installed callback이 monitor 안에서 실행된다. parked failure callback도 정리 경로에서 실행된다.
- PREPARE install+drain의 “사이에 관찰 가능한 빈 구간이 없어야 한다”는 불변식 때문에 지점 수보다 우선순위가 높다.

### `ZLinkCompositeRelocationBarrier`

[ZLinkCompositeRelocationBarrier.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/relocation/ZLinkCompositeRelocationBarrier.java:24)

- sync 9곳. active seal, generation, committing과 여러 lane의 seal/cut을 함께 다루는 **C2**다.
- `trySeal`이 monitor 안에서 각 `ZLinkAsyncSerialQueue.trySealRelocation()`와 rollback을 호출한다.
- `runCapture`는 상태 확인 뒤 capture를 밖에서 실행하지만, complete/cut과 lane commit의 inline 동작을 계속 점검해야 한다.
- 여러 lane boundary 대기가 장기 작업이다.

### `ZLinkSpotTimerRegistry`

[ZLinkSpotTimerRegistry.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotTimerRegistry.java:31)

- 외부 sync 9곳, `ManagedTimer` 추가 sync 3곳. timer map, frozen/staged restore, scheduled future의 **C2**다.
- `add`, restore, resume, close가 monitor 안에서 timer start/close를 호출하며 start는 scheduler에 작업을 등록한다.
- timer dispatch 자체는 monitor 밖 queue에 enqueue하고 turn 전후 monitor를 다시 확인한다.
- future cancel과 scheduler schedule이 장기 작업 경계다.

### `ZLinkLocationRuntime`

[ZLinkLocationRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkLocationRuntime.java:26)

- sync 7곳. start/stop, heartbeat/retry task, startup completion, owner token 상태의 **C2**다.
- stop/close는 monitor 안에서 scheduled future를 cancel한다. initial-claim completion은 schedule을 monitor 안에서 수행한 뒤 `completion.complete`를 밖에서 실행한다.
- 장기 작업: owner-lease initial retry, fixed-delay heartbeat, recovery listener chain.

### `ZLinkInstanceSpotActivation`

[ZLinkInstanceSpotActivation.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkInstanceSpotActivation.java:24)

- sync 7곳이 `this`와 `idleLock`에 나뉜다. close future/start flag와 idle task/fence 상태의 **C2**다.
- `closeWithReason`에서 `this`→`idleLock` 순으로 중첩하며 scheduled future를 cancel한다.
- close completion은 monitor 밖에서 `result.complete`하므로 현재 inline continuation 처리는 안전하다.
- 장기 작업: idle timer, authority seal, user `onClosing`, resource cleanup.

### `ZLinkServiceMailboxScheduler`

[ZLinkServiceMailboxScheduler.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceMailboxScheduler.java:19)

- sync 8곳. mailbox map, ready set, claim·pending bytes의 **C2**다.
- `consumer.accept(work)`는 monitor 밖에서 실행하고 finally에서 claim을 반환한다. 유형 ③을 이미 피한다.
- 직렬화 primitive이므로 일반 state-owner 목록과 분리한다.

### `ZLinkServiceMailbox`

[ZLinkServiceMailbox.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceMailbox.java:18)

- sync 6곳. 두 domain의 owner queue/index/counter/claim, closed 상태의 **C2**다.
- 외부 callback과 async 작업은 없다.
- mailbox primitive이므로 lane 확산보다 hot-path 계수·할당 최적화 대상으로 분리한다.

### `ZLinkServiceLivenessRegistry`

[ZLinkServiceLivenessRegistry.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceLivenessRegistry.java:16)

- sync 8곳. peer map, per-peer ready/deadline/outstanding probe, probe ID의 **C2**다.
- 외부 callback·async 시작·확정 재진입은 없다.
- `tick`의 probe/timed-out list 할당만 monitor 안에서 수행한다. 작은 초기 lane 표본 후보다.

## 제안 전환 순서

배수는 이미 전환된 `ZLinkSessionActorsRuntime`의 기존 규모, 즉 sync 22곳, 3파일/18 파급 지점, CompletionStage 약 61%, 장기 작업 6개를 **1.0배**로 둔 상대 난이도다. 구현 일정이 아니라 정적 복잡도 추정이다.

| 순서 | 전환 단위 | 표본 대비 | 근거 |
|---:|---|---:|---|
| 1 | `ZLinkServiceLivenessRegistry` | 0.3배 | callback·async·재진입 없는 작은 C2 |
| 2 | `ZLinkInboundDispatchRegistration` | 0.4배 | startup-only; freeze/build 경계 검증 |
| 3 | `ZLinkSpotRelocationReplyRoutes` | 0.6배 | callback 반출 패턴이 이미 존재 |
| 4 | `ZLinkActorJoinPrewarmRegistry` | 0.7배 | 작은 크기지만 명확한 유형 ③ 분리 표본 |
| 5 | `ZLinkInstanceSpotActivation` | 0.7배 | 두 monitor와 timer cancellation 연습 |
| 6 | `ZLinkServiceTopologyRegistry` | 0.8배 | predicate를 turn 밖으로 반출해야 함 |
| 7 | `ZLinkInMemoryLocationStore` + `ZLinkInMemoryAuthorityStore` | 1.0배 | 공유 gate 때문에 반드시 한 batch |
| 8 | `ZLinkJavaRawServicePort` | 1.0배 | 확정 overload 재진입과 native 호출 분리 |
| 9 | `ZLinkActorTransferHandoff` + `MessageFollowSource` | 1.1배 | callback·scheduler·packet terminal 분리 |
| 10 | `ZLinkCompositeRelocationBarrier` | 1.1배 | 여러 기존 lane의 seal/rollback coordination |
| 11 | `ZLinkUserSpotRelocationBarrier` | 1.1배 | 기존 turn A/B 구조를 lane으로 정식화 |
| 12 | `UserSpotRetireSourceBuilder.PreparedSource` | 1.3배 | §7-5 inline continuation 확정 위험을 조기에 해소 |
| 13 | `ZLinkSpotTimerRegistry` | 1.4배 | timer callback과 freeze/restore lifecycle |
| 14 | `ZLinkLocationRuntime` | 1.4배 | heartbeat/retry/owner recovery 장기 작업 |
| 15 | `ZLinkJavaRawSpotNode` | 1.5배 | cross-map route/sequence와 다수 지연 작업 |
| 16 | `ZLinkFanoutLocationRuntime` | 1.4배 | connection callback·tick·liveness |
| 17 | `ZLinkManualFanoutRuntime` | 1.6배 | 25지점과 runtime/connection 이중 monitor |
| 18 | `ZLinkClientServerLocationRuntime` | 1.9배 | monitor callback, scheduler, 두 lock ordering |
| 19 | `ZLinkActorDispatchSerials` | 2.0배 | Actor별 admission monitor와 queue 결합 |
| 20 | `ZLinkStreamRuntime` | 2.1배 | user handler 생성이 global monitor 안에 있음 |
| 21 | `ZLinkJavaStreamSocket` | 2.2배 | monitor 안 500ms blocking close |
| 22 | `ZLinkActorRuntime` + `ActorRegistry` | 3.0배 | 35+7개 상태 경로, 275 concrete 파급 지점, 확정 재진입 |
| 23 | `ZLinkChannelSocketRegistry` | 3.5배 | 44지점, 가장 넓은 channel selection/transport aggregate |

다음 항목은 위 순서와 별도 결정이 필요하다.

- `ZLinkJavaDealerSocket`, `ZLinkJavaRouterSocket`: 동기 backend interface 위에서 non-thread-safe native socket을 직렬화하는 integration lock이다. lane으로 바꾸기 전에 동기 호출의 compatibility boundary를 정해야 한다. 단순 mutex→lane 치환 대상이 아니다.
- `ZLinkAsyncSerialQueue`, `ZLinkApplicationJobQueue`, `ZLinkServiceMailbox`, `ZLinkServiceMailboxScheduler`, `ZLinkSpotDispatchQueue`: lane/admission/mailbox primitive 자체다. 상태 소유자 L2 전환이 수렴한 뒤 별도 primitive 감사 대상으로 둔다.
- 나머지 10개 미만 클래스는 상위 소유자의 nested/helper인 경우 그 소유자와 같은 batch로 처리하고, 독립 leaf는 위 23개가 수렴한 뒤 크기순으로 처리하는 편이 안전하다.

## 검증 및 작업 상태

- 저장소 파일 변경: **0**
- build/test: **실행하지 않음**
- Git 명령: **실행하지 않음**
- 저장소 외부 전수 계수용 임시 `/tmp` 파일 하나를 만들었다가 즉시 삭제했으며, 잔존 파일은 없다.
- 조사 시작: 2026-08-26 16:55:14 KST
- 조사 종료: 2026-08-26 17:13:19 KST
- 걸린 시간: **18분 5초**
