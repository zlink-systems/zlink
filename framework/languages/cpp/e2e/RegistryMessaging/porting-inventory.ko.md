# C++ LocationMessaging .NET 기준 포팅 inventory

이 문서는 `framework/languages/dotnet/e2e/LocationMessaging`의 파일을 기준으로
C++ config-1 E2E의 대응 파일과 남은 gap을 기록한다. C++ 디렉터리 이름은 시나리오 ID 연속성을 위해
아직 `RegistryMessaging`을 유지하지만, 목표 구조는 location store 기반 `LocationMessaging`이다.
이 문서에서 scenario 행은 현재 모두 `done` 상태로 유지한다. 이후 새 누락이 발견되면 현재 C++ 파일이
동작을 일부 담고 있더라도, 목표 구조나 검증 수준이 `.NET` 기준과 같은 의미로 정렬되지 않은 항목만
별도 gap으로 기록한다.

## 기준

- 공통 문서: `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md`
- .NET 기준 구현: `framework/languages/dotnet/e2e/LocationMessaging`
- C++ 대상: `framework/languages/cpp/e2e/RegistryMessaging`

## 파일 매핑

| .NET 기준 파일 | C++ 대응 파일 | 분류 | 상태 | 비고 |
|----------------|---------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 실행 로그와 산출물을 제외한다. |
| `README.ko.md` | `feature-map.ko.md` | docs | not-needed | C++에는 config별 보충 README가 없고, 공통 기준과 구현 상태는 feature-map에 둔다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | RM-A/B/C 시나리오 상태를 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | Redis location store를 준비하고 provider, workflow, consumer, client 프로세스를 실제로 띄운다. registry role은 제거했고 store consumer env 이름도 `STORE_CONSUMER` 기준으로 정리했다. Config-1 전체 sweep이 location store 기반으로 통과했다. 디렉터리와 target 이름은 scenario ID 연속성을 위해 아직 `RegistryMessaging`을 유지한다. |
| `Shared/Messages.cs` | `Shared/registry_messaging_contracts.hpp` | shared | done | request/reply/evidence DTO와 channel 이름을 C++ 타입으로 대응한다. |
| `Shared/RegistryMessaging.Shared.csproj` | `Shared/registry_messaging_contracts.hpp` | build | not-needed | C++ shared contract는 별도 프로젝트 파일 없이 header로 포함된다. |
| `Client/Program.cs` | `Client/main.cpp` | client-entry | done | scenario 선택과 HTTP-only driver 구성을 수행한다. client 프로세스는 framework runtime을 소유하지 않는다. |
| `Client/RegistryMessaging.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registry_messaging_client` target이 대응한다. |
| `Client/Support/ClientOptions.cs` | `Client/Support/client_support.hpp` | client-support | done | env parsing helper가 C++ support header에 있다. |
| `Client/Support/DynamicClusterLauncher.cs` | `run_e2e.sh` | runner-support | done | 프로세스 시작, stop, scenario별 cluster 조작은 shell runner가 담당한다. |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/client_support.hpp` | client-support | done | `ensure` helper가 C++ support header에 있다. |
| `Client/Scenarios/RmA1LocationStoreAutoConnectScenario.cs` | `Client/Scenarios/rm_a1_discovery_request_scenario.hpp` | scenario | partial | Endpoint 없는 Redis automatic discovery의 첫 request, reply와 선택된 Provider evidence는 forward·reverse·shuffle actual에서 통과했다. Ready Server 2개 public snapshot은 C++ `client_server_runtime_t` 구현이 없어 남아 있다. |
| `Client/Scenarios/RmA2ManualEndpointScenario.cs` | `Client/Scenarios/rm_a2_manual_endpoint_scenario.hpp` | scenario | done | 최초 manual request, in-flight 보존, automatic descriptor 병합과 두 ready target 선택을 actual-process로 검증한다. |
| `Client/Scenarios/RmA4SameRidFailoverScenario.cs` | `Client/Scenarios/rm_a4_same_rid_failover_scenario.hpp` | scenario | partial | Persistent Consumer의 v1→v2 public messaging replacement는 actual-process로 검증한다. Automatic RID와 public ClientServer status 조건은 source 구현 gap으로 남아 있다. |
| `Client/Scenarios/RmA6MultipleChannelsScenario.cs` | `Client/Scenarios/rm_a6_multiple_channels_scenario.hpp` | scenario | partial | 같은 persistent Consumer의 api/workflow public messaging과 각 channel scale-in 격리는 actual-process로 검증한다. MeshName별 public status 조건은 구현 gap으로 남아 있다. |
| `Client/Scenarios/RmA7GlobalObjectIdentityScenario.cs` | 없음 | scenario | blocked | M6B focused regression은 Actor·User Spot의 global create 합류, typed mismatch와 후속 InMesh 불변을 검증한다. Public Object fixture가 current exact interface로 compile되지 않아 두 process의 manager `Find`와 direct messaging actual은 아직 작성하지 않았다. |
| `Client/Scenarios/RmB1ScaleOutScenario.cs` | `Client/Scenarios/rm_b1_scale_out_scenario.hpp` | scenario | done | RM-B1 scale-out barrier와 post-scale 검증을 provider HTTP endpoint로 실행한다. |
| `Client/Scenarios/RmB2ScaleInScenario.cs` | `Client/Scenarios/rm_b2_scale_in_scenario.hpp` | scenario | done | RM-B2 scale-in barrier와 stale 회피 검증을 provider HTTP endpoint로 실행한다. |
| `Client/Scenarios/RmC1RequestSendScenario.cs` | `Client/Scenarios/rm_c1_request_send_scenario.hpp` | scenario | done | RM-C1 request/send happy path를 provider HTTP endpoint로 실행한다. |
| `Client/Scenarios/RmC2TargetedRouteScenario.cs` | `Client/Scenarios/rm_c2_targeted_route_scenario.hpp` | scenario | done | Public topology에서 target Ready를 확인한 뒤 exact Node direct request를 한 번 제출한다. `api-b` 단독 처리와 미등록 RID의 `RequestTargetNotFound`를 actual process에서 검증한다. |
| `Client/Scenarios/RmC3MultiProviderDistributionScenario.cs` | `Client/Scenarios/rm_c3_multi_provider_distribution_scenario.hpp` | scenario | done | Direct Consumer가 public ClientServer manual endpoint 두 개를 등록한다. 단건 request 90건을 제출해 두 Provider 선택, reply 합계와 Provider evidence 합계를 검증한다. Actual은 `logs/20260729-051403-2182401`이다. |
| `Client/Scenarios/RmC4TimeoutIsolationScenario.cs` | `Client/Scenarios/rm_c4_timeout_isolation_scenario.hpp` | scenario | done | store consumer HTTP role을 거쳐 RM-C4 timeout/late-reply isolation을 검증한다. |
| `Client/Scenarios/RmC5MissingPacketScenario.cs` | `Client/Scenarios/rm_c5_missing_packet_scenario.hpp` | scenario | done | store consumer HTTP role을 거쳐 RM-C5 missing packet negative path를 검증한다. |
| `Client/Scenarios/RmC7WeightedProviderScenario.cs` | `Client/Scenarios/rm_c7_weighted_provider_scenario.hpp` | scenario | done | RM-C7 weighted provider distribution을 provider HTTP endpoint로 실행하고 high-weight provider 선호를 검증한다. |
| `Client/Scenarios/RmC8PayloadRoundTripScenario.cs` | `Client/Scenarios/rm_c8_payload_round_trip_scenario.hpp` | scenario | done | RouteMesh SS payload length·SHA-256 왕복과 후속 정상 request를 검증한다. StreamNode Core STREAM inbound 상한은 별도 runtime·unit contract다. |
| `Client/Scenarios/RmC9BackpressureScenario.cs` | `Client/Scenarios/rm_c9_backpressure_scenario.hpp` | scenario | done | one-way send pressure 제출과 recovery evidence를 검증한다. public send는 bounded-failure oracle을 노출하지 않는다. |
| `Server/Consumer/Configuration/ConsumerOptions.cs` | `Server/Consumer/Configuration/consumer_options.hpp` | consumer-role | done | consumer HTTP endpoint, Redis location store endpoint/key prefix, direct provider endpoints를 env에서 읽는다. 미적용 `clientMaxMessageSize` 입력은 제거했다. |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/main.cpp` | consumer-role | done | C++ consumer role은 exact ClientServer `client()`를 한 번 등록한다. Config 1의 기본 consumer는 Redis Location Store automatic discovery를 사용하고, manual topology 시나리오만 endpoint마다 `connect()`를 사용한다. |
| `Server/Consumer/Endpoints/ConsumerEndpoints.cs` | `Server/Consumer/Endpoints/consumer_endpoints.hpp` | consumer-role | done | profile request, slow/missing request, missing command, payload, backpressure endpoint가 scenario 검증 경로에 쓰인다. RM-C3은 같은 consumer public request 경로를 반복 호출해 multi-provider distribution을 검증한다. C++ HTTP array body binding 차이는 scenario/public messaging 동작 차이로 보지 않는다. |
| `Server/Consumer/Program.cs` | `Server/Consumer/main.cpp` | consumer-role | done | consumer role executable 진입점이 있다. |
| `Server/Consumer/RegistryMessaging.Consumer.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registry_messaging_consumer` target이 대응한다. |
| `Server/Provider/Configuration/ServerOptions.cs` | `Server/Provider/Configuration/provider_options.hpp` | server-role | done | provider endpoint, weight와 log dir를 env에서 읽는다. Socket에 적용되지 않던 `maxMessageSize` 입력은 제거했다. |
| `Server/Provider/Endpoints/ProviderEndpoints.cs` | `Server/Provider/Endpoints/provider_endpoints.hpp` | endpoint | done | health/evidence와 profile request/manual/send, route request/missing, peer location list HTTP endpoint를 제공한다. 이 endpoint들이 public framework client/store를 호출하고 C++ E2E client는 HTTP로만 운전한다. |
| `Server/Provider/Handlers/ProviderHandlers.cs` | `Server/Provider/Handlers/provider_handlers.hpp`; `Server/Provider/main.cpp` | handler | done | route handler는 exact `route_message_context_t`를 받는다. ClientServer는 `client()`와 `server()` role builder로 구성하며 제거된 parent builder API를 사용하지 않는다. Provider와 ObjectClient target, contract headers와 M6A runtime focused test가 통과했다. |
| `Server/Provider/Infrastructure/EvidenceStore.cs` | `Server/Provider/Infrastructure/scenario_state.hpp` | infrastructure | done | evidence snapshot 저장소가 대응한다. |
| `Server/Provider/Program.cs` | `Server/Provider/main.cpp` | server-entry | done | provider role 진입점과 framework 구성을 수행한다. |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/main.cpp` | server-role | done | C++ app 구성은 provider main에 직접 노출한다. |
| `Server/Provider/RegistryMessaging.Provider.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registry_messaging_provider` target이 대응한다. |
| `Server/Registry/*` | 없음 | removed | done | Config-1에는 registry process가 없으므로 C++ registry role과 target을 제거했다. |
| `Server/Workflow/Configuration/ServerOptions.cs` | `Server/Workflow/Configuration/workflow_options.hpp` | server-role | done | workflow rid, endpoint, Redis location store endpoint/key prefix, log dir를 env에서 읽는다. |
| `Server/Workflow/Endpoints/WorkflowEndpoints.cs` | `Server/Workflow/Endpoints/workflow_endpoints.hpp`; `Server/Workflow/main.cpp` | endpoint | done | C++ workflow role은 health/evidence와 workflow request HTTP endpoint를 제공하고 runner가 HTTP readiness도 확인한다. |
| `Server/Workflow/Handlers/WorkflowHandlers.cs` | `Server/Workflow/Handlers/workflow_handlers.hpp` | handler | done | workflow request handler가 대응한다. |
| `Server/Workflow/Infrastructure/EvidenceStore.cs` | `Server/Workflow/Infrastructure/scenario_state.hpp` | infrastructure | done | workflow evidence snapshot 저장소가 대응한다. |
| `Server/Workflow/Program.cs` | `Server/Workflow/main.cpp` | server-entry | done | workflow role은 exact ClientServer `client()`·`server()` builder와 공통 TCP endpoint parser를 사용한다. |
| `Server/Workflow/WorkflowHostFactory.cs` | `Server/Workflow/main.cpp` | server-role | done | C++ app 구성은 workflow main에 직접 노출한다. |
| `Server/Workflow/RegistryMessaging.Workflow.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registry_messaging_workflow` target이 대응한다. |

## 공통 scenario ID 대응

| Scenario ID | C++ 대응 파일 | 상태 | 비고 |
|-------------|---------------|------|------|
| `RM-A1` | `Client/Scenarios/rm_a1_discovery_request_scenario.hpp` | partial | Automatic request actual 세 축은 통과했다. ClientServer public snapshot과 automatic RID prefix의 exact source 구현이 필요하다. |
| `RM-A2` | `Client/Scenarios/rm_a2_manual_endpoint_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-A4` | `Client/Scenarios/rm_a4_same_rid_failover_scenario.hpp` | partial | Public messaging replacement는 통과했다. Automatic identity/status/Shutdown/conflict 조건은 남아 있다. |
| `RM-A6` | `Client/Scenarios/rm_a6_multiple_channels_scenario.hpp` | partial | Public messaging 격리와 scale-in 독립성은 통과했다. MeshName별 status sequence·ready count는 남아 있다. |
| `RM-A7` | 없음 | blocked | `test_cpp_framework_m6b_runtime`은 global create/CAS 의미를 검증한다. Public Object fixture를 exact interface로 이관한 뒤 actual-process `GetOrCreate`·`Find`·direct request를 추가해야 한다. |
| `RM-B1` | `Client/Scenarios/rm_b1_scale_out_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-B2` | `Client/Scenarios/rm_b2_scale_in_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-C1` | `Client/Scenarios/rm_c1_request_send_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-C2` | `Client/Scenarios/rm_c2_targeted_route_scenario.hpp` | done | Actual `logs/20260729-040050-3900121`에서 target 단독 처리, 반대 provider evidence 부재와 `RequestTargetNotFound`를 검증했다. |
| `RM-C3` | `Client/Scenarios/rm_c3_multi_provider_distribution_scenario.hpp` | done | Public manual endpoint 두 개에서 90건의 reply와 evidence 합계가 일치한다. Actual `logs/20260729-051403-2182401`. |
| `RM-C4` | `Client/Scenarios/rm_c4_timeout_isolation_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-C5` | `Client/Scenarios/rm_c5_missing_packet_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-C7` | `Client/Scenarios/rm_c7_weighted_provider_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-C8` | `Client/Scenarios/rm_c8_payload_round_trip_scenario.hpp` | done | scenario 파일이 직접 검증한다. |
| `RM-C9` | `Client/Scenarios/rm_c9_backpressure_scenario.hpp` | done | P2 send pressure/recovery를 public one-way send 계약에 맞춰 검증한다. |

## 검증

- 2026-07-08: `timeout 180s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh RM-B2`
  - 결과: 통과, exit 0
  - 로그: `logs/20260708-131420-7922`
  - 의미: `api-b` provider scale-in 경로를 focused runner로 재검증했다. runner는 의도한 종료에서
    정상 exit, SIGINT, SIGTERM만 허용하고 SIGSEGV 같은 비정상 종료를 실패로 드러내도록 수정했다.
- 2026-07-08: `timeout 560s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh all`
  - 결과: 통과, exit 0
  - parent 로그: `logs/20260708-131829-51832`
  - 주요 child 로그: `logs/20260708-131832-52236`(RM-A1), `logs/20260708-131936-58483`(RM-B2),
    `logs/20260708-132020-63253`(RM-C4), `logs/20260708-132028-63890`(RM-C5),
    `logs/20260708-132039-64754`(RM-C7), `logs/20260708-132114-67294`(RM-C9)
  - 의미: parent runner가 Redis container 하나를 준비하고 child scenario가 같은 Redis endpoint를
    공유하는 형태로 RM-A1, RM-A2, RM-A4, RM-A6, RM-B1, RM-B2, RM-C1, RM-C2, RM-C3, RM-C4,
    RM-C5, RM-C7, RM-C8, RM-C9 sweep를 모두 통과했다. cleanup 중 provider가
    segmentation fault 같은 비정상 종료를 내면 runner가 실패하도록 보강한 뒤의 증거다.
- 2026-06-30: `./framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh RM-C9`
  - 결과: 통과
  - 로그: `logs/20260630-081704-3233416`
  - 의미: 현재 C++ 경로의 send pressure, provider evidence, recovery 검증은 유지된다.
- 2026-06-30: `./framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh all`
  - 결과: 통과
  - parent 로그: `logs/20260630-081727-3234507`
  - RM-C9 child 로그: `logs/20260630-081915-3246228`
  - 의미: 구현된 RegistryMessaging 시나리오는 전체 sweep에서 통과한다.
- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh all`
  - 결과: 통과
  - parent 로그: `logs/20260630-161051-366893`
  - RM-C9 child 로그: `logs/20260630-161317-377857`
  - 의미: 현재 checkout에서도 RM-A1, RM-A2, RM-A4, RM-A6, RM-B1, RM-B2, RM-C1, RM-C2,
    RM-C3, RM-C4, RM-C5, RM-C7, RM-C8, RM-C9 sweep가 모두 통과한다. RM-C9 child log의
    `backpressure-consumer-flow.log`와 `api-a-flow.log`에는 send pressure와 후속 recovery
    request/reply flow가 남는다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh RM-C9`
  - 결과: 통과
  - 로그: `logs/20260701-141721-60851`
  - 의미: 현재 public one-way send 계약에 맞춘 RM-C9 send pressure, provider evidence, recovery 검증이
    focused runner에서 통과한다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh all`
  - 결과: 통과
  - parent 로그: `logs/20260701-141526-48855`
  - RM-C9 child 로그: `logs/20260701-141721-60851`
  - 의미: RM-A1, RM-A2, RM-A4, RM-A6, RM-B1, RM-B2, RM-C1, RM-C2,
    RM-C3, RM-C4, RM-C5, RM-C7, RM-C8, RM-C9 sweep가 모두 통과한다.
- 2026-07-02: `timeout 420s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh all`
  - 결과: 통과, exit 0
  - parent 로그: `logs/20260702-064828-39071`
  - 주요 child 로그: `logs/20260702-064831-39355`(RM-A1), `logs/20260702-064848-41421`(RM-A4),
    `logs/20260702-064946-48026`(RM-C2), `logs/20260702-065108-55565`(RM-C8),
    `logs/20260702-065126-56684`(RM-C9)
  - 의미: C++ client를 HTTP-only driver로 바꾼 뒤에도 RM-A1, RM-A2, RM-A4, RM-A6, RM-B1, RM-B2,
    RM-C1, RM-C2, RM-C3, RM-C4, RM-C5, RM-C7, RM-C8, RM-C9 sweep가 모두 통과한다.
- 2026-07-03:
  `ZLINK_CPP_E2E_BUILD_DIR=framework/languages/cpp/build-redis-vcpkg timeout 560s framework/languages/cpp/e2e/RegistryMessaging/run_e2e.sh all`
  - 결과: 통과, exit 0
  - parent 로그: `logs/20260703-191402-27862`
  - 주요 child 로그: `logs/20260703-191407-28333`(RM-A1), `logs/20260703-191441-31736`(RM-A6),
    `logs/20260703-191452-32797`(RM-B1), `logs/20260703-191504-34171`(RM-B2),
    `logs/20260703-191604-39946`(RM-C4), `logs/20260703-191617-41118`(RM-C5),
    `logs/20260703-191628-42218`(RM-C7), `logs/20260703-191719-46328`(RM-C9)
  - 의미: registry role 제거 뒤 Redis location store 기반으로 Config-1 전체 sweep가 통과한다.
    RM-C7은 `api-a=75`, `api-b=25`로 weighted 자동 연결 분산을 확인했다. RM-B2는 `api-b`
    provider 종료 뒤 peer row 제거를 기다린 다음 `api-a` 단독 처리를 검증한다.
