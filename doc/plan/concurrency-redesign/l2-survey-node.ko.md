# node L2 전환 순서 조사 (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다.

# Node L2 전환 순서 조사 (2026-08-26)

## 결론

사용자가 제공한 브랜치 `refactor/lane-ownership-concurrency`를 기준으로 `framework/languages/node/packages/framework/src`의 TypeScript 소스 **373개**를 정적으로 조사했다. Git 명령 금지 때문에 실제 브랜치는 별도로 확인하지 않았다.

Node에서는 lock 개수가 아니라 다음을 후보 기준으로 삼았다.

- 여러 컬렉션·필드 또는 map entry 상태가 하나의 불변 조건을 이룬다.
- 상태를 읽은 뒤 `await`/Promise continuation/timer를 지나 다시 상태를 쓴다.
- 주입 callback이나 같은 클래스의 public 메서드 호출로 동기 재진입할 수 있다.
- `await` 뒤 현재 map entry·token을 재검사하더라도, 그 재검사가 여러 상태 전체를 보호하는지는 별도로 판정한다.

식별 결과는 다음과 같다.

- **C2 전환 단위: 43개**
- **C1, 전환 불요: 3개**
- 별도 실행·admission·queue primitive는 L2 state-owner 목록에서 제외
- 이미 전환한 `ZLinkActorSessionBindingRegistry`와 L1에서 async가 전파된 runtime 범위는 제외

호출 파급은 구체 타입 이름을 정적으로 확인할 수 있는 외부 파일/참조 지점이다. interface나 runtime 형식으로 지워진 호출은 포함하지 않았다.

## C1 — 전환 불요

