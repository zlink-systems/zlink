# D-095 — .NET 경과 시간 측정 결과

.NET runtime의 local wait·retry·retention·timer 진행을 monotonic 시간으로 계산한다.
Readiness의 `min(request timeout, 5초)`, liveness의 5초/15초, terminal retention과 retry window의
설정값은 유지한다. Request 경로도 공통 readiness cap을 적용한다.

- 소유 계층: Framework의 각 wait·registry·lifecycle·timer owner. Core·binding의 transport 결정은 변경하지 않는다.
- Spec 조항: [channel messaging §3](../../../framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md)의 제한 대기(170–174행), [actor model](../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md)의 original deadline 뒤 5분 retention(666행), [transport liveness §2–3](../../../framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md)의 5초/15초, [Spot timer §1–2·4](../../../framework/doc/framework/common/spec/server/03-spot-actor/10-spot-timer.ko.md)의 논리 cursor·tick 계약. [D-095](decisions.ko.md)가 해당 경과 시간원 변경을 승인한다.
- 교차언어 대조: Java `ZLinkChannelSocketRegistry.java:268,274`는 readiness에 `System.nanoTime()`, 같은 파일 457–459·975·1106·1133행은 liveness에 nanoTime을 사용한다. Node readiness는 `fix-node-clientserver-readiness-cap-2` 병렬 작업 범위다. .NET만 UTC 계산을 수정하는 이유는 시간원 선택의 기존 결함이며 public contract 차이가 아니다.
- 변경 분류: **B — 기존 결함**, `decisions.ko.md` D-095의 감사·수정 승인 범위.

## 시간원 감사

범위는 `framework/languages/dotnet/src/**`다. 직접 `DateTime.UtcNow`·`DateTimeOffset.UtcNow`뿐 아니라
기존 주입 시계를 놓치지 않도록 `GetUtcNow()`도 조사했다. 아래 표는 수정 전 `47a8f4329b`의
**183개 occurrence / 53개 파일**을 분류한다. `file:line`은 원인 추적을 위한 수정 전 행 번호다.
경로 기준은 `framework/languages/dotnet/src/`이며 같은 분류의 행은 묶었다.

`duration`은 변경한 local 경과 시간이다. `timestamp`는 보고·wire·authoritative store 시각이어서
유지한다. `경계 변환`은 수신한 절대 deadline을 처음 상대 timeout/CTS로 바꾸거나 wire 유효성을
검사하는 지점이다. 이 경계의 UTC는 유지하며 반복 retry·retention 계산에는 사용하지 않는다.

