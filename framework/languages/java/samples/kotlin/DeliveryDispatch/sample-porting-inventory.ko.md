# Kotlin DeliveryDispatch Sample Porting Inventory

이 문서는 `framework/doc/plan/framework-kotlin-sample-dotnet-porting-plan.ko.md`의 샘플 단위
절차에 따라 `.NET` DeliveryDispatch 샘플과 공통 샘플 문서를 Kotlin 구현에 매핑한다.

현재 Kotlin 루트는 `.NET` DeliveryDispatch와 공통 샘플 문서의 역할 배치, message 계약, 검증 marker를
따른다. client stream connector, entry spot, actor, dispatch worker, tracking fanout source는 compile
검증과 standalone runtime 검증을 통과했다. `run_sample.sh`는 dynamic topology, 실제 framework role
process, stream runtime client, server evidence self-check를 실행한다.

## 기준 파일 매핑

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `.NET: Client/DeliveryDispatch.Client.csproj` | `Client/build.gradle.kts` | client-project | done | Kotlin standalone Gradle Client project |
| `.NET: Client/DeliveryDispatchClientScenario.cs` | `Client/src/main/kotlin/.../client/Program.kt` | client-scenario | done | stream connector wait API로 성공 배차, timeout 재배정, server evidence를 검증한다. standalone runtime proof 통과. |
| `.NET: Client/Program.cs` | `Client/src/main/kotlin/.../client/Program.kt` | client-entrypoint | done | Client role entrypoint |
| `.NET: DeliveryDispatch.sln` | `standalone.settings.gradle.kts` | build-root | done | Kotlin standalone multi-project |
| 공통 DeliveryDispatch 문서 | `framework/doc/framework/common/sample/deliverydispatch/README.ko.md` | common-sample | done | 공통 문서가 시나리오, DTO와 검증 기준을 소유하며 Kotlin 전용 계약은 두지 않는다. |
| `.NET: Server/Configuration/DeliveryDispatch.Server.Configuration.csproj` | `Server/Configuration/build.gradle.kts` | server-config-project | done | shared server configuration project |
| `.NET: Server/Configuration/EvidenceStore.cs` | `Server/Configuration/src/main/kotlin/.../DeliveryEvidenceStore.kt` | server-evidence | done | Tracking role이 delivery별 status evidence를 파일에 기록한다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | `Server/Configuration/src/main/kotlin/.../SampleFlowLog.kt` | logging | done | Dispatch, Tracking, CustomerGateway role log에 flow evidence를 남긴다. |
| `.NET: Server/Configuration/SampleNames.cs` | `Server/Configuration/src/main/kotlin/.../SampleNames.kt` | configuration | done | channel, mesh, role 이름과 검증 marker 상수를 둔다. |
| `.NET: Server/Configuration/SampleTopology.cs` | `Server/Configuration/src/main/kotlin/.../SampleTopology.kt` + `run_sample.sh` | topology | done | runner가 배정한 role URL을 Kotlin topology 형태와 같은 key로 전달한다. |
| `.NET: Server/CourierActorNode/ActorDirectory.cs` | `Server/CourierSpotNode/src/main/kotlin/.../ActorDirectory.kt` | courier-actor-directory | done | actor id와 courier actor instance mapping을 관리한다. |
| `.NET: Server/CourierActorNode/CourierActor.cs` | `Server/CourierSpotNode/src/main/kotlin/.../CourierActor.kt` | courier-actor | done | bound session으로 offer를 push하고 courier decision timeout을 처리한다. |
| `.NET: Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj` | `Server/CourierSpotNode/build.gradle.kts` | server-role-project | done | Kotlin role 이름은 공통 문서의 Courier spot server node 의미를 따른다. |
| `.NET: Server/CourierActorNode/NodeHostFactory.cs` | `Server/CourierSpotNode/src/main/kotlin/.../CourierSpotNodeApplication.kt` | framework-host | done | courier actor spot mesh, entry spot, actor factory를 public framework API로 구성한다. |
| `.NET: Server/CourierActorNode/Program.cs` | `Server/CourierSpotNode/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real ZLink spot node host entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/CourierActorNode/RouteHandlers.cs` | `Server/CourierSpotNode/src/main/kotlin/.../handlers/*RouteHandler.kt` | spot-request-handler | done | `FindCourierActorReq`·`EnsureCourierActorReq`는 Spot request handler, `OfferDeliveryMsg`는 **one-way Spot packet handler**다(공통 sample spec §7.4). |
| `.NET: Server/CourierActorNode/Spots/EntrySpot/EntrySpot.cs` | `Server/CourierSpotNode/src/main/kotlin/.../spots/CourierEntrySpot.kt` | entry-spot | done | courier actor entry spot과 actor registration을 구현했다. |
| `.NET: Server/CourierActorNode/Spots/EntrySpot/Handlers/BindCourierSessionActorHandler.cs` | `Server/CourierSpotNode/src/main/kotlin/.../spots/handlers/BindCourierSessionActorHandler.kt` | actor-bind-handler | done | bound session join response를 actor handler로 반환한다. |
| `.NET: Server/CourierActorNode/Spots/EntrySpot/Handlers/CourierDecisionActorHandler.cs` | `Server/CourierSpotNode/src/main/kotlin/.../spots/handlers/CourierDecisionActorHandler.kt` | actor-request-handler | done | courier decision send를 actor가 응답을 기다리는 offer에 연결한다. |
| `.NET: Server/CourierGateway/CourierDirectory.cs` | `Server/CourierGateway/src/main/kotlin/.../CourierDirectory.kt` | courier-directory | done | courier id -> actor node/session route mapping을 public handler에서 사용한다. |
| `.NET: Server/CourierGateway/CourierGatewayHandlers.cs` | `Server/CourierGateway/src/main/kotlin/.../handlers/*Handler.kt` | channel-handler | done | `BindCourierReq` handler가 `requestToSpot`으로 courier actor node entry spot에 요청한다. offer는 gateway를 거치지 않는다. |
| `.NET: Server/CourierGateway/CourierGatewayHostFactory.cs` | `Server/CourierGateway/src/main/kotlin/.../CourierGatewayApplication.kt` | framework-host | done | courier channel server와 courier actor spot mesh client를 public framework API로 구성한다. |
| `.NET: Server/CourierGateway/DeliveryDispatch.Server.CourierGateway.csproj` | `Server/CourierGateway/build.gradle.kts` | server-role-project | done | distinct role project |
| `.NET: Server/CourierGateway/Program.cs` | `Server/CourierGateway/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real ZLink channel server entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/CourierSession/BindCourierSessionHandler.cs` | `Server/CourierSession/src/main/kotlin/.../sessions/CourierSession.kt` | stream-handler | done | courier stream bind request를 courier gateway channel과 actor binding으로 연결한다. |
| `.NET: Server/CourierSession/CourierSession.cs` | `Server/CourierSession/src/main/kotlin/.../sessions/CourierSession.kt` | stream-session | done | courier decision packet을 bound actor로 relay한다. |
| `.NET: Server/CourierSession/CourierSessionHostFactory.cs` | `Server/CourierSession/src/main/kotlin/.../CourierSessionApplication.kt` | framework-host | done | courier stream node와 courier channel client를 public framework API로 구성한다. |
| `.NET: Server/CourierSession/DeliveryDispatch.Server.CourierSession.csproj` | `Server/CourierSession/build.gradle.kts` | server-role-project | done | distinct role project |
| `.NET: Server/CourierSession/Program.cs` | `Server/CourierSession/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real stream server entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/CustomerGateway/CustomerActor.cs` | `Server/CustomerGateway/src/main/kotlin/.../CustomerActor.kt` | customer-actor | done | bound session으로 `DeliveryStatusNotify`를 push한다. |
| `.NET: Server/CustomerGateway/CustomerActorDirectory.cs` | `Server/CustomerGateway/src/main/kotlin/.../CustomerActorDirectory.kt` | customer-directory | done | customer actor와 delivery subscription mapping을 관리한다. |
| `.NET: Server/CustomerGateway/CustomerGatewayHandlers.cs` | `Server/CustomerGateway/src/main/kotlin/.../handlers/*Handler.kt` | stream-handler | done | `EnsureCustomerActorReq`와 customer status push handler를 추가했다. |
| `.NET: Server/CustomerGateway/CustomerGatewayHostFactory.cs` | `Server/CustomerGateway/src/main/kotlin/.../CustomerGatewayApplication.kt` | framework-host | done | stream node, customer route channel, customer entry spot, actor factory를 public framework API로 구성한다. |
| `.NET: Server/CustomerGateway/CustomerSession.cs` | `Server/CustomerGateway/src/main/kotlin/.../sessions/CustomerSession.kt` | stream-session | done | customer stream session dispatch와 actor relay를 구현했다. |
| `.NET: Server/CustomerGateway/DeliveryDispatch.Server.CustomerGateway.csproj` | `Server/CustomerGateway/build.gradle.kts` | server-role-project | done | distinct role project |
| `.NET: Server/CustomerGateway/Program.cs` | `Server/CustomerGateway/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real ZLink stream server entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/CustomerGateway/Spots/EntrySpot/CustomerEntrySpot.cs` | `Server/CustomerGateway/src/main/kotlin/.../spots/CustomerEntrySpot.kt` | entry-spot | done | customer actor entry spot과 actor registration을 구현했다. |
| `.NET: Server/CustomerGateway/Spots/EntrySpot/Handlers/SubscribeDeliveryActorHandler.cs` | `Server/CustomerGateway/src/main/kotlin/.../spots/handlers/SubscribeDeliveryActorHandler.kt` | actor-handler | done | actor subscription request를 entry spot으로 연결한다. |
| `.NET: Server/CustomerGateway/SubscribeDeliverySessionHandler.cs` | `Server/CustomerGateway/src/main/kotlin/.../sessions/handlers/SubscribeDeliverySessionHandler.kt` | session-handler | done | stream subscription 요청에서 customer actor를 보장하고 session에 bind한다. |
| `.NET: Server/Dispatch/DeliveryDispatch.Server.Dispatch.csproj` | `Server/Dispatch/build.gradle.kts` | server-role-project | done | distinct Dispatch role |
| `.NET: Server/Dispatch/DispatchServerHostFactory.cs` | `Server/Dispatch/src/main/kotlin/.../DispatchServerApplication.kt` | framework-host | done | HTTP + courier/tracking channel client config를 public framework API로 구성한다. |
| `.NET: Server/Dispatch/DispatchWorkQueue.cs` | `Server/Dispatch/src/main/kotlin/.../DispatchWorkQueue.kt` | work-queue | done | in-process dispatch queue를 별도 worker에 연결한다. |
| `.NET: Server/Dispatch/DispatchWorker.cs` | `Server/Dispatch/src/main/kotlin/.../DispatchWorker.kt` | dispatch-worker | done | courier channel offer, timeout reassignment, tracking status publish를 구현했다. |
| `.NET: Server/Dispatch/Program.cs` | `Server/Dispatch/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real ZLink Dispatch server entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/Registry/DeliveryDispatch.Server.Registry.csproj` | `Server/Registry/build.gradle.kts` | server-role-project | done | distinct Registry role |
| `.NET: Server/Registry/Program.cs` | `Server/Registry/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real embedded registry entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/Registry/RegistryHostFactory.cs` | `Server/Registry/src/main/kotlin/.../RegistryApplication.kt` | registry-host | done | embedded registry pub/router endpoint 설정을 public framework API로 구성한다. |
| `.NET: Server/Tracking/DeliveryDispatch.Server.Tracking.csproj` | `Server/Tracking/build.gradle.kts` | server-role-project | done | distinct Tracking role |
| `.NET: Server/Tracking/Handlers.cs` | `Server/Tracking/src/main/kotlin/.../handlers/*Handler.kt` | tracking-handler | done | `DeliveryStatusChangedReq` evidence 저장, CustomerGateway forward, server assertion handler가 standalone runtime proof를 통과했다. |
| `.NET: Server/Tracking/Program.cs` | `Server/Tracking/src/main/kotlin/.../Program.kt` | server-entrypoint | done | real Tracking channel server entrypoint로 standalone runtime proof를 통과했다. |
| `.NET: Server/Tracking/TrackingServerHostFactory.cs` | `Server/Tracking/src/main/kotlin/.../TrackingServerApplication.kt` | framework-host | done | Tracking channel server와 evidence store bean을 public framework API로 구성한다. |
| `.NET: Shared/Contracts/Messages.cs` | `Shared/src/main/kotlin/.../shared/contracts/Messages.kt` | shared-contract | done | 공통 message 이름과 필드를 Kotlin data class/enum으로 대응 |
| `.NET: Shared/DeliveryDispatch.Shared.csproj` | `Shared/build.gradle.kts` | shared-project | done | Kotlin shared contracts project |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | dynamic topology, installed app role startup, stream runtime client, marker grep이 standalone runtime proof를 통과했다. |

