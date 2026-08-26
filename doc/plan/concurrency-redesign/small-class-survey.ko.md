# dotnet 소형 잔여 전수 조사 (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다. 소형 배치 편성의 근거.

조사 결과 소형 잔여 타입은 예상치보다 많은 **99개**였습니다.

- **C2: 91개**
- **C1: 3개**
- **C3: 4개**
- **C1+C3: 1개**
- 현재 진행 중인 `ZLinkManagedMeshNode.cs` 안의 `ManagedActor`, `ZLinkManagedSpot`도 별도 클래스이므로 포함했지만, 해당 대형 작업이 끝나기 전에는 병렬 배치에 투입하면 안 됩니다.
- 판정 기준: [state ownership §4–6](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:75), [concurrency design §4](/home/hep7/project/zlink/doc/plan/concurrency-redesign/design.ko.md:46).

표의 경로는 모두 `framework/languages/dotnet/src/Zlink.Framework/` 기준입니다. `L`은 `lock`, `S`는 상태 mutex인 `SemaphoreSlim`입니다. 재진입은 같은 타입 표면의 정적 자기 호출 후보이며, 동적 dispatch는 포함하지 않았습니다. 장기 작업은 detached task·loop·timer·expiry 시작점입니다.

| 클래스 / 파일 경로 | lock 수·gate | 판정과 근거 | 파급 파일 | 재진입 의심 | 장기 작업 | 특이점 |
|---|---|---|---:|---:|---:|---|
| `TargetStage` — `Runtime/Spots/ZLinkSpotRetireTransport.cs` | 3L `HeldGate`1, `_finalRootGate`2 + 2S `PublishGate`,`AbortGate` | C2 — held records·final root·abort/publish 상태와 async cleanup 결합 | 1 | 0 | 0 | `cleanup`을 `AbortGate` 보유 중 호출하는 유형③; sync 92% |
| `ZLinkActivationConcurrencyAdmission` — `Runtime/Spots/ZLinkActivationConcurrencyAdmission.cs` | 3L `_gate` | C3 — 단일 active counter의 bounded 증감 | 5 | 0 | 0 | 변경 콜백은 lock 밖 호출; sync 100% |
| `ZLinkActorBoundSessionCoordinator` — `Runtime/Host/ZLinkActorBoundSessionCoordinator.cs` | 12L `_outboundProofGate`7, `_pendingRemoteRequests`5 | C2 — proof map·remote request admission·epoch/cancellation 불변식 | 2 | ≈11 | 2 | 두 gate와 높은 동기 표면 83% |
| `…PendingOutboundProof` — 같은 파일 | 4L `_turnGate` | C2 — ticket·turn map·serving 순서 불변식 | 0 | 0 | 0 | private 중첩; sync 67% |
| `ZLinkActorBoundSessionRegistry` — `Runtime/Host/ZLinkActorBoundSessionRegistry.cs` | 4L `_gate` | C1 — 단일 session→binding registry; immutable value/CAS로 전환 가능 | 1 | 0 | 0 | `unbind`는 lock 밖 호출; sync 100% |
| `ZLinkActorDispatchMailbox` — `Runtime/Actors/ZLinkActorDispatchMailbox.cs` | 8L `_sync` | C2 — admission flag·waiter/pending counts·serial queue admission 결합 | 2 | ≈2 | 1 | queue 호출과 gate lock-order 확인 필요; sync 83% |
| `ZLinkActorHandlerActivation` — `Runtime/Actors/ZLinkActorHandlerActivation.cs` | 1L `_gate` | C3 — 단일 dispose-task의 exact-once 초기화 | 1 | 0 | 0 | `Lazy<Task>` 계열 필요; async 표면 100% |
| `ZLinkActorJoinPrewarmRegistry` — `Runtime/Actors/ZLinkActorJoinPrewarmRegistry.cs` | 5L `_gate` | C2 — handoff/object 두 map과 parked-frame 전이 | 1 | ≈3 | 0 | `captureFrame`, `deliver`를 lock 안에서 호출하는 유형③ |
| `ZLinkActorMessageFollowLease` — `Runtime/Actors/ZLinkActorHandoffState.cs` | 5L `_gate` | C2 — phase·commit time·duration·suppression 상태 결합 | 0 | 0 | 0 | suppression 내부 gate와 중첩 lock-order; sync 100% |
| `ZLinkActorMessageFollower.ActorQueue` — `Runtime/Spots/ZLinkActorMessageFollower.cs` | 2L `_lifecycleGate` | C2 — queue·bytes·retired/scheduled flag 결합 | 0 | 0 | 1 | private 중첩 retirement 작업; sync 100% |
| `ZLinkActorSessionRegistry` — `Runtime/Actors/ZLinkActorSessionRegistry.cs` | 6L `_gate` | C2 — map clear 결정 뒤 generation fence·async cleanup 수행 | 2 | 0 | 1 | map만 보이지만 `ResetGenerationAsync` 때문에 C1 아님; sync 83% |
| `ZLinkApplicationJobQueue` — `Runtime/Dispatch/ZLinkApplicationJobQueue.cs` | 9L `_gate` | C2 — waiters·permits·queued count·pressure epoch 결합 | 12 | ≈3 | 0 | `ReceiveFlowController`와 gate 공유; sync 91% |
| `ZLinkAutoConnectLifecycleCoordinator` — `Runtime/Host/ZLinkAutoConnectLifecycleCoordinator.cs` | 2L `_gate` | C2 — state/start/stop task의 lifecycle 전이 | 1 | 0 | 0 | `_start` async delegate가 lock 보유 경로에서 시작되는 유형③ |
| `ZLinkAutoConnectLoop` — `Runtime/Locations/ZLinkAutoConnectLoop.cs` | 1L `_disposeGate`; `_wake`는 signal | C3 — lock 보호 상태는 단일 dispose-task | 1 | ≈3 | 4 | long loop와 dispose factory의 동기 선행 구간 주의; sync 20% |
| `ZLinkAutoConnectReconciler` — `Runtime/Locations/ZLinkAutoConnectReconciler.cs` | 0L+1S `_reconcileGate` | C2 — 전체 async reconcile을 배타화하며 여러 상태 결정을 수행 | 2 | 0 | 0 | semaphore→semaphore 치환 금지 사례; sync 44% |
| `ZLinkAutomaticFanoutSubscriberRuntime` — `Runtime/Channels/ZLinkAutomaticFanoutSubscriberRuntime.cs` | 5L `_gate` | C2 — connections·excluded·location·dispose 상태 결합 | 3 | 0 | 0 | outer 표면 async 75% |
| `…Connection` — 같은 파일 | 11L `_gate` | C2 — descriptor·ready/state·failure와 loop lifecycle 결합 | 0 | 0 | 3 | private 중첩; sync 75% |
| `ZLinkBackendSpotNodeWrapper` — `Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs` | 8L `_lifecycleGate`3, `_entrySpotGate`1, `_forwardGate`4 | C2 — node lifecycle·entry spot·forward buffers/completions 결합 | 1 | ≈5 | 0 | backend 외부 호출을 gate 아래 수행하는 자리 있음; sync 81% |
| `ZLinkBackendStreamSocketWrapper` — `Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs` | 5L `_sessionGate`2, `_sendGate`3 | C2 — session ownership과 serialized send lifecycle 결합 | 1 | 0 | 1 | binding/socket 호출과 gate 순서 주의; sync 73% |
| `ZLinkBoundSessionDispatchScope` — `Runtime/Streams/ZLinkBoundSessionDispatchScope.cs` | 3L `_gate` 계열 +1S `_drainGate` | C2 — deferred queue·drained flag·async drain 순서 | 7 | ≈2 | 0 | operation callback은 object lock 밖이나 drain semaphore 안에서 실행; sync 20% |
| `ZLinkBoundedIngressAdmission` — `Runtime/ZLinkBoundedIngressAdmission.cs` | 7L `_gate` | C2 — record/byte counters·closed·empty waiter 불변식 | 3 | 0 | 0 | completion은 lock 밖; sync 80% |
| `ZLinkChannelReplyGate` — `Runtime/Channels/ZLinkChannelApplicationDispatchQueue.cs` | 2L `_gate` | C2 — close와 reply callback 실행의 원자 경계 | 2 | 0 | 0 | `reply()` lock 안 호출 유형③; sync 100% |
| `ZLinkChannelRuntimeBundle` — `Runtime/Channels/ZLinkChannelRuntimeBundle.cs` | 3L `_disposeGate` +2S `_connectionGate`,`ReceiveGate` | C2 — connection/receive/dispose lifecycle 결합 | 4 | 0 | 0 | sync 75% |
| `ZLinkChannelRuntimeManager` — `Runtime/Channels/ZLinkChannelRuntimeManager.cs` | 3L `state.SyncRoot` | C2 — 여러 bundle dictionary와 async disposal 결합 | 3 | ≈2 | 1 | foreign gate 소유; sync 67% |
| `ZLinkClientServerRuntimeService` — `Runtime/Channels/ZLinkClientServerRuntimeService.cs` | 3L `_gate` | C2 — monitor hub·sequence registry 결합 | 0 | 0 | 0 | sync 67% |
| `…MonitorHub` — 같은 파일 | 4L `_gate` | C2 — observers·producer·signal·last state 결합 | 0 | 0 | 1 | private 중첩; sync 75% |
| `ZLinkClientServerServerIdentity` — `Runtime/Channels/ZLinkClientServerServerIdentity.cs` | 13L `_gate` | C2 — peer set·state·revision·weight/router identity 결합 | 8 | ≈5 | 1 | sync 93%; signature 파급 큼 |
| `ZLinkCompletionDispatcher` — `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs` | 4L `_gate` | C2 — linked queue head/tail와 reservation count 불변식 | 1 | 0 | 1 | 영구 worker thread; callbacks는 lock 밖 실행 |
| `ZLinkDeadlineClock` — `Runtime/Service/ZLinkDeadlineClock.cs` | 1L `_gate` | C2 — observed wall time와 monotonic timestamp의 2-field 불변식 | 1 | 0 | 0 | sync 100% |
| `ZLinkDeferredActorJoinHandlerScope` — `Runtime/Actors/ZLinkDeferredActorJoin.cs` | 3L `_sync` 계열 | C2 — sealed/completed와 join/request collection 결합 | 2 | 0 | 0 | sync 100% |
| `ZLinkDirectReplyCompletionRegistry` — `Runtime/Spots/ZLinkDirectReplyCompletionRegistry.cs` | 3L `_gate` | C2 — pending·terminal map·terminal order·capacity 결합 | 1 | 0 | 0 | sync 100% |
| `ZLinkDrainAdmissionGate` — `Runtime/Host/ZLinkDrainAdmissionGate.cs` | 11L `_gate` | C2 — owner·sealed/draining·lease·admission counter/TCS 결합 | 3 | 0 | 0 | `commit/reopen/acquire/complete` lock 안 호출 유형③; sync 93% |
| `ZLinkDrainCoordinator` — `Runtime/Host/ZLinkDrainCoordinator.cs` | 4L `_gate` | C2 — shared operation·force-stop·terminal lifecycle 결합 | 1 | ≈5 | 2 | async operation을 lock 보유 중 시작하는 유형③ 후보; sync 29% |
| `ZLinkEndpointConnections` — `Runtime/Configuration/ZLinkEndpointConnections.cs` | 7L `_endpoints` | C2 — endpoint list·attachment·frozen mode 결합 | 2 | ≈1 | 0 | connect/disconnect 콜백을 lock 안 호출하는 명확한 유형③; sync 100% |
| `ZLinkEntrySpotActivation` — `Runtime/Spots/ZLinkEntrySpotActivation.cs` | 1L `_lifecycleGate` +1S `_gate` | C2 — activation execution과 lifecycle/finalization 상태 결합 | 4 | 0 | 2 | sync 36% |
| `ZLinkEntrySpotDispatchPump.ActorLane` — `Runtime/Spots/ZLinkEntrySpotDispatchPump.cs` | 3L `_lifecycleGate` | C2 — queue·pipeline·retirement state 결합 | 0 | 0 | 1 | private executor-like 타입; sync 50% |
| `ZLinkEnvelopeCodec` — `Runtime/Messaging/ZLinkEnvelopeCodec.cs` | 2L static cache gates | C2 — header map+eviction queue 및 decoded-cache 배열 전이 | 39 | ≈17 | 0 | 전부 sync이며 파급 최대; 단순 C1 cache 아님 |
| `ZLinkFanoutPublisherIdentity` — `Runtime/Channels/ZLinkFanoutPublisherIdentity.cs` | 2L `_gate` | C2 — revision과 runtime state가 한 snapshot 불변식 | 3 | ≈2 | 0 | sync 100% |
| `ZLinkFanoutRuntimeService` — `Runtime/Locations/ZLinkFanoutRuntimeService.cs` | 7L `_gate` | C2 — observer registry·host lifecycle·state 결합 | 3 | ≈1 | 0 | sync 80% |
| `ZLinkFrameworkComponentState` — `Runtime/Host/ZLinkFrameworkRuntimeState.cs` | 3L `SyncRoot`2, `_disposeGate`1 | C2 — 여러 runtime dictionary와 disposal 결합 | 18 | 0 | 0 | `SyncRoot` 외부 소비 18파일; sync 71% |
| `ZLinkFrameworkMaintenanceRuntime` — `Runtime/Host/ZLinkFrameworkMaintenanceRuntime.cs` | 17L `_gate` | C2 — relocation operation/result/mode/version/cancellation 결합 | 1 | ≈1 | 0 | 내부 상태량이 큰 소형 후보; sync 57% |
| `ZLinkHostCapacityProjection` — `Runtime/Host/ZLinkHostCapacityProjection.cs` | 2L `_applicationJobQueue.SyncRoot` | C2 — queue count와 measurement epoch의 shared invariant | 1 | ≈1 | 0 | foreign gate; sync 100% |
| `ZLinkInMemoryProviderLocationStore` — `Runtime/Locations/ZLinkInMemoryProviderLocationStore.cs` | 3L `_gate` | C2 — entries·scan snapshots·version·expiry cursor 결합 | 1 | 0 | 0 | 표면은 이미 전부 async |
| `ZLinkInstanceSpotMonitoring.Aggregate` — `Runtime/Spots/ZLinkInstanceSpotMonitoring.cs` | 3L `_gate` | C2 — message/byte counters와 last outcome 불변식 | 0 | 0 | 0 | private 중첩; sync 100% |
| `ZLinkLocationAutoConnectHost` — `Runtime/Locations/ZLinkLocationAutoConnectHost.cs` | 1L `_disposeGate` +1S `_lifecycleGate` | C2 — start/stop/dispose lifecycle 직렬화 | 3 | 0 | 0 | sync 47% |
| `ZLinkLocationLifecycle` — `Runtime/Locations/ZLinkLocationLifecycle.cs` | 6L `_disposeStartGate`1, `_backgroundGate`5 +1S `_backgroundDrainGate` | C2 — background task set·stop source·dispose 상태 결합 | 9 | 0 | 1 | operation async delegate를 lock 안에서 시작하는 유형③; sync 25% |
| `ZLinkLocationRuntime` — `Runtime/Locations/ZLinkLocationRuntime.cs` | 1L `_disposeStartGate` +1S `_lifecycleGate` | C2 — lifecycle state와 async disposal/child work 결합 | 11 | ≈4 | 7 | async 표면 75%; 파급 큼 |
| `ZLinkLocationStoreHealth` — `Runtime/Locations/ZLinkLocationStoreHealth.cs` | 4L `_gate` | C2 — failure map·timestamps·recovery generation 결합 | 6 | ≈1 | 0 | `Changed`는 lock 밖 호출; sync 100% |
| `ZLinkManagedMeshNode.ManagedActor` — `Runtime/Service/ZLinkManagedMeshNode.cs` | 8L `_gate` | C2 — binding·draining·location/membership 상태 결합 | 0 | 0 | 0 | 진행 중 대형 파일과 충돌하므로 후행 전용; sync 100% |
| `ZLinkManagedSpot` — 같은 파일 | 2L `_subscriptions`; actor count는 atomic | C1+C3 — subscription map과 독립 actor counter 사이 불변식 없음 | 0 | ≈1 | 0 | lane 불필요지만 진행 중 대형 파일 종료 후 작업; sync 94% |
| `ZLinkMeshCompletionTable` — `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs` | 8L `_gate` | C2 — pending map·capacity/outstanding·closed/drained 결합 | 5 | ≈5 | 0 | dispatcher와 lock-order 존재; sync 100% |
| `ZLinkMeshDispatchPump` — `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs` | 4L `_lifecycleGate`; `_signal`은 signal | C2 — pending domains·loop/start/stop lifecycle 결합 | 4 | ≈5 | 1 | sync 89% |
| `ZLinkMeshNodeOwnedMailbox` — `Runtime/Service/ZLinkMeshOwnedMailbox.cs` | 7L `_gate` | C2 — record queue·bytes·claimed flag 결합 | 1 | 0 | 0 | `onRecordEnqueued` lock 안 호출 유형③; dequeue callback은 밖 |
| `ZLinkMeshNodeRouteDispatcher` — `Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs` | 2L `_orderedActorRelayGate` | C2 — actor별 task tail과 error/dispatch 후속 행동 결합 | 1 | ≈1 | 0 | sync 100% |
| `ZLinkMessageFollowSuppressionRegistry` — `Runtime/Messaging/ZLinkMessageFollowSuppressionRegistry.cs` | 5L `_gate` | C2 — marker state machine과 global capacity 불변식 | 2 | 0 | 0 | 단일 map이나 Count+Add 원자성 때문에 C1 아님 |
| `ZLinkObservationQueue` — `Runtime/Diagnostics/ZLinkObservationQueue.cs` | 3L `_gate`; `_available`은 signal | C2 — 세 collection·ordinal·loss counters·complete 상태 결합 | 4 | ≈4 | 0 | sync 75% |
| `ZLinkObservedLocationGenerations.Observed` — `Runtime/Locations/ZLinkObservedLocationGenerations.cs` | 4L `_gate` | C2 — map 전체 순회와 사용자 predicate를 하나의 원자 경계로 실행 | 0 | 0 | 0 | `predicate` lock 안 호출 유형③; sync 100% |
| `…ObservedDescriptors` — 같은 파일 | 2L `_gate` | C2 — owner/version/retired-owner set과 predicate 순회 결합 | 0 | 0 | 0 | 유형③; 위 타입과 같은 파일이므로 별도 배치 |
| `ZLinkOwnerLeaseTracker` — `Runtime/Locations/ZLinkOwnerLeaseTracker.cs` | 2L `_cacheGate` | C1 — 단일 owner→snapshot cache; store read는 lock 밖 | 8 | 0 | 0 | lane 불필요; 표면 전부 async |
| `ZLinkProviderLocationRepository` — `Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs` | 0L+2S `authorityGenerationGate`,`aggregateRecoveryGate` | C2 — authority generation·aggregate recovery의 async CAS workflow | 1 | ≈41 | 2 | partial 전체 자기호출이 매우 많음; sync 18% |
| `ZLinkReceiveFlowController` — `Runtime/Dispatch/ZLinkApplicationJobQueue.cs` | 8L `_gate`6, `entry.ApplyGate`2 | C2 — entry registry·pending updates·sequence/state 결합 | 0 | 0 | 0 | binding `Apply`를 `ApplyGate` 안 호출하는 유형③ |
| `ZLinkRelocationAttemptLeaseState` — `Runtime/Host/ZLinkStandaloneActorRelocationRuntime.cs` | 4L `_gate` | C2 — users·closing·quiesced completion 불변식 | 0 | 0 | 0 | sync 100% |
| `ZLinkRelocationChunkAssembler` — `Runtime/Locations/ZLinkRelocationDirectTransfer.cs` | 2L `_gate` | C2 — received set·lengths·buffer·completed 상태 결합 | 1 | 0 | 0 | sync 100% |
| `ZLinkRelocationTransferBudget` — 같은 파일 | 3L `_gate` | C2 — in-flight bytes와 waiter queue 결합 | 1 | ≈1 | 0 | 위 타입과 별도 배치; sync 50% |
| `ZLinkRemoteRelayFrameAssembler` — `Runtime/Host/ZLinkRemoteRelayFrameAssembler.cs` | 6L `_gate` | C2 — pending frames·buffer bytes·dispose 상태 결합 | 1 | ≈3 | 1 | `_getShutdownToken` lock 안 호출 유형③ |
| `ZLinkResolvedSpotHandle` — `Runtime/Locations/ZLinkLocationAddressResolvers.cs` | 6L `_gate` | C2 — snapshot·availability·version 불변식 | 5 | ≈1 | 0 | sync 80% |
| `ZLinkRouteMeshRuntimeService` — `Runtime/Host/ZLinkRouteMeshRuntimeService.cs` | 7L `_monitorGate`6, `_sequenceGate`1 | C2 — monitor registry·stopped state·sequence map 결합 | 2 | ≈4 | 0 | sync 88% |
| `…MonitorHub` — 같은 파일 | 13L `_gate` | C2 — observers·descriptors·last status·producer lifecycle 결합 | 0 | 0 | 2 | private 중첩; 위 타입과 별도 배치 |
| `ZLinkRuntimeTaskRunner` — `Runtime/Execution/ZLinkRuntimeTaskRunner.cs` | 4L `_gate` | C2 — accepting·active tasks·supervisor admission 결합 | 12 | ≈1 | 1 | detached-task 시작 소유자; sync 29% |
| `ZLinkRuntimeTaskSupervisor` — 같은 파일 | 3L `_gate` | C2 — accepting flag와 active task set 결합 | 0 | 0 | 0 | `start()` lock 안 호출 유형③; runner와 별도 배치 |
| `ZLinkScopedHandlerInstanceOwner` — `Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs` | 2L `_gate` | C2 — instance map·disposed·dispose-task 전이 | 9 | ≈1 | 0 | sync 50% |
| `ZLinkSerialWorkItem` — `Runtime/Execution/ZLinkSerialWorkItem.cs` | 1L `_acceptedPayloadGate` | C2 — created flag·payload와 exact-once factory 실행 결합 | 3 | 0 | 1 | payload factory lock 안 호출 유형③ |
| `ZLinkSessionActor` — `Runtime/Streams/ZLinkSessionActor.cs` | 1L `_disconnectGate` | C3 — 단일 disconnect-task exact-once 초기화 | 6 | ≈1 | 0 | `Lazy<Task>` 필요; sync 33% |
| `ZLinkSessionActorCoordinator` — `Runtime/Streams/ZLinkSessionActorCoordinator.cs` | 2L `_actorOperationGatesLock` | C2 — actor→operation-gate registry와 lifetime 결합 | 1 | 0 | 1 | sync 17% |
| `…ActorOperationGate` — 같은 파일 | 0L+1S `Gate` | C2 — actor operation 전체를 async 직렬화 | 0 | 0 | 0 | private 중첩; coordinator와 별도 배치 |
| `ZLinkSpotActivation` — `Runtime/Spots/ZLinkSpotActivation.cs`, `…Execution.cs` | 13L `_lifecycleGate`1, `_relocationReadyGate`4, `_messageFollowPendingGate`8 +1S `_membershipPublicationGate` | C2 — relocation/message-follow/lifecycle 상태가 광범위하게 결합 | 17 | ≈23 | 10 | partial, 고파급·장기 작업 다수; sync 44% |
| `ZLinkSpotActorMembership` — `Runtime/Spots/ZLinkSpotActorMembership.cs` | 5L `_gate` | C1 — 단일 actor map의 get/add/remove/snapshot | 5 | 0 | 0 | lane 불필요; sync 100% |
| `ZLinkSpotHandleRegistry` — `Runtime/Locations/ZLinkSpotHandleRegistry.cs` | 3L `_gate` | C2 — actor/spot 두 map과 handle update 전이 | 1 | 0 | 0 | `update(handle)` lock 안 호출 유형③; sync 100% |
| `ZLinkSpotLocationLifecycle` — `Runtime/Locations/ZLinkSpotLocationLifecycle.cs` | 7L `_gate` | C2 — spot map과 async activation/close lifecycle 결합 | 1 | 0 | 0 | sync 43% |
| `ZLinkSpotNodeBundleRegistry` — `Runtime/Spots/ZLinkSpotNodeBundleRegistry.cs` | 2L `_gate` | C2 — publisher map·closed·dispose-task 결합 | 1 | 0 | 0 | sync 50% |
| `ZLinkSpotNodeRuntime` — `Runtime/Spots/ZLinkSpotNodeRuntime.cs` | 6L `_disposeGate` | C2 — stop source·dispose state·attachments·bundles/spots 결합 | 18 | 0 | 0 | 파급 큼; sync 52% |
| `ZLinkSpotPeerConnectionSet` — `Runtime/Spots/ZLinkSpotPeerConnectionSet.cs` | 9L `_gate` | C2 — auto/manual/retained 세 collection의 ownership 불변식 | 2 | 0 | 0 | sync 100% |
| `ZLinkSpotPeerConnector` — `Runtime/Spots/ZLinkSpotPeerConnector.cs` | 6L `_gate` | C2 — claim rollback과 physical connect/disconnect의 원자 경계 | 1 | ≈1 | 0 | `node`·`connections` 외부 호출을 lock 안 수행하는 유형③ |
| `ZLinkSpotRetireTargetRuntime` — `Runtime/Spots/ZLinkSpotRetireTransport.cs` | 2L `stage.HeldGate`; `_reconciliationWake`는 signal | C2 — target-stage registry와 held-record/reconcile lifecycle 결합 | 7 | ≈30 | 6 | 대형 소스 내부 자기호출 다수; sync 43% |
| `ZLinkSpotSerialExecutor` — `Runtime/Spots/ZLinkSpotSerialExecutor.cs` | 19L `_laneGate`3, `_barrierGate`16 | C2 — queue·claims·barrier·relocation admission 결합 | 4 | ≈5 | 1 | primitive-like; 두 callback을 barrier lock 안 호출하는 유형③ |
| `ZLinkSpotTimerRegistry` — `Runtime/Spots/ZLinkSpotTimerRegistry.cs` | 9L `_lifecycleGate` | C2 — timers·closed/frozen/restore/finalization 결합 | 3 | ≈2 | 1 | timer 외부 메서드를 gate 아래 호출; sync 50% |
| `ZLinkStandaloneActorRelocationRuntime.AttemptSlot` — `Runtime/Host/ZLinkStandaloneActorRelocationRuntime.cs` | 0L+1S `Gate` | C2 — stage/abort/routed-join identity의 복합 transaction 직렬화 | 0 | 0 | 0 | private 중첩; sync 100% |
| `…TargetStage` — 같은 파일 | 5L `_readySubmissionGate` | C2 — READY phase와 external current/remove 결정의 원자 경계 | 0 | 0 | 0 | `isCurrent/remove` lock 안 호출 유형③; sync 100% |
| `ZLinkStoreLocationResolvers` — `Runtime/Locations/ZLinkStoreLocationResolvers.cs` | 6L `_routeCacheGate` | C2 — actor/spot 두 cache와 health/time 판단 결합 | 11 | ≈4 | 0 | 두 map이므로 C1 아님; sync 50% |
| `ZLinkStreamNodeRuntime` — `Runtime/Streams/ZLinkStreamNodeRuntime.cs` | 11L `_disposeGate`1, `state.Gate`3, `_receiveStateGate`7 | C2 — receive states·disconnect ordering·snapshot·loop lifecycle 결합 | 2 | 0 | 1 | sync 60% |
| `ZLinkStreamReceiveState` — `Runtime/Streams/ZLinkStreamReceiveBuffer.cs` | 1L `Gate` | C2 — Removed·Pending·Buffer disposal 불변식 | 1 | 0 | 0 | sync 100% |
| `ZLinkStreamSessionLiveness` — `Runtime/Streams/ZLinkStreamSessionLiveness.cs` | 4L `_gate` | C2 — heartbeat flag와 두 timestamp의 decision state | 2 | 0 | 0 | sync 100% |
| `ZLinkStreamSessionRuntime` — `Runtime/Streams/ZLinkStreamSessionRuntime.cs` | 11L `_disposeGate`1, `_replacementGate`4, `_terminalGate`5, `_transportCloseGate`1 | C2 — replacement·terminal callback·transport close lifecycle 결합 | 3 | ≈3 | 7 | terminal callback 경계 있음; sync 59% |
| `ZLinkStreamSessionSerialExecutor` — `Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs` | 4L `_disposeGate`1, `_stopGate`3 | C2 — queue stop·cancellation·source disposal 결합 | 2 | ≈1 | 0 | sync 38% |
| `ZLinkStreamSessionTable` — `Runtime/Streams/ZLinkStreamSessionTable.cs` | 13L `_gate` | C2 — sessions·creation tasks·stopping/reject flags 결합 | 1 | 0 | 0 | sync 75% |
| `ZLinkTimer` — `Runtime/Timers/ZLinkTimer.cs` | 12L `_scheduleGate`11, `_lifecycleGate`1 | C2 — schedule indices·pending/active dispatch·freeze/lifecycle 결합 | 2 | ≈1 | 2 | sync 67% |
| `ZLinkTimerScheduler` — `Runtime/Timers/ZLinkTimerScheduler.cs` | 9L `_gate`8, `_disposeGate`1; `_wake`는 signal | C2 — priority queue·timer set·sequence·closed/pump 결합 | 8 | 0 | 1 | sync 75% |
| `ZLinkWorkerCall.Execution` — `Runtime/Execution/ZLinkWorkerCall.cs` | 3L `_admissionGate` | C2 — settled state와 fail/cancel callback 선택 불변식 | 0 | ≈2 | 0 | private 중첩; sync 100% |
| `ZLinkWorkerPool` — `Runtime/Execution/ZLinkWorkerPool.cs` | 11L `_sync`; `_directWaiterSlots`는 capacity semaphore | C2 — direct/queued work·thread/idle counts·dispose lifecycle 결합 | 4 | ≈1 | 2 | sync 67% |