| file:line | class / 분류 | 변경·유지 근거 |
|---|---|---|
| `Systems.Zlink.Stream.Connector/Runtime/Protocol/ZlinkStreamFlowId.cs:15` | `ZlinkStreamFlowId` / timestamp — 유지 | UUID/flow ID에 기록하는 Unix timestamp이며 대기·만료 한도가 아니다. |
| `Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamInboundObserverDispatcher.cs:100` | `ZlinkStreamInboundObserverDispatcher` / timestamp — 유지 | observer에 보고하는 event timestamp. |
| `Zlink.Framework/Runtime/Actors/ZLinkActorHandoffAdmissions.cs:47,121,204,261,285,366,449,574` | `ZLinkActorHandoffAdmissions` / duration — 변경 | 기존 `_timeProvider`로 pending admission·prepared completion을 측정한다. |
| `Zlink.Framework/Runtime/Actors/ZLinkActorHandoffAdmissions.cs:190,246` | `ZLinkActorHandoffAdmissions` / 경계 변환 — UTC 유지 | 수신 wire/context 절대 deadline의 최초 변환·유효성 검사. 이후 상대 timer 또는 monotonic deadline이 대기를 제한한다. |
| `Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:231` | `ZLinkActorManagerService` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:452` | `ZLinkActorManagerService` / duration — 변경 | 생성 전체 timeout은 시작 timestamp에서 차감한다. Wire deadline 값은 유지한다. |
| `Zlink.Framework/Runtime/Actors/ZLinkActorRuntimeState.cs:784,791` | `ZLinkActorRuntimeState` / duration — 변경 | 기존 `_timeProvider`의 monotonic session tombstone TTL. |
| `Zlink.Framework/Runtime/Actors/ZLinkDeferredActorJoin.cs:124` | `ZLinkDeferredActorJoin` / timestamp — 유지 | wire·lifecycle context에 전달할 절대 deadline. 실제 defer budget은 이미 `_registeredTimestamp`/Stopwatch로 차감한다. |
| `Zlink.Framework/Runtime/Actors/ZLinkSessionActorBindingTable.cs:721,735` | `ZLinkSessionActorBindingTable` / duration — 변경 | 기존 `_timeProvider`의 monotonic binding tombstone TTL. |
| `Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs:184,196` | `ZLinkBackendStreamSocketWrapper` / duration — 변경 | 기존 bind retry window를 Stopwatch로 측정한다. Retry 정책은 변경하지 않는다. |
| `Zlink.Framework/Runtime/Channels/ZLinkAutomaticFanoutSubscriberRuntime.cs:246,346,404,418` | `ZLinkAutomaticFanoutSubscriberRuntime` / duration — 변경 | 기존 owner `_time`의 activity timestamp로 15초 liveness를 판정한다. |
| `Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:229,248,595,600,1223,1332,1390,1444` | `ZLinkClientServerClientRuntime` / duration — 변경 | 주입 가능한 BCL TimeProvider 하나를 readiness·request remaining·connection liveness가 공유한다. |
| `Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:105` | `ZLinkClientServerRuntimeService` / timestamp — 유지 | monitoring snapshot에 보고하는 관측 시각. |
| `Zlink.Framework/Runtime/Channels/ZLinkClientServerServerIdentity.cs:104,130,144` | `ZLinkClientServerServerIdentity` / duration — 변경 | Stopwatch로 기존 probe 5초·peer deadline 15초를 측정한다. |
| `Zlink.Framework/Runtime/Execution/ZLinkWorkerPool.cs:206,214,242` | `ZLinkWorkerPool` / duration — 변경 | Stopwatch로 direct-submit capacity 대기 한도를 계산한다. |
| `Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs:97,163,204,982` | `ZLinkActorRemoteJoiner` / duration — 변경 | 입력 UTC는 wire용으로 보존하고, private Join 단계들은 동일한 monotonic deadline을 전달한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkActorFacade.cs:55,156` | `ZLinkFrameworkActorFacade` / duration — 변경 | UTC 입력을 호출 진입 시 변환하고 동일한 경과 시간 한도로 후속 대기를 계산한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs:101` | `ZLinkFrameworkDrainExecutor` / timestamp — 유지 | relocation·closing context의 공개 절대 deadline. Drain 자체는 기존 deadlineToken이 제한한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkMaintenanceRuntime.cs:104,149,228,269,603,695` | `ZLinkFrameworkMaintenanceRuntime` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkMaintenanceRuntime.cs:411,481,663` | `ZLinkFrameworkMaintenanceRuntime` / duration — 변경 | 상태의 UTC deadline과 내부 relocation·shutdown 경과 시간 한도를 구분한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntime.cs:274` | `ZLinkFrameworkRuntime` / timestamp — 유지 | closing callback context용 절대 deadline. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActorJoinPrewarm.cs:108` | `ZLinkFrameworkRuntimeActorJoinPrewarm` / duration — 변경 | 외부 admission deadline을 진입 시 한 번 변환하고 prewarm 대기는 Stopwatch로 계산한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:2194,2419` | `ZLinkFrameworkRuntimeActors` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:4583,5362` | `ZLinkFrameworkRuntimeActors` / 경계 변환 — UTC 유지 | 수신 wire/context 절대 deadline의 최초 변환·유효성 검사. 이후 상대 timer 또는 monotonic deadline이 대기를 제한한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:5406,5415` | `ZLinkFrameworkRuntimeActors` / duration — 변경 | session bind retry의 고정 한도는 Stopwatch로 측정한다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeLocationAccess.cs:123` | `ZLinkFrameworkRuntimeLocationAccess` / duration — 변경 | Instance Spot authority transition의 대기 한도는 caller가 전달한 monotonic 값이다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeLocationAccess.cs:168` | `ZLinkFrameworkRuntimeLocationAccess` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeSpotRetire.cs:159` | `ZLinkFrameworkRuntimeSpotRetire` / duration — 변경 | Target stage의 local retention은 Stopwatch로 시작한다. |
| `Zlink.Framework/Runtime/Host/ZLinkRouteMeshRuntimeService.cs:80,435,507,829` | `ZLinkRouteMeshRuntimeService` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Host/ZLinkRouteMeshRuntimeService.cs:911,913` | `ZLinkRouteMeshRuntimeService` / duration — 변경 | descriptor 재조회 간격 100ms를 Stopwatch로 측정한다. |
| `Zlink.Framework/Runtime/Host/ZLinkStandaloneActorRelocationRuntime.cs:644,1335,3035,3081` | `ZLinkStandaloneActorRelocationRuntime` / duration — 변경 | 입력 deadline은 진입 시 변환한다. Target stage·abort TTL도 Stopwatch로 측정한다. |
| `Zlink.Framework/Runtime/Locations/ZLinkFanoutDiscovery.cs:229,297,344` | `ZLinkFanoutDiscovery` / timestamp — 유지 | descriptor 발견·관측 timestamp이며 peer lease 비교 값과 함께 외부에 전달한다. |
| `Zlink.Framework/Runtime/Locations/ZLinkFanoutRuntimeService.cs:135,248,331` | `ZLinkFanoutRuntimeService` / timestamp — 유지 | monitoring snapshot·event timestamp. |
| `Zlink.Framework/Runtime/Locations/ZLinkInMemoryLocationStore.Authority.cs:36,37,54,260,274,304,424,477,597,682,781` | `ZLinkInMemoryLocationStore.Authority` / timestamp — 유지 | authoritative StoreNow·lease/reservation·scan cursor의 store 시각이다. Store가 내보낸 절대 시각과 같은 기준으로 비교한다. |
| `Zlink.Framework/Runtime/Locations/ZLinkInMemoryLocationStore.cs:56,185,291,736,773,797,821,838` | `ZLinkInMemoryLocationStore` / timestamp — 유지 | authoritative store의 lease·reservation·상태 timestamp. Application 경과 시간과 혼합하지 않는다. |
| `Zlink.Framework/Runtime/Locations/ZLinkInMemoryProviderLocationStore.cs:33,50,123` | `ZLinkInMemoryProviderLocationStore` / timestamp — 유지 | provider의 authoritative StoreNow와 TTL 만료 시각은 같은 store clock을 사용한다. |
| `Zlink.Framework/Runtime/Locations/ZLinkLocationRuntime.cs:398` | `ZLinkLocationRuntime` / timestamp — 유지 | Store에 게시하는 UpdatedAt timestamp. |
| `Zlink.Framework/Runtime/Locations/ZLinkLocationStoreHealth.cs:40,50` | `ZLinkLocationStoreHealth` / timestamp — 유지 | 관측용 마지막 성공·실패 timestamp. |
| `Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:389,423,546,733,745,909,1043,1313,1405,1420,1634,4064,4421` | `ZLinkProviderLocationRepository.Authority` / duration — 변경 | CounterRetryWindow와 backoff remaining을 같은 Stopwatch 시간으로 계산한다. |
| `Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:4601` | `ZLinkProviderLocationRepository.Authority` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Messaging/ZLinkClientCallCodec.cs:31` | `ZLinkClientCallCodec` / timestamp — 유지 | 프로세스 사이에서 전달하는 envelope deadline의 Unix timestamp. |
| `Zlink.Framework/Runtime/Service/ZLinkDeadlineClock.cs:17` | `ZLinkDeadlineClock` / 경계 변환 — 유지 | wire deadline을 local 시간으로 변환하기 위한 기존 UTC 기준 초기화다. |
| `Zlink.Framework/Runtime/Service/ZLinkDeadlineClock.cs:25` | `ZLinkDeadlineClock` / duration — 변경 | 기존 UTC high-water 판정은 wire admission 경계에 유지한다. Admission 때 고정한 local deadline의 이후 실행·retention은 TimeProvider timestamp만 사용한다. |
| `Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:1332,1421,3756,3811,4547,9626` | `ZLinkManagedMeshNode` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4258,4363,6737,6885` | `ZLinkManagedMeshNode` / 경계 변환 — UTC 유지 | 수신 wire/context 절대 deadline의 최초 변환·유효성 검사. 이후 상대 timer 또는 monotonic deadline이 대기를 제한한다. |
| `Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4583,4734,4827` | `ZLinkManagedMeshNode` / duration — 변경 | reply terminal TTL·native terminal 재제출·원래 deadline 뒤 operation retention은 기존 deadline clock을 공유한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkActorMessageFollower.cs:396,876` | `ZLinkActorMessageFollower` / duration — 변경 | 수신 wire deadline은 pending reply 등록 시 한 번 변환한다. Expiry와 반복 remaining은 같은 deadline을 사용한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkDirectReplyCompletionRegistry.cs:36,64` | `ZLinkDirectReplyCompletionRegistry` / duration — 변경 | BCL TimeProvider timestamp에 terminal을 기록하고 elapsed >= retention에서 재등록을 허용한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkEntrySpotActivation.cs:340` | `ZLinkEntrySpotActivation` / timestamp — 유지 | closing callback context의 절대 deadline. |
| `Zlink.Framework/Runtime/Spots/ZLinkInstanceSpotActivationTarget.cs:844,932` | `ZLinkInstanceSpotActivationTarget` / duration — 변경 | 수신 wire deadline을 JoinExisting 진입 시 변환한다. Forwarding과 authority retry는 같은 deadline을 사용한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotActivationActors.cs:627,646` | `ZLinkSpotActivationActors` / duration — 변경 | post-commit callback 진입 시 UTC를 변환하고 반복 callback 대기는 Stopwatch로 계산한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotActivationExecution.cs:664` | `ZLinkSpotActivationExecution` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotActivationExecution.cs:1373,1395` | `ZLinkSpotActivationExecution` / duration — 변경 | Message Follow 등록·route 만료 판정은 Stopwatch로 통일한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotActivationExecution.cs:1473,1751` | `ZLinkSpotActivationExecution` / 경계 변환 — UTC 유지 | 수신 wire/context 절대 deadline의 최초 변환·유효성 검사. 이후 상대 timer 또는 monotonic deadline이 대기를 제한한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotClientCalls.cs:259,380` | `ZLinkSpotClientCalls` / duration — 변경 | Instance Spot request와 authority transition이 동일한 monotonic deadline을 공유한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotClosingInvocation.cs:11` | `ZLinkSpotClosingInvocation` / 경계 변환 — UTC 유지 | 공개 closing context의 절대 deadline을 callback 진입 시 상대 timeout으로 1회 변환한다. 이후 CTS가 제한한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotMessageFollow.cs:96` | `ZLinkSpotMessageFollow` / duration — 변경 | local Message Follow retention·drain 한도를 Stopwatch로 계산한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotNodeCatalog.cs:232,427,1531,1588` | `ZLinkSpotNodeCatalog` / timestamp — 유지 | close·lifecycle context로 전달하는 절대 deadline. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotNodeRuntime.cs:293,373,940` | `ZLinkSpotNodeRuntime` / 경계 변환 — UTC 유지 | 수신 UserSpot/Actor wire deadline을 target callback 진입 시 상대 timeout으로 변환한다. 940은 relocation context timestamp다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotRetireScheduler.cs:169` | `ZLinkSpotRetireScheduler` / 경계 변환 — UTC 유지 | 외부 relocation deadline을 CTS로 1회 변환한다. 이후 기존 deadline token으로 진행한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotRetireTransport.cs:359,395,2321,2447` | `ZLinkSpotRetireTransport` / duration — 변경 | authority 대기와 target stage·tombstone retention은 Stopwatch로 계산한다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotRuntimeManager.cs:153,357,540,581,665,707,740` | `ZLinkSpotRuntimeManager` / duration — 변경 | create·join-existing·close의 경과 시간 한도는 Stopwatch, wire에는 기존 UTC deadline을 보낸다. |
| `Zlink.Framework/Runtime/Spots/ZLinkSpotTimerRegistry.cs:76` | `ZLinkSpotTimerRegistry` / timestamp — 유지 | relocation 가능한 논리 timer snapshot의 시작 UTC timestamp. |
| `Zlink.Framework/Runtime/Streams/ZLinkSessionActorCoordinator.cs:457,528` | `ZLinkSessionActorCoordinator` / duration — 변경 | session bind retry의 경과 시간 한도를 Stopwatch로 계산한다. |
| `Zlink.Framework/Runtime/Timers/ZLinkTimer.cs:49,175` | `ZLinkTimer` / timestamp — 유지 | 외부에 전달하는 wire/context·관측 timestamp다. Local 경과 시간 계산에는 사용하지 않는다. |
| `Zlink.Framework/Runtime/Timers/ZLinkTimer.cs:388,418` | `ZLinkTimer` / duration — 변경 | scheduler의 기존 논리 cursor를 monotonic queue 시간으로 연결한다. Import된 UTC snapshot은 한 번 변환한다. |
| `Zlink.Framework/Runtime/Timers/ZLinkTimerScheduler.cs:157` | `ZLinkTimerScheduler` / duration — 변경 | queue due time은 monotonic ticks이며 timer들이 scheduler의 TimeProvider 하나를 공유한다. |

추가로 `ZLinkManagedMeshNode`가 호출하던 `ZLinkDeadlineClock.GetUnixTimeMilliseconds()`도
추적했다. 수신 operation은 동일한 local deadline으로 실행과 retention을 계산한다.
Retention 기준은 **완료 시각이 아니라 original deadline + 기존 retention**이다. Direct-reply
completion registry의 dedup retention은 기존 계약대로 terminal 등록 시점부터 측정한다.

새 UTC 읽기는 외부 deadline/snapshot의 최초 변환과 관측 timestamp에 한정한다. 다른 process나
Store에서 받은 Unix timestamp를 Stopwatch ticks와 직접 비교하지 않는다. 기존
`ZLinkDeadlineClock`의 high-water UTC와 state lane은 **wire admission 경계에 유지**한다.
Operation admission 때 deadline을 monotonic으로 확정하므로 이후 UTC 전진이 진행 중인
operation의 실행 한도·retention을 줄이지 않는다. Clock 전진 후 새로 생성한 wire deadline은
현재 high-water UTC에서 변환해 원래 상대 한도를 유지한다.

## 소유자별 규칙 수

비교 기준은 **경과 시간을 판정하는 시간원 규칙**이다. Public UTC timestamp 표기는 별도 사실이며
규칙 수에 포함하지 않는다. 표의 `2→1`은 UTC subtraction/comparison과 상대 timer의 혼합을
monotonic 판정 하나로 수렴한 경우다. 순수 TTL registry의 `1→1`은 시간원 교체이며 예외를 추가하지 않는다.

| owner | 수정 전/후 규칙 수 |
|---|---|
| `ZLinkActorHandoffAdmissions` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkActorManagerService` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkActorRuntimeState` | 1→1 (UTC TTL → monotonic TTL) |
| `ZLinkSessionActorBindingTable` | 1→1 (UTC TTL → monotonic TTL) |
| `ZLinkBackendStreamSocketWrapper` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkAutomaticFanoutSubscriberRuntime` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkClientServerClientRuntime` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkClientServerServerIdentity` | 1→1 (UTC TTL → monotonic TTL) |
| `ZLinkWorkerPool` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkActorRemoteJoiner` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkFrameworkActorFacade` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkFrameworkMaintenanceRuntime` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkFrameworkRuntimeActorJoinPrewarm` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkFrameworkRuntimeActors` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkFrameworkRuntimeLocationAccess` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkFrameworkRuntimeSpotRetire` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkRouteMeshRuntimeService` | 1→1 (UTC TTL → monotonic TTL) |
| `ZLinkStandaloneActorRelocationRuntime` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkProviderLocationRepository.Authority` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkDeadlineClock` | 경과 시간 2→1; 기존 wire admission high-water 규칙은 유지 |
| `ZLinkManagedMeshNode` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkActorMessageFollower` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkDirectReplyCompletionRegistry` | 1→1 (UTC TTL → monotonic TTL) |
| `ZLinkInstanceSpotActivationTarget` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSpotActivationActors` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSpotActivationExecution` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSpotClientCalls` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSpotMessageFollow` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSpotRetireTransport` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSpotRuntimeManager` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkSessionActorCoordinator` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkTimer` | 2→1 (경과 시간 판정은 monotonic 하나) |
| `ZLinkTimerScheduler` | 2→1 (경과 시간 판정은 monotonic 하나) |