| 후보 | 근거 | 파급·POSDDD |
|---|---|---|
| [`ZLinkSpotActorHandlerRegistryRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/spot-actor-dispatch.ts:39) | `_packets` 단일 map뿐이다. `addHandler()`→`addPacket()` 재호출은 L51에 있지만 한 JS turn에서 단일 map 연산으로 끝난다. | **7파일/16지점**. snapshot 한 번 외에 교차 불변식·장기 작업 없음. **C1**. |
| [`ZLinkReceiveTaskTracker`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/channels/channel-receive-task-tracker.ts:9) | 단일 task set에 exact Promise를 추가하고 completion에서 같은 Promise만 제거한다. `waitForAll()`은 snapshot을 기다릴 뿐 이후 state write가 없다. | **1파일/7지점**. task별 continuation 할당은 있으나 state lane 전환 근거는 아니다. **C1**. |
| [`ZLinkChannelOutboundOperations`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/channels/channel-outbound-operations.ts:42) | `_pendingRequests`는 channel별 독립 계수다. `measureRequest()`가 await 뒤 L537–542에서 현재 count를 **다시 읽어** 감소시키며, 요청 결과를 결정하는 다른 상태와 결합하지 않는다. | **1파일/3지점**. hot path map 갱신은 측정 대상이나 lane으로 모든 요청을 직렬화하면 손해가 더 크다. **C1**. |

## C2 후보 전수 목록

### Actors·Streams

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`DefaultZLinkActorManager`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/index.ts:153) + [`ZLinkTransferredActorRollbackCoordinator`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/transferred-actor-rollback.ts:22) | actor state/mesh-name/relocation-stage map과 rollback task map. `destroy()`가 state를 읽고 await 뒤 map을 제거한다(L218–257). relocation 준비·제거도 L456–546에서 같은 세 map을 다시 쓴다. rollback은 injected `states` map을 읽고 native destroy/retry 뒤 삭제한다(L29–91). `submitCreate()`→public `find()` 재진입 L352. | **11/27 + 1/3**. creation completion, rollback retry와 5ms waiter가 장기 작업이다. `states.values()` snapshot(L623–627)과 actor ID를 두 map에 중복 보관하는 비용이 있다. **C2**. 같은 batch 권장. |
| [`ZLinkActorHandoffCoordinator`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:231) | active handoff, follow-route, stale generation/ref, reply-route와 per-entry phase/pending queue. `messageFollowRelayed()`는 suppression 상태를 잡고 await 후 갱신한다(L1117–1229). public 재호출 L718, L746. | **6/14**. follow-route 만료 L500, reply-route 만료 L903, per-entry Promise tail L1117. pending `slice()`와 route snapshot이 반복된다. **C2**. |
| [`ActorJoinRegistrationScope`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-join-deferred-scope.ts:17) | intents, totalBytes, open/prepared. `discard()`가 intents를 읽어 `Promise.allSettled` 후 배열을 비운다(L84–87). `activateSealedJoins()`→`prepareSealedJoins()` 재진입 L77. | **1/2**. deferred join 전체 prepare/execute/discard가 장기 작업이다. operation-scoped지만 교차 상태이므로 **C2**. |
| [`ZLinkPostCommitActorBinder`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/post-commit-actor-binder.ts:10) + [`ZLinkPostCommitActorLocation`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/post-commit-actor-location.ts:17) | desired-ref/task map과 actor별 operation queue/task map. binder는 ref를 읽고 bind를 await한 뒤 exact ref를 재검사해 삭제한다(L30–45). location은 queue head를 잡고 operation을 await한 뒤 같은 queue entry를 완료한다(L73–102). | **3/7 + 2/4**. 둘 다 retry loop가 장기 작업이다. location queue는 head 증가 후 주기적 `splice()`로 이동 복사한다(L106–116). **C2**, 같은 패턴으로 함께 전환 가능. |
| [`ZLinkManagedStream`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/managed-stream.ts:52) | local/remote address, transportClosed, nativeActorBindings. bind/unbind가 transport 상태를 읽고 await 뒤 binding map을 갱신한다(L184–205, L273–286). `close()`→`markTransportClosed()`, `closeForDrain()`→`closeForReason()` 등 재진입 4곳. | **5/18**. bind/unbind 및 close deadline(L495)이 장기 경계다. mutable native binding 참조가 lane 밖으로 나가는 지점을 같이 점검해야 한다. **C2**. |
| [`ZLinkStreamSessionRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/stream-session-runtime.ts:171) + [`ZLinkStreamSessionNodeRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/stream-session-runtime.ts:883) | session 연결·disconnect·close·liveness field와 node의 session/receive/ready/metadata queue·cursor. liveness는 상태를 읽고 ping을 await한 뒤 `_awaitingPongSince`를 쓴다(L628–660). node dispose는 receive loop를 기다린 뒤 여러 map/queue를 비운다(L948–981). | public wrapper 포함 **2파일/18지점**. receive loop, liveness, disconnect drain이 장기 작업이다. metadata/unaddressed 배열의 head-compaction `splice()` L1377/L1404가 있다. **C2**. |

### Backend·Channel

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`ZLinkMeshCompletionTable`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/backend/mesh-completion-table.ts:35) | pending completion map과 disposed flag. `submit()`의 caller `operation()`이 **동기적으로 `dispose()`에 재진입할 수 있음**이 L67–80에 명시돼 있다. callback 뒤 disposed를 재검사하지만 pending 등록 전체가 한 불변식이다. | **13/40**로 작은 클래스 중 파급이 가장 넓다. abort listener와 pending Promise가 장기 작업. dispose snapshot L121, reply `Buffer.from` 복사 L139 이후가 POSDDD 관찰점. **C2**. |
| [`ZLinkChannelRuntimeLifecycle`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/channels/channel-runtime-lifecycle.ts:86) | channel/subscriber/route receive loop, manual subscriber owner, dispatcher, location runtime, disposed. start가 location runtime을 await한 뒤 field를 설치한다(L124–151); subscriber open/disconnect도 await 전후 collection을 바꾼다(L530–590). public `start()`→`prepareMeshDispatch()` L172. | **1/3**. 여러 receive loop와 auto-connect가 장기 작업. dispose 때 여러 set/map snapshot(L302–322), transition Promise chain과 array `splice()` L552/L680/L741. **C2**. receive-loop 클래스 자체는 아래 primitive 제외에 둔다. |
| [`ZLinkChannelSocketRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:115) | 18개 map/set와 ready identity, connection, monitor, liveness timer/probe ID. configured admission이 connection을 읽고 await한 뒤 같은 map을 갱신한다(L1218–1245). public 표면 재호출 **17곳**: 대표 L613, L653, L1204, L1243, L1347, L1357. | **7/16**. ready polling L658, liveness interval L1356, monitor callback/connection admission이 장기 작업. snapshot·set materialization이 매우 많다(L170–209, L877–901, L1177). **C2**. |
| [`ZLinkClientServerLocationRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/channels/client-server-location-runtime.ts:39) | local descriptor/connection map, controller, timer. publish/reclaim이 descriptor snapshot을 읽고 store await 뒤 map을 갱신한다(L98–165, L369–388). `schedule()`→public `tick()` L409. | **2/4**. recurring timer L407과 connection close/drain. connection snapshot·정렬 L88/L210/L346. **C2**. |
| [`ZLinkFanoutLocationRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/channels/fanout-location-runtime.ts:32) | local descriptor, publisher identity, connection map, controller/timer. publish/reclaim/remove가 store await 뒤 map을 갱신한다(L103–183, L352–364). `schedule()`→`tick()` L382. | **1/3**. recurring timer L380과 subscriber lifecycle. connection snapshot L87/L237. callback `onSubscriberOpened`도 재진입 가능하다. **C2**. |

