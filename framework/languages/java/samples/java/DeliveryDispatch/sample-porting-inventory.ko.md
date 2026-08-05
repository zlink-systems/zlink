# Java DeliveryDispatch sample porting inventory

이 문서는 `framework/doc/plan/framework-java-sample-dotnet-porting-plan.ko.md`의 샘플 단위 절차에 따라
`.NET` DeliveryDispatch 샘플과 공통 DeliveryDispatch 문서의 요구를 Java 샘플에 매핑한다.

## `.NET` 기준 파일 매핑

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: DeliveryDispatch.sln` | `standalone.settings.gradle.kts` | build | done | Shared, Client, Server/<Role> project를 포함한다. |
| `.NET: README.ko.md` | `README.ko.md` | doc | done | Java role 구조, runner, marker를 설명한다. |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | Redis location store를 준비하고 실행별 properties 파일을 만든 뒤 server role부터 Client까지 실제 process를 띄워 marker를 검증한다. |
| `.NET: Shared/DeliveryDispatch.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Shared contract project다. |
| `.NET: Shared/Contracts/Messages.cs` | `Shared/src/main/java/.../shared/contracts/Messages.java` | shared-contract | done | 공통 메시지를 Java record와 enum으로 대응했다. |
| `.NET: Client/DeliveryDispatch.Client.csproj` | `Client/build.gradle.kts` | build | done | Client application project다. |
| `.NET: Client/Program.cs` | `Client/src/main/java/.../client/Program.java` | client-entry | done | HTTP client와 stream connector를 만들고 scenario를 실행한다. |
| `.NET: Client/DeliveryDispatchClientScenario.cs` | `Client/src/main/java/.../client/DeliveryDispatchClientScenario.java` | client-scenario | done | 성공 배차, timeout 재배정, server evidence marker를 검증한다. |
| `.NET: Server/Configuration/*.cs` | `Server/Configuration/src/main/java/.../server/configuration/*.java` | server-support | done | `EvidenceStore`, `SampleFlowLog`, `SampleNames`, `SampleTopology`, `SampleTimings`로 공통 설정과 evidence를 둔다. `SampleTopology`는 `--config`로 받은 properties 파일을 시작 시 한 번 읽는다. |
| `.NET: location store bootstrap` | `Server/Configuration/src/main/java/.../server/configuration/SampleLocationStore.java` | server-config | done | role들이 공유하는 Redis location store extension을 생성한다. channel client와 courier Spot peer는 endpoint를 직접 연결하지 않고 framework가 location store에서 발견한다. |
| `.NET: Server/Tracking/*` | `Server/Tracking/src/main/java/.../server/tracking/*` | server-role | done | tracking channel, evidence 기록, customer push, server assertion을 처리한다. |
| `.NET: Server/CustomerGateway/*` | `Server/CustomerGateway/src/main/java/.../server/customergateway/*` | server-role | done | customer stream session, customer actor, entry spot, status push를 처리한다. |
| `.NET: Server/CourierSession/*` | `Server/CourierSession/src/main/java/.../server/couriersession/*` | server-role | done | courier stream session과 courier actor/session bind를 처리한다. |
| `.NET: Server/CourierActorNode/*` | `Server/CourierSpotNode/src/main/java/.../server/courierspotnode/*` | server-role | done | node 1/2 courier actor, entry spot, Spot request handler를 제공한다. Java project명은 spot 책임을 드러내도록 `CourierSpotNode`로 둔다. |
| `.NET: Server/Dispatch/*` | `Server/Dispatch/src/main/java/.../server/dispatch/*` | server-role | done | HTTP API, dispatch worker, courier offer, tracking event, self-check endpoint를 제공한다. |