대안으로 모든 owner에 UTC 점프 보정·high-water 규칙을 넣는 방법과 기존 `TimeProvider`/Stopwatch로
경과 시간을 계산하는 방법을 비교했다. 후자는 wall-clock 예외·보정 상태가 필요 없으므로 선택했다.
새 public API나 별도 clock abstraction은 추가하지 않았다. Private Join tuple은 동일 operation의
wire UTC와 local monotonic deadline을 전달하며 기존 `ZLinkDeadlineClock`은 해당 owner에서 재사용한다.

## 변경 파일과 검증

변경 파일은 위 표의 duration owner 구현과 이를 연결하는 `ZLinkFanoutLivenessProtocol`,
회귀 테스트 `ClientServerChannelRuntimeTests`, `DirectReplyCompletionRegistryTests`,
`TimerLifecycleTests`, 내부 시간 인자를 맞춘 `FanoutAutomaticDiscoveryTests`,
`SessionActorCoordinatorTests`, `RelocationRuntimeTests`다. 정확한 diff 범위는
`git diff --stat -- framework/languages/dotnet`로 확인한다.

| 검증 | 결과 | 로그 |
|---|---|---|
| Named owners 첫 회귀 | 8 passed / 0 failed | `/tmp/zlink-d095/first-focused.log` |
| Wall jump·original deadline retention·timer cursor | 12 passed / 0 failed | `/tmp/zlink-d095/clock-focused.log` |
| Touched owners focused | 504 passed / 0 failed / 0 skipped | `/tmp/zlink-d095/owners-focused.log` |
| Direct reply·Instance Spot·Join boundary focused | 43 passed / 0 failed / 0 skipped | `/tmp/zlink-d095/boundary-focused.log` |
| Review 보완: admission 이후 jump·전진 후 신규 deadline·rollback replay | 17 passed / 0 failed | `/tmp/zlink-d095/review-focused.log` |
| 지정된 unit half 1회 | 1966 passed / 0 failed / 0 skipped | `/tmp/zlink-d095/unit-half.log` |