### Service foundation

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`ServiceDiscoveryRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-discovery-registry.ts:33) + [`ServiceTopologyRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-topology-registry.ts:74) + [`SmoothWeightedSelection`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-weighted-selection.ts:32) | descriptor/index map과 selection cursor/plan. discovery의 public `selectClientServer()`→`selectClientServerConnection()` L82–84. topology의 public `instanceSpotPlacementTypes()`→`peers()` L397. selection은 `candidateProvider`와 caller `accept` callback을 plan/cursor 갱신 도중 호출한다(L43–67). | **1/4 + 1/3 + 2/6**. 자체 timer 없음. 선택마다 list/sort/spread를 만들고 topology는 최대 4,096 step/10ms cycle 계산을 동기 실행한다(L72–105). callback 반출이 필요하므로 **C2**, 세 클래스 같은 batch 권장. |
| [`OperationRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:52) | entries, nextId, generation, closed. public `cancel()`→`fail()` L110. open은 entry를 설치하고 injected timer callback이 exact entry를 종료한다(L75–91). | **3/6**. operation별 timer/Promise가 장기 작업이며 close snapshot L121. **C2**. |
| [`ServiceMaintenanceRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-maintenance-runtime.ts:27) | units/observers, activeOutbound, state/kind/error/operation. `runUnits()`가 unit set을 읽고 relocations를 await하면서 set과 outbound count를 다시 갱신한다(L127–160). public `observe/run/publish`→`snapshot()` 재진입 5곳. | **1/2**. deadline timer L87과 relocation task set. 완료 unit 제거의 `splice()` L143. **C2**. |
| [`ServiceStatefulRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-registry.ts:96) | Spot/Actor/type/binding/reservation/generation map과 per-owner Promise tail. `runTurn()`은 기존 tail을 읽고 Promise를 기다린 뒤 같은 `turns` map을 제거한다(L514–529). public 재호출 **16곳**. | **1/3**. owner별 Promise tail이 장기 작업. domain state와 execution-tail primitive가 같은 aggregate에 있어 POSDDD의 책임 혼합 관찰점이다. **C2**. |
| [`RawServiceMeshRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:149) | topology/mailbox/liveness/operation과 peer expectation, candidate, connection ID, monitor state/event/buffer. monitor drain이 여러 map을 읽고 await 뒤 같은 map/array를 갱신한다(L1020–1113). public 재호출 **13곳**. | **2/11**. receive pump, liveness tick, monitor drain, request terminal이 장기 작업. `shift`/`splice`와 peer-selection snapshot L254/L1112/L1546. **C2**. |

