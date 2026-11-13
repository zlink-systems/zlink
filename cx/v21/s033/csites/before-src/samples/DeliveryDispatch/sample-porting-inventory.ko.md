# C++ DeliveryDispatch .NET 기준 포팅 inventory

기준 구현: `framework/languages/dotnet/samples/DeliveryDispatch`

이 문서는 `.NET DeliveryDispatch` 샘플 파일이 C++ 샘플에서 어디에 대응되는지 기록한다. C++ 구현은
샘플과 같은 public framework API를 사용하지만, `framework/languages/cpp/samples/DeliveryDispatch` 아래에
CMake sample target과 runner를 둔다.

## 파일 매핑

| .NET 기준 파일 | C++ 대응 파일 | 분류 | 상태 | 비고 |
|----------------|---------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 실행 산출물을 제외한다. |
| `README.ko.md` | `README.ko.md`; `feature-map.ko.md` | docs | done | 실행 방법과 검증 항목을 C++ 기준으로 기록한다. |
| `run_sample.sh` | `run_sample.sh` | runner | done | 필요한 CMake target을 빌드하고 Redis location store, tracking, customer gateway, courier session, courier gateway, courier actor node 2개, dispatch center, dispatch API, probe, client를 실행한다. customer stream과 courier stream endpoint도 분리한다. |
| `Shared/DeliveryDispatch.Shared.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | shared header를 각 target include 경로로 사용한다. |
| `Shared/Contracts/Messages.cs` | `Shared/Contracts/messages.hpp` | shared | done | 상태 문자열은 이름 있는 값을 사용한다. Client request와 response에는 `ActorRef`, owner `NodeRid`와 session route를 넣지 않는다. `DeliveryStatusChangedReq`는 delivery id와 customer id를 함께 전달한다. |
| `Server/Configuration/DeliveryDispatch.Server.Configuration.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | configuration headers를 각 role target에서 include한다. |
| `Server/Configuration/EvidenceStore.cs` | `Server/Configuration/evidence_store.hpp` | infrastructure | done | 상태 evidence append/read/sequence 검증을 담당한다. |
| `Server/Configuration/SampleFlowLog.cs` | `sample_log_dir.hpp`; role `main.cpp` trace option | infrastructure | done | role별 message-flow log 파일 경로를 제공한다. |
| `Server/Configuration/SampleNames.cs` | `Server/Configuration/sample_names.hpp` | configuration | done | channel, route, actor, spot 이름이 대응한다. |
| `Server/Configuration/SampleTopology.cs` | `Server/Configuration/sample_topology.hpp` | configuration | done | role endpoint 환경 변수를 해석한다. |
| `Server/Registry/DeliveryDispatch.Server.Registry.csproj` | not-needed | build | removed | C++ sample은 Redis location store 기반 자동 연결을 사용하므로 registry target을 빌드하지 않는다. |
| `Server/Registry/RegistryHostFactory.cs` | not-needed | server-role | removed | registry host factory 역할은 location store 등록으로 대체됐다. |
| `Server/Registry/Program.cs` | not-needed | server-role | removed | registry role 진입점은 삭제했다. |
| `Server/Tracking/DeliveryDispatch.Server.Tracking.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | tracking target이 대응한다. |
| `Server/Tracking/TrackingServerHostFactory.cs` | `Server/Tracking/main.cpp` | server-role | done | C++ tracking role은 main에서 tracking route, status publisher, handler group을 구성한다. |
| `Server/Tracking/Handlers.cs` | `Server/Tracking/Handlers/tracking_handlers.hpp` | handler | done | actor ensure, delivery subscription, status changed handler가 대응한다. 상태 변경 handler는 요청의 customer id로 고객 actor를 찾으며 고정된 샘플 고객 id에 의존하지 않는다. |
| `Server/Tracking/Program.cs` | `Server/Tracking/main.cpp` | server-role | done | tracking role 진입점이다. |
| `Client/DeliveryDispatch.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | client target이 대응한다. |
| `Client/DeliveryDispatchClientScenario.cs` | `Client/delivery_dispatch_client_scenario.hpp` | scenario | done | successful delivery, reassignment, self-check marker를 검증한다. |
| `Client/Program.cs` | `Client/main.cpp` | client | done | api-url, stream endpoint를 받아 scenario를 실행한다. |
| `DeliveryDispatch.sln` | `framework/languages/cpp/CMakeLists.txt` | build | not-needed | C++는 solution 파일 대신 CMake target 묶음으로 role executable을 정의한다. |
| `Server/Dispatch/DeliveryDispatch.Server.Dispatch.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | C++의 dispatch API와 dispatch center target이 .NET Dispatch server 책임을 나눠 맡는다. |
| `Server/Dispatch/DispatchServerHostFactory.cs` | `Server/DispatchApi/main.cpp`; `Server/DispatchCenter/main.cpp` | server-role | done | HTTP API host와 dispatch worker host 구성이 C++에서 두 executable로 분리되어 있다. |
| `Server/Dispatch/DispatchWorkQueue.cs` | `Server/DispatchCenter/main.cpp` | infrastructure | done | C++ dispatch center role 내부 queue/worker loop가 대응한다. |
| `Server/Dispatch/DispatchWorker.cs`; `Server/Dispatch/AssignDeliveryHandler.cs`; `Server/Dispatch/DispatchZLinkAdapters.cs` | `Server/DispatchCenter/main.cpp`; `Server/DispatchApi/main.cpp` | support | done | delivery assignment HTTP/channel edge, dispatch adapter, courier offer worker 책임은 DispatchApi/DispatchCenter로 나뉘며, courier offer는 CourierGateway를 거쳐 CourierActorNode actor handler로 전달되고 timeout 재배정과 tracking status publish가 대응한다. |
| `Server/Dispatch/Program.cs` | `Server/DispatchApi/main.cpp`; `Server/DispatchCenter/main.cpp` | server-role | done | C++는 HTTP edge와 worker를 별도 role executable로 실행한다. |
| `Server/CustomerGateway/DeliveryDispatch.Server.CustomerGateway.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | CustomerGateway target이 대응한다. |
| `Server/CustomerGateway/CustomerGatewayHostFactory.cs` | `Server/CustomerGateway/main.cpp` | server-role | done | customer stream bind, customer actor spot mesh, status fanout subscriber를 구성한다. |
| `Server/CustomerGateway/CustomerSession.cs` | `Server/CustomerGateway/main.cpp` | session | done | 고객 stream session이 customer actor를 생성하고 `session_actor_manager_t`로 바인드한다. framework가 현재 stream 연결과 disconnect 정리를 관리하며, 알 수 없는 packet은 bound actor로 relay한다. |
| `Server/CustomerGateway/CustomerActor.cs`; `Server/CustomerGateway/CustomerActorAccess.cs` | `Server/CustomerGateway/main.cpp` | actor | done | CustomerGateway role의 runtime actor와 actor 접근 경로가 대응한다. |
| `Server/CustomerGateway/CustomerActorDirectory.cs` | `Server/CustomerGateway/main.cpp` | infrastructure | done | CustomerGateway가 delivery id별 customer actor lookup을 관리한다. |
| `Server/CustomerGateway/CustomerGatewayHandlers.cs` | `Server/CustomerGateway/main.cpp`; `Server/Tracking/Handlers/tracking_handlers.hpp` | handler | done | CustomerGateway handler가 customer actor bound session으로 push한다. Tracking handler는 상태를 기록하고 actor directory에서 대상 actor ref를 찾아 one-way 상태 메시지를 보낸다. |
| `Server/CustomerGateway/SubscribeDeliverySessionHandler.cs` | `Server/CustomerGateway/main.cpp` | handler | done | stream `SubscribeDeliveryReq` 요청을 받아 customer actor를 bind하고 actor entry spot의 subscribe handler로 relay한다. |
| `Server/CustomerGateway/Spots/EntrySpot/CustomerEntrySpot.cs` | `Server/CustomerGateway/main.cpp` | spot | done | CustomerGateway entry spot이 구독과 상태 갱신 handler를 등록한다. Actor 조회는 Framework Actor Directory가 담당한다. |
| `Server/CustomerGateway/Spots/EntrySpot/Handlers/SubscribeDeliveryActorHandler.cs`; `Server/CustomerGateway/Spots/EntrySpot/Handlers/DeliveryStatusUpdatedHandler.cs` | `Server/CustomerGateway/main.cpp` | handler | done | customer actor subscribe request가 delivery id별 customer binding을 저장하고 `SubscribeDeliveryRes`를 반환하며, delivery status update는 bound customer actor/session fanout으로 대응한다. |
| `Server/CustomerGateway/Program.cs` | `Server/CustomerGateway/main.cpp` | server-role | done | customer gateway role 진입점이다. |
| `Server/CourierSession/DeliveryDispatch.Server.CourierSession.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | CourierSession target이 대응한다. |
| `Server/CourierSession/CourierSessionHostFactory.cs` | `Server/CourierSession/main.cpp` | server-role | done | courier stream endpoint와 courier actor spot mesh bridge를 별도 role로 구성한다. |
| `Server/CourierSession/CourierSession.cs`; `Server/CourierSession/CourierSessionBinder.cs` | `Server/CourierSession/main.cpp` | session | done | courier-a/courier-b stream session을 받고 actor ref를 public `session_actor_manager_t`에 바인드한다. framework가 stream 연결과 disconnect 정리를 관리하며, decision packet은 bound actor로 relay한다. |
| `Server/CourierSession/BindCourierSessionHandler.cs` | `Server/CourierSession/main.cpp` | handler | done | `BindCourierSessionReq`를 public stream request로 받는다. `ActorManager.GetOrCreate`가 반환한 exact `ActorRef`를 Framework session bind에만 사용하고 client reply에는 courier id만 반환한다. |
| `Server/CourierSession/Program.cs` | `Server/CourierSession/main.cpp` | server-role | done | courier session role 진입점이다. |
| `Server/CourierGateway/*` | not-needed | server-role | removed | Framework의 global ActorManager, Actor Directory와 Actor Client가 생성·위치 조회·전송을 담당하므로 별도 courier gateway와 application route cache를 두지 않는다. |
| `Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | courier actor node target이 대응한다. runner는 같은 executable을 두 instance로 실행하고 RouteMesh routing id는 runtime allocation을 사용한다. |
| `Server/CourierActorNode/NodeHostFactory.cs` | `Server/CourierActorNode/main.cpp` | server-role | done | courier actor node별 channel과 courier actor spot mesh를 구성한다. |
| `Server/CourierActorNode/ActorDirectory.cs` | not-needed | infrastructure | removed | Actor 위치는 Location Store와 Framework Actor Directory가 관리한다. Courier actor는 현재 offer attempt만 보관한다. |
| `Server/CourierActorNode/CourierActor.cs` | `Server/CourierActorNode/main.cpp` | actor | done | Courier actor가 actor context를 보유하고, entry spot handler가 bound session으로 `OfferDeliveryNotify`를 push한다. |
| `Server/CourierActorNode/RouteHandlers.cs` | `Server/CourierSession/main.cpp`, `Server/Dispatch/main.cpp` | handler | done | 별도 Entry Spot route packet을 두지 않는다. CourierSession은 `ActorManager.GetOrCreate`로 actor를 보장하고 Dispatch는 Actor Directory와 Actor Client로 `OfferDeliveryMsg`를 보낸다. |
| `Server/CourierActorNode/Spots/EntrySpot/EntrySpot.cs` | `Server/CourierActorNode/main.cpp` | spot | done | courier entry spot이 actor join과 actor packet handler 등록을 담당한다. |
| `Server/CourierActorNode/Spots/EntrySpot/Handlers/BindCourierSessionActorHandler.cs` | `Server/CourierActorNode/main.cpp` | handler | done | `BindCourierSessionReq` actor request가 actor/session binding 확인 reply를 반환한다. |
| `Server/CourierActorNode/Spots/EntrySpot/Handlers/CourierDecisionActorHandler.cs` | `Server/CourierActorNode/main.cpp` | handler | done | `CourierDecisionMsg` actor send가 pending offer decision을 완료한다. |
| `Server/CourierActorNode/Program.cs` | `Server/CourierActorNode/main.cpp` | server-role | done | courier actor node role 진입점이다. |

## Scenario 대응

| Scenario | C++ 대응 | 상태 | 비고 |
|----------|----------|------|------|
| location store readiness | `Probe/main.cpp`; `run_sample.sh` | done | Tracking route readiness를 client 실행 전에 확인한다. |
| successful delivery | `Client/delivery_dispatch_client_scenario.hpp`; `Server/CustomerGateway/main.cpp`; `Server/CourierSession/main.cpp`; `Server/CourierGateway/main.cpp`; `Server/CourierActorNode/main.cpp` | done | 고객 session 상태 push와 courier-a gateway/actor-node/courier-session offer/decision 경로를 검증한다. |
| reassigned delivery | `Client/delivery_dispatch_client_scenario.hpp`; `Server/CustomerGateway/main.cpp`; `Server/CourierSession/main.cpp`; `Server/CourierGateway/main.cpp`; `Server/CourierActorNode/main.cpp`; `Server/DispatchCenter/main.cpp` | done | courier-a stream offer timeout 뒤 gateway가 courier-b actor node로 재요청하고, courier-b stream offer/decision과 재배정 상태를 검증한다. |
| server evidence self-check | `Server/DispatchApi/main.cpp`; `Server/Configuration/evidence_store.hpp` | done | `/self-check/assert`가 evidence log를 검증한다. |
| message-flow evidence | role `main.cpp`; `run_sample.sh` | done | role별 trace log에 message-flow 기록이 남는지 확인한다. |

## 검증

- 2026-07-15: `ctest --test-dir framework/languages/cpp/build -R 'test_cpp_framework_sample_parity' --output-on-failure`
  - 결과: 통과
  - 의미: 이름 있는 상태 문자열, 전체 actor ref snapshot, `customerId` 직렬화와 Tracking의 customer id 기반 actor 조회를 검사한다.
- 2026-07-15: `framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh`
  - 결과: 통과
  - 출력: `deliverydispatch sample result=passed`
  - 의미: 변경된 customer id 전달 계약으로 고객 상태 알림과 재배정 흐름이 끝까지 동작한다.

- 2026-07-01: `timeout 420s framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh`
  - 결과: 통과
  - 출력: `deliverydispatch sample result=passed`
  - 의미: sample target build, location store readiness, customer stream status push, courier stream offer/decision,
    reassignment marker, server evidence self-check, role별 message-flow evidence가 샘플 runner에서 검증된다.
- 2026-07-01: `ctest --test-dir framework/languages/cpp/build -R 'test_cpp_framework_sample_parity' --output-on-failure`
  - 결과: 통과
  - 의미: DeliveryDispatch sample directory, shared contract 위치, public target 문서화, runner marker,
    common DeliveryDispatch message name drift 방지 검사가 C++ sample parity test에 포함된다.
- 2026-07-01: `timeout 600s framework/languages/cpp/samples/run_samples.sh`
  - 결과: 통과
  - 출력: `PASS TicTacToe.Cpp`, `tictactoe full client/server self-check completed`,
    `bingo full client/server self-check completed`, `deliverydispatch sample result=passed`
  - 의미: C++ sample contract gate와 기존 샘플 runner를 포함한 상위 sample runner가 DeliveryDispatch 추가 후에도 통과한다.
- 2026-07-07: `CMAKE_BUILD_PARALLEL_LEVEL=1 nice -n 10 timeout 900s framework/languages/cpp/samples/run_samples.sh`
  - 결과: 통과
  - 출력: `PASS TicTacToe.Cpp`, `tictactoe full client/server self-check completed`,
    `bingo full client/server self-check completed`, `deliverydispatch sample result=passed`,
    `PASS SupportChat.Cpp`, `supportchat sample result=passed`, `PASS GameQuest.Cpp`,
    `gamequest sample result=passed`, `PASS ShoppingMall.Cpp`
  - 의미: 상위 sample runner가 여섯 C++ 샘플을 모두 실행하고 DeliveryDispatch를 빠뜨리지 않는지 다시 확인했다.

- 2026-08-03: `framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh`
  - 결과: exit code 0, `deliverydispatch sample result=passed`
  - 의미: `occurredAtUnixMs`, nullable courier/reason, timeout reassignment, customer actor
    routing, server evidence와 role별 message-flow file log를 실제 process에서 확인했다.

## Message 분류

`CreateDelivery*`, `SubscribeDelivery*`, `BindCourierSession*`, `DeliveryStatus*`와
`OfferDelivery*`·`CourierDecisionMsg`는 공통 sample 계약에 속한다. `EnsureCustomerActorReq`와
`EnsureCourierActorReq`는 role 사이의 internal request다. `server_assertion_*`는 runner가
server evidence를 읽는 test/evidence-only message이며 public client contract가 아니다.

## 설계 재검토

Tracking이 고객 actor를 찾는 방법으로 delivery id에 대한 별도 색인을 유지하는 안과 상태 변경 메시지의
customer id를 사용하는 안을 비교했다. 별도 색인은 Dispatch와 Tracking이 같은 delivery-customer 관계를
각자 관리하게 만든다. 확정된 wire 필드를 사용하면 Tracking은 요청의 식별자만 해석하고, 고객 actor
directory가 위치 조회를 전담한다. 따라서 고정된 샘플 고객 id와 중복 관계 저장을 제거했다.