Fake time 테스트는 UTC ±5초 이동과 monotonic 진행을 독립적으로 조절한다. Registry는 retention
직전 재등록을 거부하고 정확한 만료 시점에 허용한다. Readiness는 request timeout 2초와 30초에
각각 2초/5초 cap을 확인한다. Timer는 UTC가 이동해도 500ms elapsed·5번째 scheduled index를
유지하고 실제 `StartedAt`은 UTC로 보고한다.

모든 .NET 실행은 `/tmp/zlink-d095/env.sh`와 `flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용했다.
`TMPDIR=/dev/shm/zlink-tmp-dotnet`, `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`,
`NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-b9a964ffb25a3bf8`,
`UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`, telemetry off다.
NuGet SHA256은 `b9a964ffb25a3bf8cb71607da5433faf9356c7e8b9b6edc52cea8c2ea80dd4b6`,
package 내부·Core·test output native SHA256은 모두
`f20f5cdba0bc117b17db7f8e9fec25b47d79de29bdfbff1e88ceaa8a032d2640`으로 확인했다.
Unit 명령은 `dotnet test .../Zlink.Framework.UnitTests.csproj -f net8.0 --no-build
--filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests'`이며 TRX는
`/tmp/zlink-d095/results/`에 보존한다. 전체 unit half는 반복하지 않는다.