## 공통 message 계약 매핑

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `common/.NET: CreateDeliveryReq` | `CreateDeliveryReq` | shared-contract | done | `deliveryId`, `customerId`, `pickupAddress`, `dropoffAddress` 대응 |
| `common/.NET: CreateDeliveryRes` | `CreateDeliveryRes` | shared-contract | done | `deliveryId` 대응 |
| `common/.NET: SubscribeDeliveryReq` | `SubscribeDeliveryReq` | shared-contract | done | `deliveryId` 대응 |
| `common/.NET: SubscribeDeliveryRes` | `SubscribeDeliveryRes` | shared-contract | done | `deliveryId` 대응 |
| `common/.NET: DeliveryStatusNotify` | `DeliveryStatusNotify` | shared-contract | done | `deliveryId`, `status`, `courierId`, `occurredAt` 대응 |
| `common: AssignDeliveryMsg` | `AssignDeliveryMsg` | shared-contract | done | one-way send 이름과 `deliveryId`, `customerId`, `pickupAddress`, `dropoffAddress`를 공통 spec에 맞춘다. |
| `common/.NET: BindCourierSessionReq` | `BindCourierSessionReq` | shared-contract | done | client 요청은 `courierId`, actor relay는 `actor`, `sessionRoute`를 채운다. |
| `common/.NET: BindCourierSessionRes` | `BindCourierSessionRes` | shared-contract | done | `courierId`, `actor`, `sessionRoute` 대응 |
| `common/.NET: BindCourierReq` | `BindCourierReq` | shared-contract | done | `courierId`, `sessionRoute` 대응 |
| `common/.NET: BindCourierRes` | `BindCourierRes` | shared-contract | done | `courierId`, `actor`, `sessionRoute` 대응 |
| `common/.NET: EnsureCourierActorReq` | `EnsureCourierActorReq` | shared-contract | done | `courierId` 대응 |
| `common/.NET: EnsureCourierActorRes` | `EnsureCourierActorRes` | shared-contract | done | `courierId`, `actor` 대응 |
| `common/.NET: OfferDeliveryMsg` | `OfferDeliveryMsg` | shared-contract | done | `courierId`, `deliveryId`, `attempt`, `pickupAddress`, `dropoffAddress` 대응 (one-way) |
| `common/.NET: OfferDeliveryNotify` | `OfferDeliveryNotify` | shared-contract | done | `deliveryId`, `courierId`, `pickupAddress`, `dropoffAddress` 대응 |
| `common/.NET: OfferDeliveryResultMsg` | `OfferDeliveryResultMsg` | shared-contract | done | `deliveryId`, `courierId`, `attempt`, `accepted`, `reason` 대응 (one-way) |
| `.NET: CourierDecisionMsg` | `CourierDecisionMsg` | shared-contract | done | courier stream send 메시지로 `deliveryId`, `courierId`, `accepted`, `reason` 대응 |
| `.NET: ReassignDelivery` | 없음 | shared-contract | not-needed | 공통 문서에 없는 미사용 `.NET` record다. 재배정은 `DeliveryStatus.Reassigned`와 `DispatchWorker` 흐름으로 검증한다. |
| `common/.NET: DeliveryStatusChangedReq` | `DeliveryStatusChangedReq` | shared-contract | done | `deliveryId`, `status`, `courierId`, `occurredAt` 대응 |
| `common/.NET: DeliveryStatusChangedRes` | `DeliveryStatusChangedRes` | shared-contract | done | `deliveryId`, `status` 대응 |
| `common/.NET: EnsureCustomerActorReq` | `EnsureCustomerActorReq` | shared-contract | done | `customerId` 대응 |
| `common/.NET: EnsureCustomerActorRes` | `EnsureCustomerActorRes` | shared-contract | done | `customerId`, `actor` 대응 |
| `.NET: ServerAssertionReq` | `ServerAssertionReq` | validation-contract | done | server evidence self-check 입력으로 `successfulDeliveryId`, `reassignedDeliveryId` 대응 |
| `.NET: ServerAssertionRes` | `ServerAssertionRes` | validation-contract | done | server evidence self-check 결과로 `passed`, `evidence` 대응 |
| `common/.NET: DeliveryStatus` | `DeliveryStatus` | shared-contract | done | `Assigned`, `Reassigned`, `Accepted`, `PickedUp`, `Delivered` 포함 |
| `common/.NET: courier actor ref` | `ActorRefWire` | shared-contract | done | courier actor `nodeRid`, `actorId`, `generation`을 wire-safe DTO로 전달한 뒤 session bind 직전에 framework actor ref로 재구성한다. |