## Lane이 불필요한 별도 목록

- **C1:** `ZLinkActorBoundSessionRegistry`, `ZLinkOwnerLeaseTracker`, `ZLinkSpotActorMembership`
- **C3:** `ZLinkActivationConcurrencyAdmission`, `ZLinkActorHandlerActivation`, `ZLinkAutoConnectLoop`, `ZLinkSessionActor`
- **C1+C3:** `ZLinkManagedSpot`

`ZLinkObservedLocationGenerations`의 두 중첩 타입은 단일 map처럼 보이지만, arbitrary predicate를 lock 안에서 실행하며 map mutation과 원자 경계를 만들기 때문에 C1에서 제외했습니다. `ZLinkStandaloneActorRelocationRuntime.TargetStage`도 단일 enum처럼 보이지만 `isCurrent/remove` 콜백이 같은 gate 아래 있으므로 C2입니다.

## 제안 배치

순서는 대체로 외부 사용 파일 수가 작은 것부터이며, 내부 크기·재진입·장기 작업이 큰 후보는 뒤로 밀었습니다. 각 배치 안에는 동일 파일이 없습니다.

### 비-lane 기계 전환

1. `ZLinkActorHandlerActivation`, `ZLinkAutoConnectLoop`, `ZLinkOwnerLeaseTracker`, `ZLinkSessionActor`
2. `ZLinkActorBoundSessionRegistry`, `ZLinkActivationConcurrencyAdmission`, `ZLinkSpotActorMembership`