### Host·relocation

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`ZLinkActorTransferRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/actor-transfer-runtime.ts:210) | source departure/core leave/deferred terminal map. prepare/notify가 map entry를 잡고 authority·delivery를 await한 뒤 map을 갱신한다(L340–366, L570–654, L870–884). public 재호출 L473, L1583. | **4/10**. 72개 await, retry timers L412/L1532, departure task continuation L1136. **C2**. |
| [`ZLinkFrameworkRuntimeHost`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/index.ts:239) | lifecycle/start/stop/relocation operation, 모든 runtime component, observer/metric/error-handler set. `startCore()`와 owner-recovery 설치가 await 전후 여러 runtime field를 바꾼다(L1280–1355, L1470–1497). public start/stop 재진입 L1029/L1043/L1664/L1670. | 구체 타입 **1/4**지만 내부 사용면이 매우 넓다. 69개 await, shutdown/relocation/owner recovery 장기 작업. registration snapshot과 component reference 복사가 집중된다. **C2**. 최종 배치 권장. |
| [`ZLinkRouteMeshRuntimeCoordinator`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:87) | mesh state, placement/peer/store-health fingerprint, host/shutdown operation, retire-prepared. retire/relocate/drain이 state snapshot을 읽고 await 뒤 flag를 갱신한다(L354–502). public observer methods→`snapshot()` 재진입 8곳. | **1/4**. timer 8개와 placement interval L668. state snapshot L415/L491/L545, descriptor sort L748+. **C2**. |
| [`ZLinkHostServiceRelocationRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts:355) 및 중첩 `LocalTargetPort` | target stage/terminal/authority/prepare/reply/session/source/cutover/payload-budget 등 16개 map/set. coordinator와 prepare control이 state를 읽고 다수 await 뒤 exact stage/map을 갱신한다(L2040–2201, L2440–2538). `LocalTargetPort`도 deferred-join root map을 relay await 뒤 갱신한다(L4142, L4350). | **2/4**. 154개 await, timer 10개, stage Promise tail, retry/follow/deadline 작업. frame 배열 `slice`/`splice`, map snapshot/sort가 많다. **C2**, 가장 큰 후보. |
| [`ZLinkStatefulAuthorityRouteRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/stateful-authority-route-runtime.ts:119) | abort controller, appliedByMesh, loop Promise. start/stop/reconcile가 loop·route map을 await 전후 갱신한다(L128–239); `start/run`→`reconcile()` 재진입. | **1/5**. 지속 reconcile loop와 retry timer L656. route snapshot L277. **C2**. |
| [`ZLinkUserSpotCreationCoordinator`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/user-spot-creation-coordinator.ts:84) | local/remote creation map과 entry phase/task. handler가 map에 operation을 설치한 뒤 async 결과의 `finally`에서 exact entry를 제거한다(L420–465). public close 경로 재호출 L338/L369/L727. | **2/5**. remote/local admission, deadline/retry L1122/L1147가 장기 작업. **C2**. |
| [`ZLinkInstanceActivationAuthority`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/instance-activation-authority.ts:73) | pending reservation map과 per-entry reservation/phase/terminal. relocation blob put/read, store reserve, closing wait를 연속 await한 뒤 reservation을 설치·갱신한다(L90–215 이후). | **1/2**. 25개 await. blob hash/CRC/Buffer 복사와 여러 store round-trip이 POSDDD 핵심 관찰점이다. `owner`/`onReady` callback도 재진입 가능. **C2**. |
| [`ZLinkLocationRuntimeOwner`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/location-runtime-owner.ts:34) | provider/store/runtime/lifecycle/events/lease tracker/invalidation handler. `startForRuntime()`이 public `ensureRuntime()`와 `ownerNodeRid()`를 재호출한다(L200–204). | **1/3**. location runtime start/stop이 장기 작업. provider/runtime/lifecycle의 pass-through가 아닌 한 aggregate 경계인지 확인해야 한다. **C2**. |

### Location

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`ZLinkAutoConnectLoop`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/auto-connect-loop.ts:20) + [`ZLinkAutoConnectReconciler`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/auto-connect-reconciler.ts:39) | loop controller/timer/stamp/version/failure flag와 reconciler의 active/pending-disconnect/failure/desired/local publication. tick이 store/lease/reconcile를 await한 뒤 stamp와 여러 map/flag를 갱신한다(L68–103, reconciler L90–223). public 재호출 4+2곳. | **1/3 + 2/5**. recurring timer/peer connect-disconnect가 장기 작업. desired/active snapshot과 set 재생성(L87, L166–172, L242). **C2**, 같이 전환. |
| [`ZLinkSpotNodeAutoConnectExecutor`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-node-autoconnect.ts:153) | connection-intent map과 backend peer state. map miss를 확인하고 `connectPeer()`를 await한 뒤 intent를 설치한다(L224–249); 그 사이 disconnect가 끼어들 수 있다. | private class, enclosing 파일 **1지점**. backend connect와 disconnect callback이 장기/재진입 경계. **C2**, auto-connect batch에 포함. |
| [`ZLinkInMemoryLocationStore`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/in-memory-location-store.ts:76) + [`ZLinkInMemoryAuthorityStore`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/in-memory-authority-store.ts:82) | location의 lease/row/stamp/claim/transfer와 authority의 row/terminal/aggregate/capacity/generation. `removeAllByOwner()`가 authority를 await한 뒤 mesh map을 갱신한다(L900–930). authority validation callback은 location map을 즉시 읽는다(location L94–128). | **1/2 + 1/3**. 반드시 한 batch: 분리 lane이면 authority→location callback이 재진입한다. paging마다 전체 sort 후 `slice()`(authority L215–218, location L1388–1412), payload 방어 복사가 많다. **C2**. |
| [`ZLinkInMemoryProviderLocationStore`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/in-memory-provider-location-store.ts:27) | values/scans map, version/scan counter. write 조건과 mutation 전체, scan snapshot/cursor가 하나의 불변식이다. injected `now()`가 mutation 중 public surface로 재진입할 수 있다. | **1/2**. 자체 background task 없음. scan마다 전체 map filter/sort와 byte `slice()` 복사(L78–131). **C2**. |
| [`ZLinkActorLocationClaims`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/actor-location-claims.ts:41), [`ZLinkActorSessionRouteClaims`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/actor-session-route-claims.ts:17), [`ZLinkSpotLocationClaims`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/spot-location-claims.ts:31), [`ZLinkLocationLifecycle`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/lifecycle.ts:37) | claim map entry를 잡고 store write/remove를 await한 뒤 exact entry를 설치·제거한다(actor L100–110/L240–290, session L28–35, Spot L55–123). lifecycle은 claim aggregate와 actorCleanupTasks/disposed를 묶는다(L150–240). | 각 claims **1/3**, lifecycle **9/21**. retry release L169/L271과 ownership-lost callback이 장기/재진입 경계. row object spread와 cleanup task map 중복이 있다. **C2**, 네 클래스 같은 batch 권장. |
| [`ZLinkOwnerLeaseTracker`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/lease-tracker.ts:22) | owner snapshot, in-flight refresh, fingerprint/version. snapshot miss를 읽고 store refresh를 await한 뒤 exact Promise/map을 갱신한다(L80–110). | **7/21**. refresh Promise가 장기 작업. owner key snapshot·sort L65–71. **C2**. |
| [`ZLinkStoreLocationResolvers`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:162) + [`ZLinkAuthoritySpotRouteResolver`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:870) | actor/direct-actor/Spot route cache와 placement cursor; authority cache/epoch/active-resolution map. resolver는 cached route를 읽고 store/authority를 await한 뒤 cache를 다시 쓴다(L393–425, L508–517, L577–613, L902–942). public resolver 재호출 11곳. | **8/24 + 2/4**. concurrent resolution Promise가 장기 작업. 같은 key의 cache·epoch·active resolution을 한 단위로 유지해야 한다. **C2**. |
| [`ZLinkLocationRuntime`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/locations/runtime.ts:135) | owner token/lease health/deadline, startup/cleanup, heartbeat, handlers, mesh/lease scope. start/renew/claim이 store await 뒤 token과 health field를 갱신한다(L299–465). `stop()`→`cleanupOwner()` L364, heartbeat→`renewOwnerLeaseOnce()` L1028. | **10/32**. 53개 await, heartbeat와 recovery listener가 장기 작업. global lifecycle state와 per-mesh operations가 같은 owner에 있어 lane HoL 측정이 필요하다. **C2**. |

### Spots

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`DefaultZLinkSpotManager`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/index.ts:308) | activation/factory/materialization/gate/close/terminal/deferred-release map과 idle sweep. materialization·close·dispatch가 상태를 읽고 await 뒤 여러 map을 갱신한다(L560–659, L819–963, L1104–1185, L1884 이후). public 재호출 3곳. | **11/31**. 117개 await, idle timer L905, close/materialization continuation. pending map 이중 index와 Promise/timer 할당이 많다. **C2**. |
| [`ZLinkSpotActivationRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-activation-registry.ts:36) | activation/staged/pending/closing/failed/waiter map과 scan cursor. `startClose()`가 activation을 읽고 caller close를 실행한 뒤 `finally`에서 여러 map을 갱신한다(L219–268). `getOrBegin()`도 async creation 뒤 pending/activation을 갱신한다(L277–324). | **1/3**. close/create Promise와 empty waiter가 장기 작업. activation snapshot·정렬과 operation object 할당. **C2**. |
| [`ZLinkFormalRemoteActorAdmissionRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/formal-remote-actor-admission-registry.ts:108) + [`ZLinkFormalRemoteActorTransferRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/formal-remote-actor-transfer-registry.ts:16) | admission/by-object와 transfer/by-id 이중 index, per-entry phase/timer. admission public create/evict가 delete/abort를 재호출한다(L192, L264–265). transfer는 source-leave terminal을 await한 뒤 exact entry를 갱신한다(L90–107). | **1/3 + 1/2**. admission expiry timer L191. 두 index 유지와 retained entry가 핵심. **C2**. |
| [`ZLinkSpotActivation`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-activation-state.ts:77) + [`ZLinkSpotActivationLifecycle`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-activation.ts:161) | joined actor/claim/serial/departure와 close/relocation/idle state; lifecycle cleanup-state map. relocation-ready waiter를 읽고 await 후 boundary/deferred turn field를 갱신한다(L180–252). public seal/abort/commit/close 재호출 8곳. | **6/67 + 1/3**. execution seal, close cleanup, actor admission이 장기 작업. mutable activation 참조가 여러 caller로 퍼져 있어 signature 파급이 크다. **C2**, 같은 batch. |
| [`ZLinkEntrySpotActivation`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-entry-activation.ts:115) | serial/mailbox/timer/handler/dispatch/initialized/disposed/context. create/dispose/attach가 await 전후 initialization과 dispatcher field를 갱신한다(L270–325, L382–397). public `commitServiceActorJoin()`→`notifyJoinActor()` L360. | **2/5**. actor dispatch와 disposal이 장기 작업. 여러 execution primitive를 한 owner가 조율한다. **C2**. |
| [`ZLinkSpotNodeRuntimeManager`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-node-runtime-manager.ts:160) | mesh node/pump/completion/entry activation/publisher/publish-slot/auto-connect/descriptor/weight map. start, state publication, entry activation, dispose가 await 전후 여러 map을 갱신한다(L230–258, L350–376, L520–575, L819–971). public 재호출 4곳. | **10/30**. 44개 await, publish-slot timeout L1280, mesh pump/auto-connect 장기 작업. snapshot/sort/materialization이 매우 많다. **C2**. |
| [`ZLinkSpotRoutedActorAdmission`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-routed-actor-admission.ts:66) | transfer pending map과 per-entry phase/admissionTask/commitTask/reply/deadline. pending entry를 설치하고 admission/commit task를 await한 뒤 같은 entry를 갱신한다(L90–140 이후). | **1/3**. admission/commit와 deadline timer가 장기 작업. decoded frame과 reply를 entry에 보존한다. **C2**. |
| [`ZLinkSpotTimerRegistry`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts:54) + [`ZLinkManagedTimer`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts:209) | timer/generation map과 execution barrier; managed timer의 disposed/paused/index/timeout/running. add가 generation/barrier를 읽고 start await 뒤 map을 갱신한다(L90–116). timer fire는 handler await 뒤 exact index와 disposed를 갱신한다(L330–344). public abort/commit/dispose 재호출 3곳. | **6/18 + 2/5**. scheduler timeout L310과 relocation freeze/restore가 장기 작업. timer snapshot/sort L133–143. **C2**, 함께 전환. |