## 공통 메시지 계약 매핑

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `CreateDeliveryReq` | `Messages.CreateDeliveryReq` | shared-contract | done | `deliveryId`, `customerId`, `pickupAddress`, `dropoffAddress`를 가진다. |
| `CreateDeliveryRes` | `Messages.CreateDeliveryRes` | shared-contract | done | 공통 spec과 같이 `deliveryId`만 반환한다. |
| `SubscribeDeliveryReq` | `Messages.SubscribeDeliveryReq` | shared-contract | done | customer stream subscription 요청이다. |
| `SubscribeDeliveryRes` | `Messages.SubscribeDeliveryRes` | shared-contract | done | subscription 수락 응답이다. |
| `DeliveryStatusNotify` | `Messages.DeliveryStatusNotify` | shared-contract | done | customer stream push payload다. |
| `AssignDeliveryMsg` | `Messages.AssignDeliveryMsg` | shared-contract | done | dispatch worker 입력 one-way send 메시지다. |
| `BindCourierSessionReq` | `Messages.BindCourierSessionReq` | shared-contract | done | CourierSession이 actor 위치와 session route를 채운 뒤 courier actor로 relay한다. |
| `BindCourierSessionRes` | `Messages.BindCourierSessionRes` | shared-contract | done | courier actor가 relay된 요청에 응답하고 원래 client 요청까지 결과를 전달한다. |
| `BindCourierReq` / `BindCourierRes` | `Messages.BindCourierReq`, `Messages.BindCourierRes` | shared-contract | not-used | 현재 실행 흐름에는 별도 courier gateway가 없다. 다른 언어와 공유하는 wire 계약이므로 메시지 타입은 유지한다. |
| `EnsureCourierActorReq` / `EnsureCourierActorRes` | `Messages.EnsureCourierActorReq`, `Messages.EnsureCourierActorRes` | shared-contract | done | target courier spot node의 actor를 보장한다. |
| `OfferDeliveryMsg` / `OfferDeliveryResultMsg` | `Messages.OfferDeliveryMsg`, `Messages.OfferDeliveryResultMsg` | shared-contract | done | 제안도 결정 결과도 **응답 없는 one-way**다. 시한은 `DispatchWorker`의 sweeper가 소유한다(공통 sample spec §7.4). |
| `OfferDeliveryNotify` / `CourierDecision` | `Messages.OfferDeliveryNotify`, `Messages.CourierDecision` | shared-contract | done | courier stream push와 courier client decision이다. |
| `ReassignDelivery` | `Messages.ReassignDelivery` | shared-contract | done | timeout 재배정 의미를 드러내는 shared message다. |
| `DeliveryStatusChangedReq` / `DeliveryStatusChangedRes` | `Messages.DeliveryStatusChangedReq`, `Messages.DeliveryStatusChangedRes` | shared-contract | done | Tracking server 기록 요청과 응답이다. |
| `EnsureCustomerActorReq` / `EnsureCustomerActorRes` | `Messages.EnsureCustomerActorReq`, `Messages.EnsureCustomerActorRes` | shared-contract | partial | actor 생성은 구현됐지만 공통 spec의 기존 actor 위치 조회 흐름은 추가 구현이 필요하다. |
| `ServerAssertionRequest` / `ServerAssertionResponse` | `Messages.ServerAssertionRequest`, `Messages.ServerAssertionResponse` | shared-contract | done | server-side evidence self-check 계약이다. |
| courier actor ref wire | `Messages.ActorRefWire` | shared-contract | done | courier actor node rid, actor id, generation을 wire-safe DTO로 전달한 뒤 session bind 직전에 framework actor ref로 재구성한다. |
| `DeliveryStatus` | `Messages.DeliveryStatus` | shared-contract | done | `Created`, `Assigned`, `Accepted`, `Reassigned`, `PickedUp`, `Delivered`, `Failed` 값을 가진다. |