`ZLinkManagedSpot`은 C1+C3지만 진행 중인 `ZLinkManagedMeshNode.cs`와 겹치므로 마지막 배치로 보냈습니다.

### C2 전환

1. `PendingOutboundProof`, `ZLinkActorMessageFollowLease`, `ActorQueue`, `ZLinkClientServerRuntimeService`
2. `ZLinkClientServerRuntimeService.MonitorHub`, `ActorLane`, `InstanceSpotMonitoring.Aggregate`, `ZLinkRuntimeTaskSupervisor`
3. `ZLinkReceiveFlowController`, `ZLinkRelocationAttemptLeaseState`, `ActorOperationGate`, `WorkerCall.Execution`
4. `AttemptSlot`, Spot-retire `TargetStage`, `ZLinkAutoConnectLifecycleCoordinator`, `ZLinkBackendStreamSocketWrapper`
5. `ZLinkCompletionDispatcher`, `ZLinkDeadlineClock`, `ZLinkDirectReplyCompletionRegistry`, `ZLinkHostCapacityProjection`, `Observed`
6. `ZLinkInMemoryProviderLocationStore`, `ZLinkMeshNodeOwnedMailbox`, `ZLinkMeshNodeRouteDispatcher`, `ZLinkRelocationChunkAssembler`, `ObservedDescriptors`
7. `ZLinkRemoteRelayFrameAssembler`, `ZLinkSpotHandleRegistry`, `ZLinkSpotLocationLifecycle`, `ZLinkSpotNodeBundleRegistry`
8. `ZLinkSpotPeerConnector`, `ZLinkStreamSessionTable`, `ZLinkRelocationTransferBudget`, `ZLinkSessionActorCoordinator`
9. `ZLinkActorJoinPrewarmRegistry`, `ZLinkAutoConnectReconciler`, `ZLinkSpotPeerConnectionSet`, `ZLinkStreamReceiveState`
10. `ZLinkActorDispatchMailbox`, `ZLinkActorSessionRegistry`, `ZLinkDeferredActorJoinHandlerScope`, `ZLinkStreamSessionLiveness`
11. `ZLinkBoundedIngressAdmission`, `ZLinkChannelReplyGate`, `ZLinkFanoutPublisherIdentity`, `ZLinkMessageFollowSuppressionRegistry`
12. `ZLinkAutomaticFanoutSubscriberRuntime`, `ZLinkChannelRuntimeManager`, `ZLinkFanoutRuntimeService`, `ZLinkLocationStoreHealth`
13. `AutomaticFanoutSubscriberRuntime.Connection`, `ZLinkChannelRuntimeBundle`, `ZLinkObservationQueue`, `ZLinkStreamSessionSerialExecutor`
14. `ZLinkDrainAdmissionGate`, `ZLinkEntrySpotActivation`, `ZLinkResolvedSpotHandle`, `ZLinkScopedHandlerInstanceOwner`
15. `ZLinkDrainCoordinator`, `ZLinkSpotTimerRegistry`, `ZLinkTimer`, `ZLinkTimerScheduler`
16. `ZLinkBoundSessionDispatchScope`, `ZLinkClientServerServerIdentity`, `ZLinkLocationAutoConnectHost`, `ZLinkStreamNodeRuntime`
17. `ZLinkMeshCompletionTable`, `ZLinkMeshDispatchPump`, `ZLinkRouteMeshRuntimeService`, `ZLinkWorkerPool`
18. `ZLinkEndpointConnections`, `RouteMeshRuntimeService.MonitorHub`, `ZLinkSerialWorkItem`, `ZLinkSpotSerialExecutor`
19. `ZLinkApplicationJobQueue`, `ZLinkRuntimeTaskRunner`, `ZLinkStoreLocationResolvers`, `ZLinkLocationLifecycle`
20. `ZLinkFrameworkComponentState`, `ZLinkSpotNodeRuntime`, `ZLinkLocationRuntime`, `ZLinkStreamSessionRuntime`
21. `ZLinkActorBoundSessionCoordinator`, `ZLinkBackendSpotNodeWrapper`, `ZLinkFrameworkMaintenanceRuntime`, `ManagedActor`, standalone `TargetStage`
22. `ZLinkEnvelopeCodec`, `ZLinkProviderLocationRepository`, `ZLinkSpotActivation`, `ZLinkSpotRetireTargetRuntime`, `ZLinkManagedSpot`

배치 21–22의 `ManagedActor`와 `ZLinkManagedSpot`은 **현재 `ZLinkManagedMeshNode` 작업이 완전히 끝난 뒤에만** 시작해야 합니다.

검증 범위는 정적 소스 조사뿐입니다. 파일 수정, git 명령, 빌드·테스트는 실행하지 않았습니다. 금지 조건 때문에 실제 checkout branch는 독립 확인하지 않고 사용자가 지정한 `refactor/lane-ownership-concurrency`를 전제로 조사했습니다.

걸린 시간: **16분 10초**.