## 공통 요구 매핑

| 기준 | Kotlin 대응 | 분류 | 상태 | 비고 |
|------|-------------|------|------|------|
| `common: Registry role` | `Server/Registry` | server-role | done | registry host가 standalone runtime proof를 통과했다. |
| `common: Dispatch role` | `Server/Dispatch` | server-role | done | HTTP API, dispatch channel, worker가 standalone runtime proof를 통과했다. |
| `common: CourierGateway role` | `Server/CourierGateway` | server-role | done | courier channel server와 directory가 standalone runtime proof를 통과했다. |
| `common: CourierSession role` | `Server/CourierSession` | server-role | done | stream server와 courier session이 standalone runtime proof를 통과했다. |
| `common: CourierSpotNode1/2 roles` | `Server/CourierSpotNode` 두 process | server-role | done | actor/entry spot/spot mesh가 standalone runtime proof를 통과했다. |
| `common: Tracking role` | `Server/Tracking` | server-role | done | tracking channel/evidence, CustomerGateway forward, assertion handler가 standalone runtime proof를 통과했다. |
| `common: CustomerGateway role` | `Server/CustomerGateway` | server-role | done | stream server, entry spot, customer actor가 standalone runtime proof를 통과했다. |
| `common: Probe readiness` | `run_sample.sh` bound endpoint probes | validation | done | startup log가 아니라 registry/channel/stream/HTTP endpoint port readiness를 확인한다. |
| `common: delivery-success status order` | `Client/.../Program.kt` + `Tracking` evidence | validation | done | stream connector wait API로 Assigned, Accepted, PickedUp, Delivered push를 검증했다. |
| `common: delivery-reassign status order` | `Client/.../Program.kt` + `Tracking` evidence | validation | done | courier-a timeout 후 courier-b Reassigned, Accepted, Delivered push를 검증했다. |
| `common: server evidence check` | `Client/.../Program.kt` + `DeliveryEvidenceStore.kt` | validation | done | Tracking evidence endpoint에서 두 delivery의 status sequence를 읽어 검증한다. |
| `common: topology=ready` | `run_sample.sh` bound endpoint probes | validation | done | registry/channel/stream/HTTP endpoint readiness 확인 후 출력 |
| `common: deliverydispatch-reassignment=completed` | `Client/.../Program.kt` | validation | done | 실제 worker/courier timeout 재배정 proof로 marker를 출력한다. |
| `common: deliverydispatch-server-evidence=completed` | `Client/.../Program.kt` | validation | done | Tracking evidence store를 읽어 marker를 출력한다. |
| `common: deliverydispatch=completed` | `Client/.../Program.kt` | validation | done | standalone framework runtime proof로 marker를 출력한다. |

## 남은 gap

- 없음. Standalone `run_sample.sh`에서 `topology=ready`, `deliverydispatch-reassignment=completed`,
  `deliverydispatch-server-evidence=completed`, `deliverydispatch=completed` marker를 확인했다.