## BLOCKERS

- 남은 테스트 실패는 없다. 지정된 unit half 1회는 1966/0이며, wire admission review 보완 전 snapshot의 결과다. 보완 후 focused 17/0으로 신규 forward-jump admission·기존 rollback replay·retention을 검증했다. 알려진 `DurableSenderPreservesExhaustionCauseAndOriginalOperation(14, withheld)` 간헐 실패는 이번 gate에서 재발하지 않았다.
- 기존 `ZLinkSpotNodeCatalog.cs:768` CS8619 warning은 요청 범위 밖이며 유지했다.
- 기존 wire admission은 rollback 뒤 만료된 replay를 거부하기 위해 UTC high-water를 유지한다.
  따라서 rollback 뒤 새로 생성한 deadline과 오래된 replay를 구분하는 동작은 기존 제약이다.
  이 작업은 wire 계약·replay assertion을 완화하지 않는다.
- Host wall clock 자체의 ±5초 이동은 이 작업에서 수정하지 않는다. Wire·authoritative Store의
  절대 시각 비교는 프로세스 사이의 시각 계약을 유지한다.
- Core·binding·다른 언어·보호된 spec 문서와 기존 사용자 변경은 수정하지 않았다. Commit하지 않았다.

문서 독립 검토는 원칙 준수·코드 부합 두 축으로 수행했다. 고정 epoch에서 신규 inbound deadline이
늘어나는 finding은 기존 wire admission을 보존하고 local retention을 분리해 수정했다.
감사 표 183개 위치 / 53개 파일의 누락·중복 없음도 확인했다.
