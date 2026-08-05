# C++ ResilienceLifecycle .NET 기준 포팅 inventory

기준 구현: `framework/languages/dotnet/e2e/ResilienceLifecycle`

현재 C++ `ResilienceLifecycle`은 Redis location store를 공유하는 Provider, Consumer, Client target과
runner 아래에서 public recovery 흐름을 검증한다. 별도 Registry role은 제거했다. Client target은
`.NET` 기준처럼 HTTP endpoint만 호출하고, framework channel client는 Consumer/Provider role 안에 둔다.
`.NET`의 `ResilienceProcessManager`가 맡는 provider 재시작, health 대기, 종료, 로그 저장 책임은
C++ runner가 같은 의미로 담당한다.
이는 언어별 harness 배치 차이일 뿐 scenario나 public 동작 차이가 아니다.

## 파일 매핑

| .NET 기준 파일 | C++ 대응 파일 | 분류 | 상태 | 비고 |
|----------------|---------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 실행 로그 제외 규칙만 있다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | 현재 gap 상태를 과장 없이 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 전용 role target을 빌드하고 Consumer HTTP smoke, RL-A1/A2/A3/A4/A5, RL-B1/B2/B3/B4/B5/B6, RL-C1/C2/C3/C4, RL-D1/D2/D3/D4/D5 slice를 실행한다. RL-A4는 green provider endpoint를 별도 process로 띄우고, RL-B1/B2/B4/B5/C1/C2/C3/D1/D3/D4/D5는 Consumer HTTP endpoint와 provider evidence로 실행한다. RL-C4는 Redis location store outage 중 established manual channel 유지와 Redis 복구 뒤 new-client recovery를 검증한다. |
| `Shared/ResilienceLifecycle.Shared.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | ResilienceLifecycle 전용 C++ target 묶음이 추가됐다. |
| `Shared/Messages.cs` | `Shared/resilience_lifecycle_contracts.hpp`, `Shared/resilience_lifecycle_messages.hpp` | shared | done | ResilienceLifecycle 전용 message file과 contract facade가 있고 profile request/reply/send/failure/status DTO가 `.NET`식 marker 필드를 지원한다. 내부 namespace, handler group, channel 이름도 ResilienceLifecycle 전용 값으로 정리했다. |
| `Client/ResilienceLifecycle.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | 전용 client target이 추가됐다. |
| `Client/Program.cs` | `Client/main.cpp`, `Client/Support/client_options.hpp` | client | done | HTTP-only client dispatcher가 있고 endpoint/scenario env 값을 전용 option 객체로 모은다. 낡은 `rm-*` selector는 제거했고 ResilienceLifecycle scenario 이름만 실행한다. |
| `Client/Support/ClientOptions.cs` | `Client/Support/client_options.hpp` | support | done | C++ runner가 env로 주입한 endpoint와 scenario 값을 전용 option 객체로 읽는다. |
| `Client/Support/LifecycleApiResult.cs` | `Client/Support/lifecycle_api_result.hpp`, `Client/Support/resilience_request_support.hpp` | support | done | provider evidence HTTP fetch/wait와 lifecycle request helper를 전용 support 파일로 분리했다. |
| `Client/Support/ResilienceProcessManager.cs` | `run_e2e.sh`; `Client/Support/client_support.hpp` | support | done | `.NET` client support가 담당하는 provider process 시작, health 대기, 종료, stdout/stderr 로그 저장 책임은 C++ runner의 `start_provider`, `stop_pid`, `kill_pid`, readiness 대기 함수가 맡는다. Redis는 runner가 loopback container로 시작한다. client helper는 scenario 동기화용 marker 파일을 처리한다. |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/scenario_assert.hpp` | support | done | assertion과 marker-file wait helper를 전용 support 파일로 분리했다. |
| `Client/Support/TopologyEntryResult.cs` | `Client/Support/topology_entry_result.hpp` | support | done | Consumer `/topology`와 `/topology/wait` response를 typed DTO로 fetch해 provider readiness를 검증한다. |
| `Client/Scenarios/RlA1ProviderRestartScenario.cs` | `Client/Scenarios/rl_a1_provider_restart_scenario.hpp`, `run_e2e.sh` | scenario | done | 같은 endpoint restart를 RL 전용 scenario와 runner orchestration으로 검증한다. |
| `Client/Scenarios/RlA2ProviderEndpointRemapScenario.cs` | `Client/Scenarios/rl_a2_provider_endpoint_remap_scenario.hpp`, `run_e2e.sh` | scenario | done | 다른 endpoint remap을 RL 전용 scenario와 runner orchestration으로 검증한다. |
| `Client/Scenarios/RlA3ReconnectStormScenario.cs` | `Client/Scenarios/rl_a3_reconnect_storm_scenario.hpp`; `run_e2e.sh` | scenario | done | 전용 client scenario가 Consumer HTTP `/profile/request/new-client`를 24번 호출하고, 각 reply의 provider id와 provider evidence marker를 검증한다. |
| `Client/Scenarios/RlA4DrainAndGreenEndpointScenario.cs` | `Client/Scenarios/rl_a4_drain_and_green_endpoint_scenario.hpp`, `run_e2e.sh` | scenario | done | provider B drain, green provider endpoint 시작, original provider shutdown, Consumer `/topology/wait` Ready 1, green evidence, green shutdown, original provider 복구, restored evidence를 `.NET` 순서로 검증한다. |
| `Client/Scenarios/RlA5ProviderFlappingScenario.cs` | `Client/Scenarios/rl_a5_provider_flapping_scenario.hpp`, `run_e2e.sh` | scenario | done | runner가 provider B stop/restart cycle을 담당하고, 전용 client scenario가 down window의 `api-a` 수렴, up window의 request 성공, provider B evidence prefix를 검증한다. |
| `Client/Scenarios/RlB1CancellationCleanupScenario.cs` | `run_e2e.sh`, `Server/Consumer/main.cpp`, `Client/Scenarios/rl_b1_cancellation_cleanup_scenario.hpp` | scenario | done | runner와 HTTP-only client scenario가 Consumer HTTP `/profile/request/timeout/100`으로 timeout 실패 payload를 확인하고, 같은 consumer의 `/profile/request` 후속 request 정상화를 검증한다. |
| `Client/Scenarios/RlB2CrashDuringInflightScenario.cs` | `Client/Scenarios/rl_b2_crash_during_inflight_scenario.hpp`; `run_e2e.sh` | scenario | done | Consumer HTTP slow request를 열고 provider B file evidence start marker를 확인한 뒤 provider B crash, Consumer `/topology/wait` Ready 0 수렴, in-flight request 실패, `api-a` follow-up, provider B 재기동 뒤 restored evidence를 검증한다. |
| `Client/Scenarios/RlB3GracefulShutdownScenario.cs` | `Client/Scenarios/rl_b3_graceful_shutdown_scenario.hpp`, `run_e2e.sh` | scenario | done | provider 정상 종료 뒤 남은 provider로 request가 성공하는지 RL 전용 scenario로 검증한다. |
| `Client/Scenarios/RlB4RuntimeDrainScenario.cs` | `Client/Scenarios/rl_b4_runtime_drain_scenario.hpp`; `run_e2e.sh` | scenario | done | 전용 client scenario가 provider B drain/restore, drained 신규 request `api-a` 수렴, provider B evidence 불변, provider A drained evidence, provider B restored evidence를 `.NET` 순서로 검증한다. |
| `Client/Scenarios/RlB5DrainInflightScenario.cs` | `Client/Scenarios/rl_b5_drain_inflight_scenario.hpp`; `run_e2e.sh` | scenario | done | 전용 client scenario가 Consumer HTTP slow request를 열고 실제 slow provider를 evidence file로 찾은 뒤 해당 provider drain/restore, healthy provider 신규 request, in-flight reply, drained provider evidence 불변, restored evidence를 `.NET` 순서로 검증한다. |
| `Client/Scenarios/RlB6GrayFaultScenario.cs` | `Client/Scenarios/rl_b6_gray_fault_scenario.hpp`; `run_e2e.sh` | scenario | done | provider B의 gray fault mode를 켠 뒤 gray request 실패와 healthy provider 성공을 함께 검증하고, fault mode 해제 뒤 follow-up request 정상화를 확인한다. |
| `Client/Scenarios/RlC1ClientHostLifecycleScenario.cs` | `Client/Scenarios/rl_c1_client_host_lifecycle_scenario.hpp`, `Server/Consumer/Endpoints/consumer_endpoints.hpp`, `run_e2e.sh` | scenario | done | 전용 client scenario가 Consumer HTTP `/profile/request/new-client`로 요청마다 transient client host를 만들어 request를 보내고, 반복 request와 cleanup follow-up marker가 provider evidence에 남는지 검증한다. |
| `Client/Scenarios/RlC2TopologyRecoveryScenario.cs` | `run_e2e.sh`, `Server/Consumer/Endpoints/consumer_endpoints.hpp` | scenario | done | provider crash 뒤 Consumer HTTP `/profile/request/new-client`가 `api-a`로 수렴하는지 확인하고, provider B 재기동 뒤 restored marker가 `api-b` evidence에 남는지 검증한다. |
| `Client/Scenarios/RlC3NodePauseRecoveryScenario.cs` | `Client/Scenarios/rl_c3_node_pause_recovery_scenario.hpp`, `run_e2e.sh`, `Server/Consumer/Endpoints/consumer_endpoints.hpp` | scenario | done | 전용 client scenario가 provider B `/shutdown`, Consumer HTTP `/profile/request`의 `api-a` 수렴, provider B 재기동 뒤 Consumer `/topology/wait` Ready 1, recovered marker가 `api-b` evidence에 남는지 검증한다. |
| `Client/Scenarios/RlC4RegistryOutageScenario.cs` | `Client/Scenarios/rl_c4_location_store_outage_scenario.hpp`; `Server/Consumer/Endpoints/consumer_endpoints.hpp`; `run_e2e.sh` | scenario | done | HTTP-only client가 Consumer endpoint를 호출하고, Consumer role의 established manual channel request가 Redis location store outage 중 계속 성공하는지 검증한다. Redis 복구와 provider A 재기동 뒤 Consumer new-client request와 provider evidence도 검증한다. |
| `Client/Scenarios/RlD1HighFanoutScenario.cs` | `run_e2e.sh`, `Server/Consumer/Endpoints/consumer_endpoints.hpp` | scenario | done | runner가 Consumer HTTP `/profile/request`로 120개 request burst를 만들고 provider evidence에서 `rl-d1-` marker가 남는지 검증한다. |
| `Client/Scenarios/RlD2ObserverFaultScenario.cs` | `Client/Scenarios/rl_d2_observer_fault_scenario.hpp` | scenario | done | provider observer fault mode를 켠 뒤 missing request dispatch error evidence, observer exception isolation, follow-up request evidence를 검증한다. |
| `Client/Scenarios/RlD3DispatchErrorEvidenceScenario.cs` | `Client/Scenarios/rl_d3_dispatch_error_evidence_scenario.hpp`; `run_e2e.sh`, `Server/Consumer/main.cpp`, `Server/Consumer/Endpoints/consumer_endpoints.hpp` | scenario | done | HTTP-only client와 runner가 Consumer HTTP `/profile/request/missing`, `/profile/command/missing`, `/profile/request`를 호출하고 provider flow log에서 missing request/send marker를 검증한다. |
| `Client/Scenarios/RlD4MissingRequestHandlerScenario.cs` | `Client/Scenarios/rl_d4_missing_request_handler_scenario.hpp`; `Server/Consumer/Endpoints/consumer_endpoints.hpp`; `run_e2e.sh` | scenario | done | 전용 client scenario가 Consumer HTTP `/profile/request/missing`을 호출하고 public failure payload와 provider dispatch error evidence를 검증한다. |
| `Client/Scenarios/RlD5MixedBurstScenario.cs` | — | scenario | deferred | 동시 다수 client, 수 분 지속, request/send 혼합, latency drift 관측을 갖춘 soak harness가 없어 공통 문서 규칙대로 보류한다. 이전 순차 mixed burst는 canonical RL-D5로 인정하지 않는다. |
| `Server/Provider/ResilienceLifecycle.Provider.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | provider role target이 추가됐다. |
| `Server/Provider/Program.cs` | `Server/Provider/main.cpp` | server-role | done | provider role 진입점이 있다. |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/provider_host_factory.hpp`, `Server/Provider/main.cpp` | server-role | done | provider framework, discovery, channel/route/http endpoint, handler group wiring은 factory header로 분리했고 main은 진입점과 logging만 담당한다. |
| `Server/Provider/ProviderEndpoints.cs` | `Server/Provider/Endpoints/provider_endpoints.hpp` | endpoint | done | evidence, shutdown, crash, drain, restore, weight 조회, weight wait, server-weight 호환 admin, observer/gray/none fault mode endpoint가 있다. runner의 shutdown/crash/drain/restore 경로는 `.NET`과 같은 endpoint 이름을 사용한다. |
| `Server/Provider/ProviderSupport.cs` | `Server/Provider/Configuration/provider_options.hpp`; `Server/Provider/Infrastructure/evidence_store.hpp`; `Server/Provider/Infrastructure/fault_state.hpp`; `Server/Provider/Infrastructure/server_weight_state.hpp` | support | done | provider option, evidence store, fault mode state, admin weight wait 상태를 전용 support 파일로 분리했다. |
| `Server/Provider/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Provider/Handlers/evidence_dispatch_error_observer.hpp`; `Server/Provider/Infrastructure/evidence_store.hpp`; `Server/Provider/Infrastructure/fault_state.hpp` | handler | done | message flow observer가 dispatch error를 evidence store에 기록하고 fault state가 `observer-throws`일 때 예외를 던진다. Provider host factory는 전용 observer helper를 설치한다. |
| `Server/Provider/Handlers/ProviderHandlers.cs` | `Server/Provider/Handlers/provider_handlers.hpp` | handler | done | request/send/slow handler가 있고, provider fault state가 `gray`일 때 gray request 실패와 `ProfileFault` evidence를 기록한다. |
| `Server/Consumer/ResilienceLifecycle.Consumer.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | consumer role target이 추가됐다. |
| `Server/Consumer/Program.cs` | `Server/Consumer/main.cpp` | server-role | done | consumer role 진입점이 있고, ResilienceLifecycle 전용 consumer configuration/endpoint wrapper를 사용한다. |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/consumer_host_factory.hpp`, `Server/Consumer/main.cpp`, `Server/Consumer/Configuration/consumer_options.hpp`, `Server/Consumer/Endpoints/consumer_endpoints.hpp` | server-role | done | long-running consumer HTTP host가 `/health`, `/profile/request`, `/profile/request/manual`, `/profile/request/timeout/100`, `/profile/request/missing`, `/profile/command`, `/profile/command/missing`, `/profile/request/new-client` endpoint를 제공한다. `/profile/request/new-client`는 `.NET`처럼 요청마다 새 client host를 만들고 별도 flow log를 남긴다. option 읽기, endpoint handler, host wiring은 ResilienceLifecycle 전용 파일로 분리했고, endpoint handler는 ResilienceLifecycle marker contract를 보존한다. runner는 smoke, RL-B1, RL-C1, RL-C2, RL-C3, RL-C4, RL-D1, RL-D3, RL-D4에서 이 HTTP 경로를 사용한다. |

## Scenario ID 대응

| Scenario ID | C++ 대응 | 상태 |
|-------------|----------|------|
| `RL-A1` | `Client/Scenarios/rl_a1_provider_restart_scenario.hpp`; `run_e2e.sh` | done |
| `RL-A2` | `Client/Scenarios/rl_a2_provider_endpoint_remap_scenario.hpp`; `run_e2e.sh` | done |
| `RL-A3` | `Client/Scenarios/rl_a3_reconnect_storm_scenario.hpp`; `run_e2e.sh` | done |
| `RL-A4` | `Client/Scenarios/rl_a4_drain_and_green_endpoint_scenario.hpp`; `run_e2e.sh` | done |
| `RL-A5` | `Client/Scenarios/rl_a5_provider_flapping_scenario.hpp`; `run_e2e.sh` | done |
| `RL-B1` | `run_e2e.sh`; `Server/Consumer/main.cpp`; `Client/Scenarios/rl_b1_cancellation_cleanup_scenario.hpp` | done |
| `RL-B2` | `Client/Scenarios/rl_b2_crash_during_inflight_scenario.hpp`; `run_e2e.sh` | done |
| `RL-B3` | `Client/Scenarios/rl_b3_graceful_shutdown_scenario.hpp`; `run_e2e.sh` | done |
| `RL-B4` | `Client/Scenarios/rl_b4_runtime_drain_scenario.hpp`; `run_e2e.sh` | done |
| `RL-B5` | `Client/Scenarios/rl_b5_drain_inflight_scenario.hpp`; `run_e2e.sh` | done |
| `RL-B6` | `Client/Scenarios/rl_b6_gray_fault_scenario.hpp`; `run_e2e.sh` | done |
| `RL-C1` | `Client/Scenarios/rl_c1_client_host_lifecycle_scenario.hpp`; `Server/Consumer/Endpoints/consumer_endpoints.hpp`; `run_e2e.sh` | done |
| `RL-C2` | `Server/Consumer/Endpoints/consumer_endpoints.hpp`; `run_e2e.sh` | done |
| `RL-C3` | `Client/Scenarios/rl_c3_node_pause_recovery_scenario.hpp`; `Server/Consumer/Endpoints/consumer_endpoints.hpp`; `run_e2e.sh` | done |
| `RL-C4` | `Client/Scenarios/rl_c4_location_store_outage_scenario.hpp`; `run_e2e.sh` | done |
| `RL-D1` | `run_e2e.sh`; `Server/Consumer/Endpoints/consumer_endpoints.hpp` | done |
| `RL-D2` | `Client/Scenarios/rl_d2_observer_fault_scenario.hpp` | done |
| `RL-D3` | `run_e2e.sh`; `Server/Consumer/main.cpp`; `Server/Consumer/Endpoints/consumer_endpoints.hpp` | done |
| `RL-D4` | `Client/Scenarios/rl_d4_missing_request_handler_scenario.hpp`; `Server/Consumer/Endpoints/consumer_endpoints.hpp`; `run_e2e.sh`; `test_cpp_framework_messaging.cpp` | done |
| `RL-D5` | — | deferred — 지속 부하 harness 대기 |

## 검증

> 아래 과거 로그의 `RL-D5 passed`는 당시 120회 순차 mixed burst 결과다. 공통 문서가 요구하는
> 지속 soak proof가 아니므로 현재 완료 근거로 사용하지 않는다.

- 2026-07-08: `timeout 560s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh all`
  - 결과: 통과, exit 0
  - 로그: `logs/20260708-133049-101113`
  - 의미: Redis location store 기반 Provider/Consumer/Client target으로 Consumer smoke, RL-A1,
    RL-A2, RL-A3, RL-A4, RL-A5, RL-B1, RL-B2, RL-B3, RL-B4, RL-B5, RL-B6, RL-C1,
    RL-C2, RL-C3, RL-C4, RL-D1, RL-D2, RL-D3, RL-D4, RL-D5가 통과했다. `RL-B2`의
    `kill -9`와 `RL-C2`의 `/admin/crash` SIGABRT는 시나리오가 의도한 failure injection으로만
    허용하고, cleanup 또는 일반 provider 종료에서 같은 비정상 종료가 나오면 runner가 실패하도록
    보강했다.
- 2026-07-03: `ZLINK_CPP_E2E_BUILD_DIR=/home/hep7/project/kairos/zlink/framework/languages/cpp/build-redis-vcpkg timeout 900s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260703-205544-18048`
  - 의미: Redis location store 기반 Provider/Consumer/Client target으로 Consumer smoke, RL-A1, RL-A2,
    RL-A3, RL-A4, RL-A5, RL-B1, RL-B2, RL-B3, RL-B4, RL-B5, RL-B6, RL-C1, RL-C2, RL-C3,
    RL-C4, RL-D1, RL-D2, RL-D3, RL-D4, RL-D5가 통과했다. 별도 registry role은 없고,
    RL-C4는 runner-owned Redis container pause/unpause로 location store outage와 recovery를 검증한다.

> 아래 2026-07-01 이전 기록은 Redis location store 전환 전의 과거 검증 근거다. 현재 구조 판단은
> 위 2026-07-03 기록과 파일 매핑을 기준으로 한다.

- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-184559-770532`
  - 의미: 현재 runner에 포함된 RL-A1, RL-A2, RL-A3, RL-A4, RL-A5, RL-B1, RL-B2, RL-B3,
    RL-B4, RL-B5, RL-B6, RL-C1, RL-C2, RL-C3, RL-C4, RL-D1, RL-D2, RL-D3, RL-D4, RL-D5 slice는
    통과한다. RL-C4는 registry outage 중 established channel 유지와 registry/provider A 재기동 뒤
    새 discovery client 복구까지 검증한다.
- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-214445-1305240`
  - 의미: Consumer role target을 추가하고 runner가 `/profile/request` smoke와 `consumer-flow.log`
    message-flow를 확인한 뒤, 기존 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-235157-1506017`
  - 의미: Client support를 `client_options.hpp`, `scenario_assert.hpp`, `lifecycle_api_result.hpp`,
    `topology_entry_result.hpp`로 분리하고, topology readiness 검증을 typed DTO fetch 경로로 바꾼 뒤
    RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-235704-1515536`
  - 의미: RL-A1, RL-A2, RL-B1, RL-B3, RL-D3를 ResilienceLifecycle 전용 scenario wrapper와 runner
    scenario 이름으로 실행한 뒤 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-000224-1525059`
  - 의미: RL-A3, RL-A4, RL-A5, RL-C1, RL-C2, RL-C3도 ResilienceLifecycle 전용 scenario wrapper와
    runner scenario 이름으로 실행한 뒤 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-001148-1542721`
  - 의미: RL-B1은 Consumer HTTP `/profile/request/timeout/100`과 후속 `/profile/request`로 검증하고,
    RL-D3는 Consumer HTTP `/profile/request/missing`, `/profile/command` 및 provider flow log의
    `handler_missing`/`reply_error`, `handler_missing`/`drop` marker로 검증한 뒤 RL-A/B/C/D slice
    전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_consumer`
  - 결과: 통과
  - 의미: Consumer role main이 ResilienceLifecycle 전용 `Configuration/consumer_options.hpp`와
    `Endpoints/consumer_endpoints.hpp` wrapper를 사용하도록 바꾼 뒤 consumer target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-003306-1573336`
  - 의미: Consumer configuration/endpoint wrapper 분리 뒤에도 Consumer smoke, RL-B1, RL-D3와
    RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider`
  - 결과: 통과
  - 의미: Provider dispatch error observer를 `Handlers/evidence_dispatch_error_observer.hpp`로 분리한 뒤
    provider target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-003804-1582520`
  - 의미: Provider dispatch error observer 분리 뒤에도 RL-D3 provider flow marker, RL-D2 observer fault
    isolation, Consumer smoke, RL-B1, RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_registry`
  - 결과: 통과
  - 의미: Registry host wiring을 `Server/Registry/registry_host_factory.hpp`로 분리한 뒤 registry target이
    빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-004138-1590362`
  - 의미: Registry host factory 분리 뒤에도 Consumer smoke, RL-A/B/C/D slice 전체와 registry outage/recovery
    검증이 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider`
  - 결과: 통과
  - 의미: Provider host wiring을 `Server/Provider/provider_host_factory.hpp`로 분리한 뒤 provider target이
    빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-004544-1598577`
  - 의미: Provider host factory 분리 뒤에도 Consumer smoke, RL-D2/RL-D3 observer/dispatch evidence,
    provider restart/drain, registry outage/recovery를 포함한 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider`
  - 결과: 통과
  - 의미: Provider observer fault mode를 `Server/Provider/Infrastructure/fault_state.hpp`로 분리한 뒤
    provider target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-005337-1614770`
  - 의미: Provider observer fault mode state 분리 뒤에도 RL-D2 observer fault isolation, RL-D3 dispatch
    evidence, Consumer smoke, provider restart/drain, registry outage/recovery를 포함한 RL-A/B/C/D slice
    전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider`
  - 결과: 통과
  - 의미: Provider evidence state를 `.NET`의 `EvidenceStore` 역할에 맞춰
    `Server/Provider/Infrastructure/evidence_store.hpp`로 분리한 뒤 provider target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-005924-1624879`
  - 의미: Provider evidence store 분리 뒤에도 RL-D2 observer fault isolation, RL-D3 dispatch evidence,
    Consumer smoke, provider restart/drain, registry outage/recovery를 포함한 RL-A/B/C/D slice 전체가
    다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_registry`
  - 결과: 통과
  - 의미: Registry evidence store와 fault state를 `Server/Registry/Infrastructure/`로 분리하고
    `/evidence`, `/evidence/clear`, `/topology` alias를 추가한 뒤 registry target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-010541-1635780`
  - 의미: Registry evidence/fault infrastructure와 evidence endpoint 추가 뒤에도 Consumer smoke,
    RL-D2/RL-D3 dispatch evidence, provider restart/drain, registry outage/recovery를 포함한 RL-A/B/C/D
    slice 전체가 다시 통과했다.
- 2026-07-01: focused registry handler check
  - 결과: 당시 통과
  - 로그: `logs/focused-registry-1645959`
  - 의미: 전용 ResilienceLifecycle selector 정리 전의 중간 검증이다. 현재 완료 근거로는 사용하지 않고,
    최신 완료 판정은 아래 `logs/20260701-173140-37072` full runner와 전용 `rl-*` selector 검증을 따른다.
- 2026-07-01: focused marker contract check
  - 결과: 통과
  - 로그: `logs/focused-marker-1656661`
  - 의미: profile request의 `value=manual`, `marker=manual-marker`를 분리해 보내고 reply marker와
    registry evidence `profile-request|rid=api-a|marker=manual-marker|value=manual`을 확인했다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-011221-1647207`
  - 의미: 선택적 Registry profile handler와 dispatch error observer 추가 뒤에도 기본 runner 경로의
    Consumer smoke, RL-D2/RL-D3 dispatch evidence, provider restart/drain, registry outage/recovery를
    포함한 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-011837-1657861`
  - 의미: profile request/reply/send DTO에 marker 필드를 추가하고 Provider/Registry handler가 marker를
    우선 evidence marker로 쓰도록 바꾼 뒤에도 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_consumer`
  - 결과: 통과
  - 의미: Consumer `/profile/request/new-client` endpoint가 transient client host를 생성하도록 추가한 뒤
    consumer target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-012511-1669180`
  - 의미: Consumer HTTP `/profile/request/new-client`가 별도 `storm-rl-c1-new-client-flow.log` message-flow를
    남기고 성공한 뒤에도 Consumer smoke, RL-B1, RL-D3, provider restart/drain, registry outage/recovery를
    포함한 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_consumer -j 4`
  - 결과: 통과
  - 의미: Consumer endpoint handler를 ResilienceLifecycle 전용 contract로 분리하고 `/profile/command`를
    정상 `ProfileMsg` send 경로로 맞춘 뒤 consumer target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-021730-1750031`
  - 의미: Consumer HTTP 경로가 marker를 보존하고, RL-D1 request burst, RL-D4 missing request, RL-D5
    request/send mixed burst evidence를 검증한 뒤에도 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-031111-1848503`
  - 의미: RL-C1 반복 new-client/cleanup, RL-C2 provider crash 뒤 `api-a` 수렴과 `api-b` restored evidence,
    RL-C3 provider down 중 `api-a` 수렴과 recovery 뒤 `api-b` evidence를 `.NET`처럼 Consumer HTTP 경로로
    검증한 뒤에도 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-044837-46874`
  - 의미: 낡은 `rm_*` client scenario 파일과 selector를 제거하고 RL-A1/RL-A2/RL-B1/RL-B3/RL-D3
    client scenario를 전용 구현으로 정리한 뒤에도 RL-A/B/C/D slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_registry zlink_cpp_e2e_resilience_lifecycle_provider zlink_cpp_e2e_resilience_lifecycle_consumer zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: ResilienceLifecycle shared message file, namespace, handler group, channel 이름, trace label,
    Consumer option reader를 전용 이름으로 정리한 뒤에도 모든 ResilienceLifecycle role target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-045858-64326`
  - 의미: ResilienceLifecycle 내부에 남아 있던 낡은 cross-config namespace/include/file 이름과 channel
    이름을 전용 이름으로 정리한 뒤에도 Consumer smoke, RL-C1 consumer new-client, RL-A/B/C/D
    slice 전체가 다시 통과했다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: provider gray fault endpoint/handler와 RL-B6 전용 client scenario 추가 뒤 provider/client target이 빌드됐다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-052142-87471`
  - 의미: RL-B6가 `.NET`처럼 gray fault mode에서 일부 request 실패와 healthy provider 성공을 함께 검증하고, fault 해제 뒤 follow-up request 정상화를 확인한다. full runner 출력은 `scenario RL-B6 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: RL-D4/RL-D5 전용 client scenario header와 consumer HTTP endpoint option 추가 뒤 client target이 빌드됐다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-052729-98838`
  - 의미: RL-D4/RL-D5를 shell-only 검증이 아니라 전용 client scenario로 실행한 뒤에도 full runner가 통과했다. `client-rl-d4.stdout.log`와 `client-rl-d5.stdout.log`는 각각 `scenario RL-D4 passed`, `scenario RL-D5 passed`를 기록한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider zlink_cpp_e2e_resilience_lifecycle_consumer -j 4`
  - 결과: 통과
  - 의미: Consumer host wiring을 `consumer_host_factory.hpp`로 분리하고 Provider admin endpoint를 drain/restore/weight/wait 이름으로 맞춘 뒤 provider/consumer target이 빌드된다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_consumer -j 4`
  - 결과: 통과
  - 의미: `/profile/request/new-client` transient client host trace 파일을 request marker 기준으로 분리한 뒤 consumer target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-053902-21173`
  - 의미: runner가 Provider drain/restore/weight wait endpoint 이름을 `.NET`과 맞춘 뒤에도 RL-A/B/C/D slice 전체가 통과했다. `storm-rl-c1-*.log`와 `storm-rl-c2-after-crash-*.log`가 marker별로 남고, 출력은 `scenario RL-C1 passed`, `scenario RL-C2 passed`, `scenario RL-D4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_registry -j 4`
  - 결과: 통과
  - 의미: Registry topology response DTO를 `topology_entry_result.hpp`로 분리한 뒤 registry target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-054314-32709`
  - 의미: Registry topology response DTO 분리 뒤에도 topology readiness, registry outage/restart, RL-A/B/C/D slice 전체가 통과했다. 출력은 `scenario RL-C4 passed`, `scenario RL-D4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_registry -j 4`
  - 결과: 통과
  - 의미: Registry `/topology/wait`, `/shutdown` endpoint와 dispatch error evidence field 확장 뒤 registry target이 빌드된다.
- 2026-07-01: focused registry admin check
  - 결과: 통과
  - 로그: `logs/focused-registry-admin-2`
  - 의미: Registry 단독 실행에서 `/topology/wait`가 조건 미충족 시 504를 반환하고 `/shutdown` 호출 뒤 프로세스가 정상 종료된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-054957-45475`
  - 의미: Registry topology wait/shutdown endpoint와 dispatch error evidence field 확장 뒤에도 RL-A/B/C/D slice 전체가 통과했다. 출력은 `scenario RL-C4 passed`, `scenario RL-D4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: RL-B5 in-flight drain 검증을 `rl_b5_drain_inflight_scenario.hpp`로 분리한 뒤 client target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-055650-60628`
  - 의미: RL-B5 전용 header 분리 뒤에도 RL-B5/B4/A4 drain slice와 RL-A/B/C/D 전체가 통과했다. 출력은 `scenario RL-B5 passed`, `scenario RL-B4 passed`, `scenario RL-A4 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-060053-70377`
  - 의미: RL-C3에서 provider B 재기동 뒤 Registry `/topology/wait` DTO가 `api-b` Ready 항목 1개를 반환하는지 확인한 뒤에도 RL-A/B/C/D 전체가 통과했다. 출력은 `scenario RL-C3 passed`, `scenario RL-C4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: RL-A3가 Consumer HTTP `/profile/request/new-client` 24회 storm과 provider evidence 검증을 전용 client scenario 안에서 수행하도록 바꾼 뒤 client target이 빌드된다.
- 2026-07-02: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_consumer zlink_cpp_e2e_resilience_lifecycle_client`
  - 결과: 통과
  - 의미: C++ client를 HTTP-only dispatcher로 바꾸고 Consumer HTTP endpoint에 established manual request와 missing command 경로를 추가한 뒤 consumer/client target이 빌드된다.
