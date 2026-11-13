# ShoppingMall C++ sample porting inventory

이 문서는 `.NET` ShoppingMall 샘플과 공통 ShoppingMall 샘플 문서의 요구를 C++ ShoppingMall
샘플에 매핑한 inventory다. C++ 샘플은 public framework API를 사용하며, public contract에 없는
경로를 private API나 raw frame 조작으로 우회하지 않는다.

## 기준 파일 매핑

| 기준 | C++ 대응 | 분류 | 상태 | 비고 |
|------|----------|------|------|------|
| `.NET: ShoppingMall.csproj`, role별 project | `framework/languages/cpp/CMakeLists.txt` | build-root | done | CommerceApi, OrderWorkflow, Client 실행 파일 target을 만든다. |
| `.NET: Shared/Contracts/Messages.cs` | `Shared/Contracts/messages.hpp` | shared-contract | done | 주문 시작, workflow command, projection, self-check request/reply를 C++ typed JSON message로 대응한다. |
| `.NET: Server/Configuration/SampleNames.cs` | `Server/Configuration/sample_topology.hpp` | server-config | done | workflow channel 이름, spot discovery 이름, Redis endpoint와 key prefix를 공유한다. |
| `.NET: Server/Configuration/SampleFlowLog.cs` | role별 `main.cpp`, `run_sample.sh` | server-evidence | done | message-flow log를 sample log directory에 남기고 runner가 확인한다. |
| `.NET: Server/Shared/Store/RedisCommerceStores.cs` | `Server/Common/store.hpp` | external-adapter | done | 주문 이벤트 스트림, 조회 모델, 재고, 결제, 멱등 상태를 Redis key prefix 아래 하나의 commerce state로 저장하고 lock key로 갱신을 직렬화한다. |
| `.NET: Server/Shared/Domain/OrderEvents.cs`, `OrderProjection.cs` | `Shared/Contracts/messages.hpp`, `Server/Common/workflow_logic.hpp` | domain | done | 주문 상태, 이벤트 이름, projection rebuild 규칙을 C++ 타입과 workflow 함수로 대응한다. |
| `.NET: Server/CommerceApi/Application/OrderWorkflow/StartOrderUseCase.cs` | `Server/CommerceApi/main.cpp` | application | done | 멱등 mapping 예약, payment method 저장, owner workflow 요청을 처리한다. |
| `.NET: Server/CommerceApi/Infrastructure/ZLink/ZLinkOrderWorkflowRouter.cs` | `Server/CommerceApi/main.cpp`, `Server/Configuration/sample_topology.hpp` | message-flow | done | C++는 `OrderId`로 workflow instance를 고르고, workflow owner에 spot 생성을 요청한 뒤 API process의 local SpotMesh bridge와 dedicated RouteMesh channel로 owner order spot에 command를 보낸다. |
| `.NET: Server/OrderWorkflow/Application/OrderWorkflow/OrderWorkflowService.cs` | `Server/OrderWorkflow/main.cpp`, `Server/Common/workflow_logic.hpp` | application | done | start, continue, projection rebuild 요청을 처리하고 이벤트 순서와 종료 상태를 남긴다. |
| `.NET: Server/OrderWorkflow/Infrastructure/ZLink/Spots/OrderWorkflowSpot/*` | `Server/OrderWorkflow/main.cpp` | spot | done | C++ target은 order별 `OrderWorkflowSpot`을 만들고, `StartOrderWorkflowReq`, `ContinueOrderWorkflowReq`, `RebuildOrderProjectionReq`를 spot packet handler에서 실행한다. runner가 workflow log의 `spot=` marker로 이 경로를 확인한다. |
| `.NET: Client/ShoppingMallClientScenario.cs`, `Client/Program.cs` | `Client/main.cpp` | client-scenario | done | 정상 주문, 중복 시작, 동시 멱등성, pending 복구, 재개, 실패, projection rebuild, 지연 읽기, scale-out self-check를 검증한다. |
| `.NET: README.ko.md`, `run_sample.sh`, `run_sample.ps1` | `README.ko.md`, `run_sample.sh` | runner-doc | done | runner가 필요한 CMake target을 빌드하고 Redis Docker 컨테이너를 띄우며, 실행별 key prefix와 log를 격리하고 완료 marker를 확인한다. |

## 공통 요구 매핑

