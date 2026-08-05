# C++ RuntimeMonitoring .NET porting inventory

## 10.0.0 목표 판정

Config 7은 MeshNode·peer·ChannelName readiness와 runtime health를 공개 monitoring 표면으로 검증한다.
Publish는 전용 snapshot·metric·runtime event를 만들지 않으므로 remote·local target 결과의 부재를
별도 negative E2E로 검증해야 한다. 아래 파일 대응과 기존 MON marker는 현재 구현 inventory이며,
이 목표 축을 모두 충족하기 전까지 RuntimeMonitoring 포팅 상태는 `11.0.0 전환 대상`이다.


기준 구현: `framework/languages/dotnet/e2e/RuntimeMonitoring`

Config 7 C++ 구현은 `.NET` RuntimeMonitoring의 HTTP driver 구조와 scenario 의미를 유지하되,
registry role은 포팅하지 않는다. 공통 Config 7 문서에 맞춰 service 계열 role이 Redis location
store를 공유하고, location runtime monitoring source가 topology/status/service summary evidence를
발행한다.

| .NET 파일 | C++ 대응 | 분류 | 상태 | 비고 |
|-----------|----------|------|------|------|
| `.gitignore` | `.gitignore` | metadata | done | logs 제외를 유지한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | 최신 Config 7 location-runtime 기준 상태와 검증 증거를 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | Redis container, service, filtered service, throwing service, trigger, client role executable을 실행하고 MON-A1~MON-D1 전체 scenario gate를 검증한다. MON-D1은 service restart를 위해 별도 client invocation으로 실행한다. |
| `Shared/RuntimeMonitoring.Shared.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | not-needed | C++ shared contract는 `Shared/runtime_monitoring_contracts.hpp` header로 각 role/client target에 포함된다. 별도 shared binary target은 필요 없다. |
| `Shared/Messages.cs` | `Shared/runtime_monitoring_contracts.hpp` | shared | done | profile message, monitoring source 이름, evidence wait request를 C++ role들이 사용한다. |
| `Client/RuntimeMonitoring.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_runtime_monitoring_client` target이 있다. |
| `Client/Program.cs` | `Client/main.cpp` | client | done | `.NET`처럼 plain HTTP driver로 common MON-A/B/C slice와 MON-D1 재시작 검증 slice를 dispatch한다. client는 framework runtime을 호스트하지 않는다. |
| `Client/Support/ClientOptions.cs` | `Client/Support/client_options.hpp` | support | done | scenario, service URL, filtered service URL, throwing service URL, trigger URL, log dir를 env에서 읽는다. |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/client_support.hpp` | support | done | assertion, evidence wait/count, log wait, trigger HTTP request helper를 scenario들이 공유한다. |
| `Client/Scenarios/MonA1SocketEventsScenario.cs` | `Client/Scenarios/mon_a1_socket_events_scenario.hpp` | scenario | done | trigger role이 service A에 transient request를 보내고 socket event evidence를 검증한다. |
| `Client/Scenarios/MonA2RegistryEventsScenario.cs` | `Client/Scenarios/mon_a2_location_events_scenario.hpp` | scenario | done | C++에서는 registry event 대신 service role의 `location-runtime` source가 발행한 topology/service summary evidence를 검증한다. |
| `Client/Scenarios/MonA3SpotEventsScenario.cs` | `Client/Scenarios/mon_a3_spot_events_scenario.hpp` | scenario | done | Redis location store로 발견한 SPOT mesh peer, subject 변화, timer failure evidence를 검증한다. |
| `Client/Scenarios/MonA4AvailabilityTransitionScenario.cs` | `Client/Scenarios/mon_a4_availability_transition_scenario.hpp` | scenario | done | drain/restore admin evidence, socket admission event, location topology evidence를 검증한다. |
| `Client/Scenarios/MonA5FixedKindsScenario.cs` | `Client/Scenarios/mon_a5_fixed_kinds_scenario.hpp` | scenario | done | invalid handshake, location `StatusChanged`, spot `StatusChanged`, stopped timer evidence를 검증한다. |
| `Client/Scenarios/MonB1RemoteBackpressureScenario.cs` | 제거 | scenario | superseded | target별 publish result·event·snapshot count를 요구하던 시나리오는 CA-D77 계약과 함께 제거했다. 새 MON-B1은 publish 전용 관측값 부재를 검증해야 한다. |
| `Client/Scenarios/MonB2LocalTargetDropScenario.cs` | 제거 | scenario | superseded | local target별 publish result·event·snapshot count를 요구하던 시나리오는 CA-D77 계약과 함께 제거했다. 새 MON-B2는 publish 전용 관측값 부재를 검증해야 한다. |
| `Client/Scenarios/MonC1DispatchFailureScenario.cs` | `Client/Scenarios/mon_c1_dispatch_failure_scenario.hpp` | scenario | done | throwing service evidence, stderr marker, follow-up request recovery를 검증한다. |
| `Client/Scenarios/MonD1FailureRecoveryScenario.cs` | `Client/Scenarios/mon_d1_failure_recovery_scenario.hpp` | scenario | done | filtered service stop/restart 뒤 trigger HTTP request, restarted service evidence, restart 이후 location topology continuity evidence를 검증한다. |
| `Server/Registry/*` | not-needed | server-role | not-needed | Config 7 C++는 별도 registry process를 실행하지 않는다. Redis location store와 `location-runtime` source가 같은 검증 의미를 담당한다. |
| `Server/Service/RuntimeMonitoring.Service.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_runtime_monitoring_service` target이 있다. |
| `Server/Service/Program.cs` | `Server/Service/main.cpp` | server-role | done | service role 진입점은 `service_host_factory.hpp`의 all-profile host factory 실행만 담당한다. |
| `Server/Service/ServiceHostFactory.cs` | `Server/Service/service_host_factory.hpp`, `Server/Service/Support/service_host.hpp` | server-role | done | Redis location store, client/server channel, SPOT router/pub-sub mesh, monitoring sources, HTTP endpoints를 구성한다. |
| `Server/Service/Handlers/ServiceEventRecorders.cs` | `Server/Service/Handlers/service_event_recorders.hpp` | handler | done | socket, spot/timer, location runtime, throwing socket event evidence 기록 helper를 제공한다. |
| `Server/Service/Handlers/ServiceHandlers.cs` | `Server/Service/Handlers/service_handlers.hpp` | handler | done | profile request handler, monitoring spot, failing timer, admin weight, spot create, shutdown HTTP handlers를 둔다. |
| `Server/Service/Support/ServiceEvidenceStore.cs` | `Server/Shared/evidence_store.hpp` | support | done | shared evidence store와 waiter API가 있다. |
| `Server/Service/Support/ServiceOptions.cs` | `Server/Service/Support/service_options.hpp` | support | done | rid, HTTP endpoint, Redis endpoint/key prefix, channel endpoint, SPOT router/pub endpoint, evidence/log option을 읽는다. |
| `Server/FilteredService/*` | `Server/FilteredService/*`, `Server/Service/Support/service_host.hpp` | server-role | done | filtered service executable이 socket-filter profile로 공통 service host 구성을 재사용한다. |
| `Server/ThrowingService/*` | `Server/ThrowingService/*`, `Server/Service/Support/service_host.hpp` | server-role | done | throwing service executable이 monitoring handler 예외 profile로 공통 service host 구성을 재사용한다. |
| `Server/Trigger/RuntimeMonitoring.Trigger.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_runtime_monitoring_trigger` target이 있다. |
| `Server/Trigger/Program.cs` | `Server/Trigger/main.cpp` | server-role | done | trigger role 진입점은 `trigger_host_factory.hpp`의 host factory 실행만 담당한다. |
| `Server/Trigger/TriggerHostFactory.cs` | `Server/Trigger/trigger_host_factory.hpp` | server-role | done | Redis location store, client channel, evidence store, HTTP endpoint mapping을 구성한다. |
| `Server/Trigger/TriggerEndpoints.cs` | `Server/Trigger/trigger_host_factory.hpp`, `Server/Shared/evidence_store.hpp` | endpoint | done | health, evidence, routed request, throw stderr wait, validation, handshake failure endpoint route mapping은 trigger host factory에서 담당한다. |
| `Server/Trigger/TriggerHandlers.cs` | `Server/Trigger/trigger_handlers.hpp` | handler | done | profile request, service-a/service-b/throw routed request, throw stderr log, validation, handshake failure handler를 둔다. |
| `Server/Trigger/Support/TriggerClientRequests.cs` | `Server/Trigger/Support/trigger_client_requests.hpp` | support | done | transient channel request helper와 invalid handshake sender가 있다. 이 helper는 trigger role 내부에서만 쓰이며 client public surface로 노출하지 않는다. |
| `Server/Trigger/Support/TriggerLogReader.cs` | `Server/Trigger/Support/trigger_log_reader.hpp` | support | done | throw stderr wait helper가 있다. |
| `Server/Trigger/Support/TriggerSupport.cs` | `Server/Trigger/Support/trigger_options.hpp`, `Server/Shared/evidence_store.hpp` | support | done | trigger endpoint/log/channel/Redis option parser와 shared evidence store가 있다. |
| `Server/Trigger/Support/TriggerValidation.cs` | `Server/Trigger/Support/trigger_validation.hpp`, `Client/Scenarios/mon_b2_registration_validation_scenario.hpp` | support | done | public monitoring builder/framework 적용 오류를 endpoint로 실행하고 client가 결과를 단언한다. |