## 공통 검증 흐름 매핑

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| Redis location store is ready first | `run_sample.sh` | validation | done | role 시작 전에 Redis endpoint readiness를 확인하고 실행별 key prefix를 전달한다. |
| Tracking starts before gateway/dispatch | `run_sample.sh` | validation | done | tracking channel endpoint readiness를 확인한다. |
| CustomerGateway stream session | `CustomerSession`, `DeliveryDispatchClientScenario` | validation | done | `SubscribeDeliveryRes`와 status notify를 검증한다. |
| CourierSession stream session | `CourierSession`, `DeliveryDispatchClientScenario` | validation | done | courier-a/b가 독립 stream session으로 bind된다. |
| Courier spot node 1/2 | `CourierSpotNode` processes | validation | done | courier-a는 node-1, courier-b는 node-2 actor로 배치된다. |
| Courier actor bind relay | `CourierSession`, `BindCourierSessionActorHandler`, `run_sample.sh` | validation | done | courier-a/b 요청이 actor handler까지 relay되고 actor 응답이 client에 전달되는지 검증한다. |
| Dispatch create delivery | `DispatchHttpServer`, client scenario | validation | done | `POST /deliveries`로 success/reassign 배송을 만든다. |
| delivery-success statuses | `DeliveryDispatchClientScenario` | validation | done | typed callback으로 기록한 실제 도착 순서가 `Assigned`, `Accepted`, `PickedUp`, `Delivered`인지 확인하고 각 알림의 `courier-a`를 검증한다. |
| delivery-reassign statuses | `DeliveryDispatchClientScenario` | validation | done | typed callback으로 기록한 실제 도착 순서가 `Assigned`, `Reassigned`, `Accepted`, `Delivered`인지 확인하고 필요한 알림의 `courier-b`를 검증한다. |
| server evidence check | `DispatchHttpServer`, `EvidenceStore` | validation | done | `/self-check/assert`가 두 delivery의 상태 순서를 검증한다. |
| topology marker | `run_sample.sh` | validation | done | 모든 role readiness 뒤 `topology=ready`를 출력한다. |
| reassignment marker | `DeliveryDispatchClientScenario` | validation | done | `deliverydispatch-reassignment=completed`를 출력한다. |
| server evidence marker | `DeliveryDispatchClientScenario` | validation | done | `deliverydispatch-server-evidence=completed`를 출력한다. |
| final marker | `run_sample.sh`, client scenario | validation | done | `deliverydispatch=completed`와 runner 완료 marker를 검증한다. |

## 현재 결론

Java `DeliveryDispatch`는 별도 courier gateway 없이 CourierSession에서 courier actor를 찾거나 만든 뒤
bind 요청을 actor로 relay한다. 메시지 이름과 `CreateDeliveryRes` 필드도 공통 spec에 맞췄다. 다만
CustomerGateway 재연결에서 기존 customer actor 위치를 먼저 조회하고 없을 때만 생성하는 흐름은
아직 닫히지 않았으므로 이 샘플의 공통 spec 대조는 진행 중이다. public framework API가 부족하면
sample helper로 우회하지 않고 public contract gap으로 분리한다.

## 검증

- `./gradlew -p samples/java/DeliveryDispatch test` 통과: 전체 13개 module compile, test task 성공.
- Java source에서 `CancellationToken`, `SpotRef`, `ZLinkAwait`, blocking `.await()`/`.join()`,
  typed packet-name override가 없음을 확인했다.
- `timeout 300s ./run_sample.sh` 통과: `deliverydispatch-reassignment=completed`,
  `deliverydispatch-server-evidence=completed`, `deliverydispatch=completed`,
  `deliverydispatch full client/server self-check completed` marker를 확인했다.
- runner cleanup은 각 server role의 종료 코드를 검사하며 정상 종료 또는 SIGTERM 종료 외의
  SIGABRT, SIGKILL과 timeout을 실패로 판정한다. 30초 bounded grace의 clean 재실행에서 모든 role이
  허용 종료 코드로 끝났다.
- `nice -n 15 timeout 600s ./run_sample.sh` 통과: `deliverydispatch full client/server self-check completed`
- `System.getProperty`·`System.getenv` 애플리케이션 코드 금지 gate와 JVM system property 주입 부재 검사를 통과했다.
- TicTacToe에만 허용된 endpoint 인자 `enableClient`, `connectRouter`, `connectPeerPub` 금지 gate와 전체 runner를 통과했다.