### Diagnostics·Node backend·handler scope

| 전환 단위 | 보호 상태와 await·재진입 | 장기 작업·파급·POSDDD |
|---|---|---|
| [`ZLinkLocationRuntimeMonitoringSource`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/diagnostics/index.ts:115) + [`ZLinkClientServerRuntimeProjection`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/diagnostics/topology-runtime-projections.ts:36) + [`ZLinkFanoutRuntimeProjection`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/diagnostics/topology-runtime-projections.ts:162) | monitoring은 이전 status/topology/summary와 store failure를 읽고 6번 await한 뒤 snapshot들을 갱신한다(L133–175). projections는 sequence와 observer set을 갱신하면서 runtime/host callback을 실행하고, public methods가 `snapshot()`을 재호출한다. | **0/0 + 1/4 + 1/4**. observer AsyncIterable가 장기 작업. status DTO·target 배열·Date를 매 관측마다 생성한다. **C2**. |
| [`ZLinkNodeRawMeshBackend`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:150) 및 `RawServiceSpot`/`RawStreamSessionService` | channel/peer-intent/completion queue와 lifecycle/weight/poller/backend callbacks. poll이 runtime receive를 await한 뒤 pump observation과 queue state를 갱신한다(L1490–1514). `shutdown()`→`close()`, disconnect→remove 재호출. nested service도 closed flag와 subscription/target map의 public close 재진입을 가진다. | **2/3 + 1/5 + 1/1**. poll timer L1493, pending completion Promise가 장기 작업. channel/peer snapshot 정렬과 completion-array compaction L1453–1471/L1619. **C2**. |
| [`DefaultHandlerInstanceScope`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/handlers/handler-instance-scope.ts:125) + [`LifecycleHandlerInstanceScope`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/handlers/handler-instance-scope.ts:168) | instance/owned/disposed와 active invocation/closing/idle/disposal. create/dispose가 factory·instance disposal을 await한 뒤 map/set을 갱신한다(L140–160, L185–229). | **1/1 + 1/4**. handler disposal·idle waiter가 장기 작업. instance snapshot과 owned set 중복 보관이 있다. **C2**. |