## Scenario 대응

| Scenario | C++ 대응 | 상태 |
|----------|----------|------|
| `MON-A1` | `Client/Scenarios/mon_a1_socket_events_scenario.hpp` | done |
| `MON-A2` | `Client/Scenarios/mon_a2_location_events_scenario.hpp`; `Server/Service/Handlers/service_event_recorders.hpp`; `run_e2e.sh`의 svc-a→svc-b 시작 순서 | done — 실제 svc-b payload와 안정 구간 무중복 검증 |
| `MON-A3` | `Client/Scenarios/mon_a3_spot_events_scenario.hpp` | done |
| `MON-A4` | `Client/Scenarios/mon_a4_availability_transition_scenario.hpp` | done |
| `MON-A5` | `Client/Scenarios/mon_a5_fixed_kinds_scenario.hpp` | done |
| `MON-B1` | `Client/Scenarios/mon_b_publish_monitoring_absence_scenario.hpp` | partial — compile-time public member 부재와 zero-target publish 뒤 snapshot·event 부재를 검사한다. 기존 Service API drift를 고친 뒤 process 증거와 막힌 remote target 검증이 필요하다. |
| `MON-B2` | `Client/Scenarios/mon_b_publish_monitoring_absence_scenario.hpp` | partial — local subscriber가 있는 publish 뒤 snapshot·event 부재를 검사한다. 기존 Service API drift를 고친 뒤 handler 단일 처리와 막힌 local target 검증이 필요하다. |
| `MON-C1` | `Client/Scenarios/mon_c1_dispatch_failure_scenario.hpp` | done |
| `MON-D1` | `Client/Scenarios/mon_d1_failure_recovery_scenario.hpp` | done |

## 검증

- 2026-07-08: `timeout 560s framework/languages/cpp/e2e/RuntimeMonitoring/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260708-133413-118111`
  - 의미: 당시 계약의 MON-A1~MON-D1 회귀 증거다. CA-D77에서 제거한 target별 publish 집계를
    검증한 MON-B1·MON-B2 결과는 현재 계약의 완료 증거로 사용하지 않는다.
- 2026-07-03: `framework/languages/cpp/e2e/RuntimeMonitoring/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260703-214231-52862`
  - 의미: Redis location store 기반 service, filtered service, throwing service와 trigger의 과거 회귀
    증거다. Public validation endpoint와 MON-D1의 잘못된 MeshName·observer capacity 검증은 유지한다.