- 2026-07-02: `timeout 420s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260702-072155-43811`
  - 의미: HTTP-only client가 Consumer/Registry/Provider HTTP endpoint만 호출하고, framework channel client는 Consumer/Provider/Registry role 안에서 실행된다. Consumer smoke, RL-A1, RL-A2, RL-A3, RL-A4, RL-A5, RL-B1, RL-B2, RL-B3, RL-B4, RL-B5, RL-B6, RL-C1, RL-C2, RL-C3, RL-C4, RL-D1, RL-D2, RL-D3, RL-D4, RL-D5가 통과했다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-060806-82776`
  - 의미: RL-A3가 `.NET`처럼 전용 client scenario 안에서 24회 new-client storm을 검증한 뒤에도 RL-A/B/C/D 전체가 통과했다. 출력은 `scenario RL-A3 passed`, `scenario RL-C3 passed`, `scenario RL-C4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: RL-A5가 `.NET`처럼 down/up window별 Consumer HTTP request와 provider evidence를 전용 client scenario 안에서 검증하도록 바꾼 뒤 client target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-061252-93278`
  - 의미: RL-A5가 provider B flapping cycle마다 down window `api-a` 수렴, registry topology wait, up window provider B evidence를 검증한 뒤에도 RL-A/B/C/D 전체가 통과했다. 출력은 `scenario RL-A5 passed`, `scenario RL-C3 passed`, `scenario RL-C4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_registry zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: Registry topology state를 `.NET`과 같은 `Ready`로 맞추고 RL-B2 topology wait도 같은 상태 이름을 쓰도록 바꾼 뒤 registry/client target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-065522-73645`
  - 의미: RL-B2가 `.NET`처럼 Consumer HTTP slow request, provider B start evidence, provider crash, topology Ready 0, in-flight failure, `api-a` follow-up, provider B restored evidence를 검증한다. RL-C2도 provider `/admin/crash`, topology Ready 0, `api-a` after-crash request, provider B restored evidence를 확인하며, full runner 출력은 `scenario RL-B2 passed`, `scenario RL-C2 passed`, `scenario RL-C1 passed`, `scenario RL-A5 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_provider zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: Provider `/shutdown`, RL-A4 green provider endpoint 검증, RL-C1 전용 client scenario 추가 뒤 provider/client target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-070604-95553`
  - 의미: RL-A4가 `.NET`처럼 provider B drain, green provider endpoint 시작, original provider shutdown, topology Ready 1, green evidence, green shutdown, original provider 복구, restored evidence를 검증한다. RL-C1도 전용 client scenario에서 반복 new-client request와 cleanup follow-up evidence를 검증하며, full runner 출력은 `scenario RL-A4 passed`, `scenario RL-C1 passed`, `scenario RL-A5 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: RL-B4 runtime drain과 RL-C3 node pause/recovery를 전용 client scenario로 분리한 뒤 client target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-071616-20746`
  - 의미: RL-B4가 `.NET`처럼 provider B drain/restore, drained 신규 request의 `api-a` 수렴, provider B evidence 불변, provider A drained evidence, provider B restored evidence를 검증한다. RL-C3도 provider B `/shutdown`, surviving provider request, provider B 재기동 뒤 topology Ready 1, recovered evidence를 전용 client scenario에서 검증하며, full runner 출력은 `scenario RL-B4 passed`, `scenario RL-B5 passed`, `scenario RL-C3 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: RL-B5가 runner marker 없이 Consumer HTTP slow request와 provider drain/restore를 전용 client scenario 안에서 수행하도록 바꾼 뒤 client target이 빌드된다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-173140-37072`
  - 의미: RL-B5가 `.NET`처럼 Consumer HTTP slow request, 실제 slow provider 식별, drained provider 신규 request 차단, in-flight reply 완료, restore 뒤 recovered evidence를 전용 client scenario에서 검증한다. full runner 출력은 `scenario RL-B5 passed`, `scenario RL-C3 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_resilience_lifecycle_consumer zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: Consumer `/profile/request/new-client` transient client host가 hosted service 객체 수명에 결과 보관을 의존하지 않도록 외부 result state로 분리한 뒤 consumer/client target이 빌드된다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_framework zlink_cpp_e2e_resilience_lifecycle_consumer zlink_cpp_e2e_resilience_lifecycle_client -j 4`
  - 결과: 통과
  - 의미: nested `app_t`가 외부 Consumer host의 handler coroutine executor를 종료하지 않도록 framework runtime executor shutdown을 owner-count 기반으로 바꾼 뒤 framework와 ResilienceLifecycle consumer/client target이 빌드된다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target test_cpp_framework_app_host -j 4 && ./framework/languages/cpp/build/test_cpp_framework_app_host`
  - 결과: 통과
  - 의미: HTTP handler 안에서 nested `app_t`를 실행한 뒤 외부 app이 후속 HTTP request를 계속 처리하는 regression을 검증한다.