## 별도 제외

### L1 registry 전환 범위

다음은 이미 완료된 `ZLinkActorSessionBindingRegistry` 전환 또는 그 async 전파 범위이므로 L2 후보에서 제외했다.

- `actor-session-binding-registry.ts`
- `actor-session-binding-runtime-owner.ts`
- `stream-binding-runtime-ports.ts`
- `streams/index.ts`
- `session-actor-coordinator.ts`
- `bound-session-service.ts`
- `bound-actor-relay-sender.ts`
- `service-session-binding-ingress-port.ts`
- 해당 port를 소비하도록 바뀐 `service-stateful-runtime.ts`
- `actor-packet-relay.ts`
- `remote-actor-packet-target-store.ts`
- `remote-bound-session-relay.ts`
- 위 변경의 runtime 연결 파일

현재 문서에는 “runtime 13파일 async 전파”로 기록되어 있다. 이 조사에서는 해당 연결 그래프 전체를 제외했고, 그 안의 큰 클래스가 다른 상태도 소유한다는 이유로 다시 L2 후보에 넣지 않았다.

### 실행·admission primitive

아래는 C2 형태의 내부 상태를 가지더라도 일반 state-owner 전환 대상이 아니다.

- `ZLinkStateLane`
- `ZLinkBoundedSerialScheduler`, `ZLinkExecutionBarrier`, `ZLinkSpotSerialExecutor`
- `ApplicationJobQueue`, receive-flow/ingress permit owner
- `ServiceMailbox`, mailbox scheduler, `EventLoopWorkQueues`
- `ZLinkMeshDispatchPump`
- channel receive-loop 3종과 round-robin coordinator
- Spot actor-packet/route/subscription/join drain
- `RuntimeEventQueue`
- `ZLinkCpuWorkerPool`
- relocation in-flight budget

