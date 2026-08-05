# Java ShoppingMall Sample .NET 기준 포팅 Inventory

이 문서는 Java `ShoppingMall` 샘플을 `.NET` 구현과 공통 샘플 문서에 맞춰 포팅하기 위한
작업 목록이다. `gap`이나 `partial`이 남아 있으면 이 샘플은 완료로 보지 않는다.

## 기준 문서와 구현

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `framework/doc/framework/common/sample/event/shoppingmall.ko.md` | 이 inventory와 Java sample source | scenario | done | Java `run_sample.sh`가 성공, 멱등, 실패 보상, projection 재생, API/Workflow scale-out self-check를 검증한다. |
| `.NET: ShoppingMall.csproj` | `build.gradle.kts` | build | done | Java root Gradle project와 역할별 project를 추가했다. |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | CommerceApi 2개, OrderWorkflow 2개, Client, 전용 Redis를 실행하고 marker를 확인한다. |
| `.NET: run_sample.ps1` | `run_sample.ps1` | runner | done | shell runner와 같은 topology, Redis, marker 검증을 PowerShell에서도 제공한다. |
| `.NET: README.ko.md` | `README.ko.md` | docs | done | Java 실행 방법, role 책임, 성공 marker를 설명한다. |

## Client

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Client/ShoppingMall.Client.csproj` | `Client/build.gradle.kts` | build | done | Client role project를 추가했다. |
| `.NET: Client/Program.cs` | `Client/src/main/java/.../client/Program.java` | client-entry | done | API A/B HTTP client를 만들고 scenario를 실행한다. |
| `.NET: Client/ShoppingMallClientScenario.cs` | `Client/src/main/java/.../client/Program.java` | validation | done | 성공, 중복 멱등, pending 멱등 복구, inventory 실패, payment 실패, projection 재생, scale-out evidence를 검증한다. |
| `.NET: Client/Configuration/SampleNames.cs` | `Client/src/main/java/.../client/configuration/*` | config | done | API endpoint와 timeout 설정을 system property와 runner 환경으로 받는다. |

## Shared Contract

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Shared/Contracts/Messages.cs` | `Shared/src/main/java/.../shared/contracts/Messages.java` | shared-contract | done | `StartOrderReq`, `StartOrderRes`, `GetOrderState*`, seed DTO, workflow request/response DTO를 추가했다. |
| common: 주문 상태와 종료 상태 | `OrderState` DTO | shared-contract | done | `Created`, `InventoryReserved`, `PaymentAuthorized`, `Confirmed`, `Failed` 상태를 client와 server가 공유한다. |

## Server Configuration

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Server/Configuration/SampleNames.cs` | `Server/Configuration/src/main/java/.../configuration/SampleNames.java` | config | done | OrderWorkflow Instance Spot type, RouteMesh와 runner marker를 모은다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | role별 message-flow log 설정 | evidence | done | runner가 `message flow` marker를 확인한다. |
| common: 공유 location store | Redis location store config | runtime-config | done | CommerceApi와 OrderWorkflow가 같은 Redis prefix로 location store를 사용한다. CommerceApi channel client는 workflow endpoint를 받지 않고 framework가 location store에서 발견한다. |

## CommerceApi Role

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Server/CommerceApi/Program.cs` | `Server/CommerceApi/src/main/java/.../commerceapi/Program.java` | server-entry | done | HTTP `/health`, `/orders/start`, `/orders/{id}`, self-check endpoint를 연다. |
| `.NET: StartOrderUseCase` | `CommerceApiService` | application | done | 멱등 키 예약, cart/address/payment 검증, workflow route 요청을 담당한다. |
| `.NET: GetOrderStateUseCase` | `CommerceApiService` | application | done | read model만 조회하며 누락된 projection은 그대로 반환한다. |
| `.NET: ZLinkOrderWorkflowRouter` | `ZLinkClient` workflow request calls | framework-adapter | done | workflow instance channel로 public request를 보낸다. |

## OrderWorkflow Role

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Server/OrderWorkflow/Program.cs` | `Server/OrderWorkflow/src/main/java/.../orderworkflow/Program.java` | server-entry | done | HTTP `/health`, workflow channel server, spot mesh를 구성한다. |
| `.NET: OrderWorkflowSpot` | `OrderWorkflowSpot` | spot-adapter | done | route handler가 주문별 owner Spot으로 요청을 전달하고 Spot handler가 상태 전이를 수행한다. |
| `.NET: StartOrderWorkflowRouteHandler` | Java channel handler | route-handler | done | CommerceApi의 workflow 시작 요청을 owner workflow instance에서 처리한다. |
| `.NET: ContinueOrderWorkflowRouteHandler` | Java channel handler | route-handler | done | projection 복구와 중단 주문 재개를 public route request로 처리한다. |
| `.NET: RebuildOrderProjectionRouteHandler` | Java channel handler | route-handler | done | 이벤트 재생으로 read projection을 다시 만든다. |

## Domain and Store

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Server/Shared/Domain/OrderEvents.cs` | Java domain event model | domain | done | domain event는 framework 타입을 모른다. |
| `.NET: Server/Shared/Domain/OrderProjection.cs` | Java projection fold | domain | done | 이벤트 접기로 현재 주문 상태를 만든다. |
| `.NET: Server/Shared/Store/RedisCommerceStores.cs` | Java store adapter | external-adapter | done | cart, inventory, payment, idempotency, event stream, read model을 Redis-backed store 뒤에 둔다. |
| common: 결정적 payment/reservation id | workflow service | domain | done | 재시작 후에도 같은 단계 id를 재사용한다. |
| common: expected version fencing | event store append | domain | done | 같은 주문 owner가 중복 진행하지 않게 version을 확인한다. |

## 공통 요구 대응

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| common: 주문 시작과 상태 조회는 CommerceApi HTTP로 진입 | CommerceApi HTTP endpoint | validation | done | client는 workflow 서버를 직접 호출하지 않는다. |
| common: OrderId별 owner Spot | OrderWorkflowSpot | runtime-flow | done | OrderId별 Spot을 생성하고 start, continue, projection rebuild 요청을 Spot handler에서 처리한다. |
| common: 성공, 재고 실패, 결제 실패 event sequence | client self-check + server assertion | validation | done | runner가 evidence를 확인한다. |
| common: 중복 요청 멱등 | idempotency store | validation | done | 같은 key는 같은 order id를 돌려준다. |
| common: projection 삭제 후 재생 | rebuild self-check | validation | done | 이벤트를 다시 접어 read model을 복구한다. |
| common: API/Workflow scale-out | runner topology | validation | done | API 2개와 Workflow 2개가 shared workflow channel과 전역 `OrderId`를 사용하며, 특정 physical owner instance를 성공 조건으로 삼지 않는다. |
| common: Redis-backed shared state | RedisCommerceStore | external-adapter | done | runner가 실행별 전용 Docker Redis를 만들고 외부 endpoint를 재사용하지 않는다. |

## 남은 확인 사항

현재 Java `ShoppingMall` 샘플 inventory에는 남은 `gap` 또는 `partial` 항목이 없다.
직접 shell runner는 `nice -n 15 timeout 600s ./run_sample.sh`로 통과했고,
`shoppingmall full client/server self-check completed` marker를 확인했다. PowerShell runner는
parser 기반 syntax 검증을 통과했다.
PowerShell 실제 실행 검증은 PowerShell 환경이 있는 별도 검증 범위로 남긴다.
TicTacToe에만 허용된 endpoint 인자 `enableClient`, `connectRouter`, `connectPeerPub` 금지 gate도 통과했다.

전체 `samples/run_samples.sh`는 Java와 Kotlin sample gate를 함께 실행한다. Java-only closure에서는
`ZLINK_SAMPLE_FILTER=java/ShoppingMall`로 이 샘플만 따로 검증하고, 전체 runner gate는 Kotlin 전용
모듈 결과와 분리해서 판정한다.