- 2026-07-01: `timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-173140-37072`
  - 의미: local port readiness는 3초 기준으로 유지하고, scenario marker 대기는 별도 scenario event 대기로 분리한 상태에서 RL-A3/RL-C1 new-client storm, provider recovery, registry outage, RL-D slice까지 전체 runner가 다시 통과했다. full runner 출력은 `scenario RL-A3 passed`, `scenario RL-C1 passed`, `scenario RL-C4 passed`, `scenario RL-D5 passed`, `resilience-lifecycle e2e result=passed`를 포함한다.
- 2026-07-01: `ZLINK_CPP_E2E_BUILD_DIR=framework/languages/cpp/build timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-173140-37072`
  - 의미: Consumer HTTP request helper 이름에서 retry 오해를 제거해 단일 request helper(`request_profile_once`)로 정리하고,
    provider crash 뒤 local health-down 대기를 3초로 낮춘 상태에서도 RL-A/B/C/D 전체 runner가 통과했다.
    scenario marker, topology wait, evidence wait는 장애/복구 event 검증용 대기이며 local port readiness는 3초 기준을 유지한다.
- 2026-07-01: `ZLINK_CPP_E2E_BUILD_DIR=framework/languages/cpp/build timeout 1200s framework/languages/cpp/e2e/ResilienceLifecycle/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-173140-37072`
  - 의미: active client/provider code에서 남아 있던 낡은 cross-config selector/marker 잔재를 제거한 뒤에도
    RL-A/B/C/D 전체 runner가 통과했다. 현재 active selector와 message marker는 ResilienceLifecycle 전용 이름만
    사용한다.