이들은 lane·queue·mailbox·drain primitive 자체이므로 L2 state-owner 확산이 수렴한 뒤 별도 primitive 감사로 다뤄야 한다.

`ZLinkActorRuntimeState`처럼 복잡한 mutable field를 가지더라도 모든 전이가 현재 한 동기 JS turn 안에서 끝나고 public 재진입이 확인되지 않은 클래스도 이번 기준에서는 제외했다.

## 제안 전환 순서

배수는 이미 전환한 `ZLinkActorSessionBindingRegistry`를 **1.0배**로 둔 정적 난이도다. 호출 파급, sync→Promise 전환, 재진입, 장기 작업, mutable-reference escape를 함께 반영했다.

| 순서 | 전환 batch | 표본 대비 |
|---:|---|---:|
| 1 | `OperationRegistry` | 0.2배 |
| 2 | `ActorJoinRegistrationScope` | 0.2배 |
| 3 | formal remote admission/transfer registries | 0.4배 |
| 4 | post-commit binder + location | 0.5배 |
| 5 | handler instance scopes | 0.5배 |
| 6 | diagnostics monitoring/projections | 0.6배 |
| 7 | discovery + topology + weighted selection | 0.8배 |
| 8 | in-memory provider store | 0.5배 |
| 9 | in-memory location + authority store | 0.9배 |
| 10 | actor/session-route/Spot claims + location lifecycle | 1.0배 |
| 11 | owner lease tracker + route resolvers | 0.9배 |
| 12 | auto-connect loop + reconciler + Spot-node executor | 1.1배 |
| 13 | ClientServer location runtime | 0.8배 |
| 14 | Fanout location runtime | 0.8배 |
| 15 | service maintenance runtime | 0.6배 |
| 16 | service stateful registry | 1.1배 |
| 17 | mesh completion table | 1.0배 — 본체는 작지만 13파일/40지점 |
| 18 | channel runtime lifecycle | 1.2배 |
| 19 | channel socket registry | 1.8배 |
| 20 | managed stream + stream-session runtimes | 1.8배 |
| 21 | Spot activation registry + timer registry | 1.2배 |
| 22 | Spot activation + lifecycle + Entry activation | 1.8배 |
| 23 | actor manager + transferred rollback | 1.7배 |
| 24 | actor handoff coordinator | 1.3배 |
| 25 | instance activation authority + user-Spot creation coordinator | 1.7배 |
| 26 | location runtime + runtime owner + authority-route runtime | 2.0배 |
| 27 | Spot-node runtime manager + routed actor admission | 2.2배 |
| 28 | raw service mesh runtime | 2.3배 |
| 29 | Node raw mesh backend와 중첩 service 객체 | 2.2배 |
| 30 | actor transfer runtime | 2.4배 |
| 31 | RouteMesh runtime coordinator | 1.8배 |
| 32 | default Spot manager | 2.8배 |
| 33 | framework runtime host | 3.5배 |
| 34 | host service relocation runtime + `LocalTargetPort` | 4배 이상 |

`ZLinkMeshCompletionTable`은 작은 초기 표본으로 보이지만 callback 재진입 계약과 40개 참조 지점 때문에 첫 6개보다 뒤에 두었다. `ZLinkFrameworkRuntimeHost`보다 `ZLinkHostServiceRelocationRuntime`을 마지막에 둔 이유는 호출 파일 수가 아니라 16개 상태 컬렉션, 154개 await, 10개 timer와 per-stage Promise tail이 한 클래스에 결합되어 있기 때문이다.

## 작업 상태

- 저장소 파일 변경: **0**
- build/test: **실행하지 않음**
- Git 명령: **실행하지 않음**
- 저장소 및 `/tmp` 파일 생성·수정: **0**
- 조사 시작: **2026-08-26 17:42:04 KST**
- 마지막 근거 확인: **2026-08-26 17:57:08 KST**
- 걸린 시간: **15분 4초**