| 기준 | C++ 대응 | 분류 | 상태 | 비고 |
|------|----------|------|------|------|
| `common: 주문 시작은 장바구니, 주소, 결제 수단을 받는다` | `Client/main.cpp`, `Server/CommerceApi/main.cpp` | validation | done | success, inventory failure, payment failure 요청을 HTTP로 시작한다. |
| `common: 재고 예약, 결제 승인, 확정과 실패 보상` | `Server/Common/workflow_logic.hpp`, `Client/main.cpp` | workflow | done | 성공은 `OrderStartedEvent > InventoryReservedEvent > PaymentAuthorizedEvent > OrderConfirmedEvent`를 남긴다. 결제 실패는 예약 해제 이벤트를 남긴다. |
| `common: 같은 주문 시작 요청은 멱등하게 하나의 주문으로 모은다` | `Server/CommerceApi/main.cpp`, `Client/main.cpp` | validation | done | 동일 idempotency key 재요청과 api-a/api-b 동시 요청을 같은 order id로 검증한다. |
| `common: pending mapping 복구` | `Server/CommerceApi/main.cpp`, `Client/main.cpp` | validation | done | self-check endpoint가 pending mapping을 만든 뒤 다른 API instance에서 주문을 이어 간다. |
| `common: 현재 주문 상태 조회` | `Server/CommerceApi/main.cpp`, `Client/main.cpp` | validation | done | `/orders/get`가 Redis 조회 모델을 읽고 client가 상태 전이를 검증한다. |
| `common: projection 삭제 뒤 replay로 복원` | `Server/Common/workflow_logic.hpp`, `Client/main.cpp` | validation | done | projection delete 후 continue와 rebuild endpoint로 복원한다. |
| `common: 성공, 재고 실패, 결제 실패 event 순서 evidence` | `Server/CommerceApi/main.cpp`, `run_sample.sh` | validation | done | `/self-check/assert`가 event 순서, payment failure, released reservation, owner 분산 evidence를 확인한다. |
| `common: 주문 상태는 무손실 저장소에 남긴다` | `Server/Common/store.hpp` | storage | done | Redis state key와 lock key를 사용해 모든 server process가 같은 state를 읽고 쓴다. |
| `common: location store는 공유 저장소를 사용한다` | `Server/Configuration/location_store.hpp`, role별 `main.cpp` | location-store | done | Redis location store를 등록하고 runner가 모든 role의 location-store-claim log를 확인한다. |
| `common: OrderId별 owner 분산` | `Server/Configuration/sample_topology.hpp`, `Client/main.cpp`, `Server/OrderWorkflow/main.cpp` | validation | done | C++는 stable owner index로 workflow-a/workflow-b에 분산하고, owner process의 order별 `OrderWorkflowSpot`에서 command를 실행한다. |
| `common: compact 구현 금지` | `Server/CommerceApi`, `Server/OrderWorkflow`, `Client` | structure | done | API, workflow, client를 별도 실행 파일로 유지한다. |

## .NET 파일 대응 보강

| .NET 파일 | C++ 대응 | 상태 | 비고 |
|-----------|----------|------|------|
| `Client/ShoppingMall.Client.csproj`; `Shared/ShoppingMall.Shared.csproj`; `Server/Shared/ShoppingMall.Server.Shared.csproj` | `Client/main.cpp`; `Shared/Contracts/messages.hpp`; `Server/Common/store.hpp`; `Server/Common/workflow_logic.hpp`; `framework/languages/cpp/CMakeLists.txt` | done | client/shared project 책임은 C++ client executable, shared DTO, common store/workflow logic, CMake target으로 대응한다. |
| `Server/Configuration/ShoppingMall.Server.Configuration.csproj` | `Server/Configuration/sample_topology.hpp`; `Server/Configuration/location_store.hpp` | done | endpoint, role name, Redis state/location store 설정을 C++ configuration header로 대응한다. |
| `Server/CommerceApi/ShoppingMall.CommerceApi.csproj`; `Infrastructure/Http/HttpCommerceApiPeerClient.cs`; `Infrastructure/Http/HttpOrderWorkflowSelfCheckClient.cs`; `Ports/Outbound/WorkflowPorts.cs` | `Server/CommerceApi/main.cpp` | done | CommerceApi executable이 peer/self-check HTTP edge, workflow outbound port, order start/continue/get API를 맡는다. |
| `Server/OrderWorkflow/ShoppingMall.OrderWorkflow.csproj`; `Application/SelfCheck/OrderWorkflowSelfCheckService.cs`; `Domain/ShoppingMall/OrderDomain.cs`; `Infrastructure/ZLink/Handlers/OrderWorkflowRouteHandlers.cs` | `Server/OrderWorkflow/main.cpp`; `Server/Common/workflow_logic.hpp`; `Shared/Contracts/messages.hpp` | done | OrderWorkflow executable이 self-check, order domain transition, route/Spot command handler를 맡는다. |
| `Infrastructure/ZLink/Spots/OrderWorkflowSpot/Handlers/StartOrderWorkflowHandler.cs`; `ContinueOrderWorkflowHandler.cs`; `PrepareInventoryReservedCheckpointHandler.cs`; `RebuildOrderProjectionHandler.cs` | `Server/OrderWorkflow/main.cpp`; `Server/Common/workflow_logic.hpp` | done | order별 `OrderWorkflowSpot` handler에서 start/continue/checkpoint/rebuild command를 실행한다. |

## 남은 gap

현재 ShoppingMall sample process에서 확인하지 못한 runtime gap은 없다. `StartOrderRes`는
`{orderId, state}`를 반환하고 `OrderState`의 nullable field와 workflow command의
`sourceCommandId`를 typed JSON으로 보존한다. workflow readiness endpoint는 두 API가 두
workflow peer를 확인한 뒤 public client call을 시작하도록 한다. cart/inventory/payment seed와
`server_assertion_*`는 self-check/evidence-only다. package provenance, native Windows와 common
E2E 전체 분모는 이 sample inventory와 별도로 ledger에서 판정한다.

## 검증 기록

- `cmake --build framework/languages/cpp/build --target sample_cpp_framework_shoppingmall_commerce_api sample_cpp_framework_shoppingmall_order_workflow sample_cpp_framework_shoppingmall_client`
- `timeout 300s framework/languages/cpp/samples/ShoppingMall/run_sample.sh`

현재 runner는 `shoppingmall-server-evidence=completed`와 `PASS ShoppingMall.Cpp`까지 통과한다. 또한
workflow-a와 workflow-b log에서 `shoppingmall order: started ... spot=` marker도 확인해
workflow command가 route handler group이 아니라 order spot handler에서 실행됐음을 검증한다.

- 2026-08-03: ShoppingMall readiness 정적 regression과 개별 process runner 4회가 통과했고,
  official six-sample aggregate와 PowerShell aggregate도 exit code 0이다.
