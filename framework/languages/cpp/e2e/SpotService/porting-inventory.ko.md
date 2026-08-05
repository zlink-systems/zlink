# C++ SpotService E2E porting inventory

이 문서는 `.NET` SpotService E2E 파일이 현재 C++ SpotService E2E에서 어디에 대응되는지 기록한다.
`status`가 `gap`인 행은 구현 검증 또는 파일 분류가 아직 완료 판정에 부족한 항목이다.

| .NET 기준 파일 | C++ 대응 파일 | 분류 | 상태 | 비고 |
|----------------|---------------|------|------|------|
| `.gitignore` | `.gitignore` | support | done | 로그 산출물 제외 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | Redis location store를 준비한 뒤 play/session/gateway/multi-node/requester/client 프로세스를 시작하고 route-ready probe 뒤 `all` 실행을 검증한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | C++ public API로 구현한 항목과 남은 gap을 구분한다. |
| `Shared/Messages.cs` | `Shared/spot_service_contracts.hpp` | shared | done | payload, evidence, stream message DTO 대응 |
| `Shared/SpotService.Shared.csproj` | `CMakeLists.txt` | build | not-needed | C++는 상위 CMake target에 통합된다. |
| `Client/SpotService.Client.csproj` | `CMakeLists.txt` | build | not-needed | C++는 `zlink_cpp_e2e_spot_service_client` target으로 빌드된다. |
| `Client/Program.cs` | `Client/main.cpp` | client | done | `.NET`과 같이 외부 client는 HTTP client와 stream connector만 사용한다. route client와 spot route 요청은 server HTTP endpoint 뒤에 둔다. |
| `Client/Support/ClientOptions.cs` | `run_e2e.sh`, `Client/Support/client_options.hpp`, `Client/main.cpp` | support | done | `.NET`은 CLI argument parser로 endpoint와 scenario 값을 받지만, C++는 runner env 주입과 HTTP/stream endpoint option 객체로 같은 실행 계약을 유지한다. |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/client_support.hpp` | support | done | `ensure(...)` helper와 scenario별 예외로 대응 |
| `Client/Support/SpotLifecycleOrderContext.cs` | `Client/Support/spot_lifecycle_order_context.hpp`, `Client/main.cpp` | support | done | `.NET`의 shared spot id/current value context를 C++ grouped mode에서 같은 의미로 유지한다. |
| `Client/Scenarios/SmA1Scenario.cs` | `Client/Scenarios/sm_a1_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A1 |
| `Client/Scenarios/SmA2Scenario.cs` | `Client/Scenarios/sm_a2_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A2 |
| `Client/Scenarios/SmA3Scenario.cs` | `Client/Scenarios/sm_a3_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A3 |
| `Client/Scenarios/SmA4Scenario.cs` | `Client/Scenarios/sm_a4_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A4 |
| `Client/Scenarios/SmA5Scenario.cs` | `Client/Scenarios/sm_a5_scenario.hpp`, `Server/Play/Spots/play_actor_model.hpp`, `run_e2e.sh` | scenario | done | `.NET`의 app-level `ScenarioStage` 의미를 C++ user spot handler와 public timer API 위에서 검증한다. |
| `Client/Scenarios/SmA6Scenario.cs` | `Client/Scenarios/sm_a6_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A6 |
| `Client/Scenarios/SmA7Scenario.cs` | `Client/Scenarios/sm_a7_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A7 |
| `Client/Scenarios/SmA8Scenario.cs` | `Client/Scenarios/sm_a8_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-A8. `/spot/worker/start`와 `/spot/worker/complete`로 spot-level worker offload와 interleaved state request evidence를 검증한다. |
| `Client/Scenarios/SmB1Scenario.cs` | `Client/Scenarios/sm_b1_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B1 |
| `Client/Scenarios/SmB2Scenario.cs` | `Client/Scenarios/sm_b2_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B2 |
| `Client/Scenarios/SmB3Scenario.cs` | `Client/Scenarios/sm_b3_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B3 |
| `Client/Scenarios/SmB4Scenario.cs` | `Client/Scenarios/sm_b4_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B4 |
| `Client/Scenarios/SmB5Scenario.cs` | `Client/Scenarios/sm_b5_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B5 |
| `Client/Scenarios/SmB6Scenario.cs` | `Client/Scenarios/sm_b6_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B6 |
| `Client/Scenarios/SmB7Scenario.cs` | `Client/Scenarios/sm_b7_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B7 |
| `Client/Scenarios/SmB8Scenario.cs` | `Client/Scenarios/sm_b8_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-B8 |
| `Client/Scenarios/SmB9Scenario.cs` | `Client/Scenarios/sm_b9_scenario.hpp`, `Client/main.cpp`, `Server/Play/Spots/play_actor_model.hpp`, `run_e2e.sh` | scenario | done | user spot admission 허용/거부를 public actor join 결과와 evidence로 검증한다. |
| `Client/Scenarios/SmC1Scenario.cs` | `Client/Scenarios/sm_c1_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-C1 |
| `Client/Scenarios/SmC2Scenario.cs` | `Client/Scenarios/sm_c2_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-C2 |
| `Client/Scenarios/SmC3Scenario.cs` | `Client/Scenarios/sm_c3_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-C3 |
| `Client/Scenarios/SmC4Scenario.cs` | `Client/Scenarios/sm_c4_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-C4 |
| `Client/Scenarios/SmC5Scenario.cs` | `Client/Scenarios/sm_c5_scenario.hpp`, `Client/main.cpp`, `Server/Play/Endpoints/spot_interaction_endpoints.hpp`, `run_e2e.sh` | scenario | done | cross-node SpotMesh publish가 target spot subscriber evidence에 남는지 검증한다. |
| `Client/Scenarios/SmD1Scenario.cs` | `Client/Scenarios/sm_d1_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D1 |
| `Client/Scenarios/SmD2Scenario.cs` | `Client/Scenarios/sm_d2_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D2 |
| `Client/Scenarios/SmD3Scenario.cs` | `Client/Scenarios/sm_d3_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D3 |
| `Client/Scenarios/SmD4Scenario.cs` | `Client/Scenarios/sm_d4_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D4 |
| `Client/Scenarios/SmD5Scenario.cs` | `Client/Scenarios/sm_d5_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D5 |
| `Client/Scenarios/SmD6Scenario.cs` | `Client/Scenarios/sm_d6_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D6 |
| `Client/Scenarios/SmD7Scenario.cs` | `Client/Scenarios/sm_d7_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D7 |
| `Client/Scenarios/SmD8Scenario.cs` | `Client/Scenarios/sm_d8_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D8 |
| `Client/Scenarios/SmD9Scenario.cs` | `Client/Scenarios/sm_d9_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D9 |
| `Client/Scenarios/SmD10Scenario.cs` | `Client/Scenarios/sm_d10_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D10 |
| `Client/Scenarios/SmD11Scenario.cs` | `Client/Scenarios/sm_d11_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D11 |
| `Client/Scenarios/SmD12Scenario.cs` | `Client/Scenarios/sm_d12_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-D12 |
| `Client/Scenarios/SmD13Scenario.cs` | `Client/Scenarios/sm_d13_scenario.hpp`, `run_e2e.sh`, `feature-map.ko.md` | scenario | done | `.NET`과 같은 heartbeat-enabled stream 유지 경로를 검증하고, 후속 actor request와 evidence를 focused run으로 확인했다. |
| `Client/Scenarios/SmD14Scenario.cs` | `Client/Scenarios/sm_d14_scenario.hpp`, `Server/Session/session_host_factory.hpp`, `run_e2e.sh`, `feature-map.ko.md` | scenario | done | public stream node TLS server 설정과 stream connector strict rejection/skip-validation 성공 경로로 bind, relay, push를 검증한다. |
| `Client/Scenarios/SmD15Scenario.cs` | `Client/Scenarios/sm_d15_scenario.hpp`, `Server/Gateway/gateway_host_factory.hpp`, `run_e2e.sh`, `feature-map.ko.md` | scenario | done | gateway HTTP request가 public actor client를 통해 actor handler에 도달하고 bound-session push notify가 client stream에 도착하는지 검증한다. |
| `Client/Scenarios/SmE1Scenario.cs` | `Client/Scenarios/sm_e1_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-E1 |
| `Client/Scenarios/SmE2Scenario.cs` | `Client/Scenarios/sm_e2_scenario.hpp`, `Server/Play/Spots/play_actor_model.hpp`, `run_e2e.sh` | scenario | done | SM-E2 public spot timer tick evidence |
| `Client/Scenarios/SmE3Scenario.cs` | `Client/Scenarios/sm_e3_scenario.hpp`, `Server/Play/Spots/play_actor_model.hpp`, `Server/Play/Handlers/play_spot_route_handlers.hpp`, `run_e2e.sh` | scenario | done | SM-E3 public spot create lifecycle에서 idle timer를 등록하고 timer handler의 public close와 닫힌 spot request 실패를 검증한다. |
| `Client/Scenarios/SmE4Scenario.cs` | `Client/Scenarios/sm_e4_scenario.hpp`, `Server/Play/Spots/play_actor_model.hpp`, `Server/Play/Handlers/play_spot_route_handlers.hpp`, `run_e2e.sh` | scenario | done | SM-E4 public timer overrun policy와 tick delivery/scheduled/skipped evidence를 검증한다. |
| `Client/Scenarios/SmF1Scenario.cs` | `Client/Scenarios/sm_f1_scenario.hpp`, `Client/main.cpp`, `Server/Play/Endpoints/spot_interaction_endpoints.hpp` | scenario | done | HTTP client가 `/spot/direct`와 `/spot/direct-command`를 호출하고, Play role endpoint가 public route client로 request/send를 보낸다. |
| `Client/Scenarios/SmF2Scenario.cs` | `Client/Scenarios/sm_f2_scenario.hpp`, `Client/main.cpp`, `Server/Play/Endpoints/spot_interaction_endpoints.hpp` | scenario | done | HTTP client가 Play role endpoint를 호출하고, server-owned public route client가 cross-node spot request/send를 수행한다. |
| 공통 E2E `SM-F3` | `Client/Scenarios/sm_f3_scenario.hpp`, `Client/main.cpp` | scenario | done | `.NET`에는 별도 scenario 파일이 없지만 feature-map과 공통 Config 2에 있는 SM-F3를 C++ scenario header로 분리했다. 같은 route mesh channel에서 일반 route request와 target spot route request가 함께 처리되는지 검증한다. |
| `Client/Scenarios/SmF4Scenario.cs` | `Client/Scenarios/sm_f4_scenario.hpp`, `Client/main.cpp` | scenario | done | SM-F4 |
| 공통 E2E `SM-F5` | `Client/Scenarios/sm_f5_scenario.hpp`, `Client/main.cpp` | scenario | done | `.NET`에는 별도 scenario 파일이 없지만 feature-map과 공통 Config 2에 있는 SM-F5를 C++ scenario header로 분리했다. spot route negative 뒤 같은 route channel의 일반 route request와 target spot route request가 계속 성공하는지 검증한다. |
| `Client/Scenarios/SmF6Scenario.cs` | `Client/Scenarios/sm_f6_scenario.hpp`, `Server/MultiNode/`, `run_e2e.sh` | scenario | done | RouteMesh를 끈 MultiNode SpotMesh-only role에서 remote spot request/send와 actor join을 검증한다. |
| `Client/Scenarios/SmG1Scenario.cs` | `Client/Scenarios/sm_g1_scenario.hpp`, `Client/main.cpp`, `run_e2e.sh` | scenario | done | SM-G1 crash/restart evidence |
| `Client/Scenarios/SmG2Scenario.cs` | `Client/Scenarios/sm_g2_scenario.hpp` | scenario | done | SM-G2 |
| `Client/Scenarios/SmG3Scenario.cs` | `Client/Scenarios/sm_g3_scenario.hpp` | scenario | done | `.NET`처럼 두 stream client를 먼저 순차 auth/bind한 뒤 ping/leave만 동시에 실행한다. 이전 C++ 구현은 auth/join까지 동시에 실행해 session `StreamBound` evidence가 중복될 수 있었다. |
| `Client/Scenarios/SmG4Scenario.cs` | `Client/Scenarios/sm_g4_scenario.hpp` | scenario | done | SM-G4 |
| `Client/Scenarios/SmQ9Scenario.cs` | `Client/Scenarios/sm_q9_scenario.hpp`, `Server/MultiNode/`, `Server/MultiNodeRequester/`, `run_e2e.sh` | scenario | done | `.NET`의 multi-node route-to-spot 흐름에 대응해 외부 client는 HTTP만 호출한다. C++ requester role은 server-side public route client와 bridge-only spot node를 소유해 multi-node spot state request를 보낸다. |
| `Server/Registry/Program.cs` | 제거됨 | server-role | not-needed | 공통 Config 2는 location store 기준이다. C++ runner는 registry role을 띄우지 않고 Redis location store를 공유한다. |
| `Server/Registry/RegistryHostFactory.cs` | 제거됨 | server-role | not-needed | registry host factory는 location store 전환 후 필요하지 않다. |
| `Server/Registry/RegistrySupport.cs` | `Server/Shared/Support/location_store.hpp`, `Server/Shared/Support/env.hpp` | support | done | role별 host factory가 Redis endpoint/key prefix env를 읽어 `redis_location_store_t`를 등록한다. |
| `Server/Registry/SpotService.Registry.csproj` | 제거됨 | build | not-needed | C++ SpotService registry target은 제거됐다. |
| `Server/Play/Program.cs` | `Server/Play/main.cpp` | server-role | done | play role 진입점 |
| `Server/Play/PlayHostFactory.cs` | `Server/Play/play_host_factory.hpp` | server-role | done | play host factory 책임을 role-local header로 분리했다. |
| `Server/Play/PlaySupport.cs` | `Server/Shared/Support/env.hpp`, `Server/Shared/Support/codecs.hpp`, `Server/Shared/Endpoints/evidence_endpoint.hpp`, `Server/Shared/scenario_state.hpp` | support | done | env, codec, evidence snapshot, scenario state 책임을 shared support/endpoint 파일로 분리했다. |
| `Server/Play/SpotService.Play.csproj` | `CMakeLists.txt` | build | not-needed | C++는 상위 CMake target에 통합된다. |
| `Server/Play/Endpoints/OperationalEndpoints.cs` | `Server/Play/Endpoints/operational_endpoints.hpp`, `Server/Shared/Endpoints/evidence_endpoint.hpp`, `Server/Shared/Handlers/channel_control_ping_handler.hpp` | endpoint | done | health/evidence/evidence-wait/control-ping/shutdown/crash mapping을 endpoint 파일로 분리했고, `/evidence/wait`는 SM-A6 focused run에서 직접 검증했다. |
| `Server/Play/Endpoints/SpotFailureEndpoints.cs` | `Server/Play/Endpoints/spot_failure_endpoints.hpp`, `Server/Play/Handlers/play_spot_route_handlers.hpp` | endpoint | done | slow, missing-handler request/command, missing-target, missing-route, spot-to-spot timeout/negative endpoint mapping을 endpoint 파일로 분리했고 SM-E1 focused run에서 missing-handler/missing-target endpoint를 직접 검증했다. |
| `Server/Play/Endpoints/SpotInteractionEndpoints.cs` | `Server/Play/Endpoints/spot_interaction_endpoints.hpp`, `Server/Play/Handlers/play_actor_handlers.hpp`, `Server/Play/Handlers/play_spot_route_handlers.hpp` | endpoint | done | 구현된 spot interaction endpoint mapping과 publish-wait endpoint는 endpoint 파일로 분리했고 SM-C4 focused run에서 `/spot/publish/wait`를 직접 검증했다. idle-close endpoint는 SM-E3 focused run에서 검증했고 overrun timer endpoint는 SM-E4 focused run에서 검증했다. `/spot/worker/start`와 `/spot/worker/complete`는 SM-A8 focused run에서 검증했다. `/spot/stage/request`, `/spot/stage/timer`, `/spot/direct-command`는 server-owned public API bridge로 검증했다. |
| `Server/Play/Endpoints/SpotLifecycleEndpoints.cs` | `Server/Play/Endpoints/spot_lifecycle_endpoints.hpp`, `Server/Play/Handlers/play_control_handlers.hpp`, `Server/Play/Handlers/play_spot_route_handlers.hpp` | endpoint | done | lifecycle create/alternate/close/type-mismatch endpoint mapping은 endpoint 파일로 분리했다. C++의 state request/command route mapping은 interaction endpoint 파일에 둔다. |
| `Server/Play/Handlers/PlayActorHandlers.cs` | `Server/Play/Handlers/play_actor_handlers.hpp`, `Server/Play/Spots/play_actor_model.hpp` | handler | done | actor/channel HTTP bridge와 channel handlers는 handler 파일로 분리했고, spot actor packet handler 구현은 role-local spot model에 유지했다. |
| `Server/Play/Handlers/PlayControlHandlers.cs` | `Server/Play/Handlers/play_control_handlers.hpp`, `Server/Shared/Handlers/channel_control_ping_handler.hpp` | handler | done | ensure/lifecycle/create/close/type-mismatch control handler는 play handler 파일로 분리했고, play/session 공통 control-ping handler는 shared handler로 분리했다. |
| `Server/Play/Handlers/PlaySessionHandlers.cs` | `Server/Play/Handlers/play_session_handlers.hpp`, `Server/Session/Handlers/session_session_handlers.hpp`, `Server/Play/Spots/play_actor_model.hpp` | handler | done | Play-local bound session push HTTP bridge는 play session handler 파일로 분리했고, stream lifecycle/auth/relay 책임은 C++ Session role handler에 대응시켰다. SM-D6 focused run으로 `/spot/push-bound-session` 경로를 검증했다. |
| `Server/Play/Handlers/PlaySpotRouteHandlers.cs` | `Server/Play/Handlers/play_spot_route_handlers.hpp` | handler | done | route client HTTP bridge handler를 목표 handler 파일로 분리했고 SM-C1/SM-C3 focused run으로 검증했다. |
| `Server/Play/Handlers/PlayStageHandlers.cs` | `Server/Play/Spots/play_actor_model.hpp`, `Server/Play/Handlers/play_spot_route_handlers.hpp` | handler | done | `.NET`의 app-level `ScenarioStage` wrapper 책임을 C++ user spot의 `StageProbeReq`/`StageTimerStartMsg` handler와 HTTP route bridge로 대응했다. |
| `Server/Play/Spots/PlayActorModel.cs` | `Server/Play/Spots/play_actor_model.hpp`, `Server/Shared/spot_actor_support.hpp` | spot | done | actor model은 role-local spot 파일로 분리했고 actor ref 변환 helper는 shared support로 분리했다. |
| `Server/Play/Spots/PlayMultiNodeScenario.cs` | `Server/MultiNode/Spots/multi_node_spots.hpp`, `Server/MultiNode/Handlers/multi_node_handlers.hpp` | spot | done | C++에서는 Play role에 섞지 않고 MultiNode role-local spot/handler로 재분류해 SM-Q9 runtime proof에서 검증한다. |
| `Server/Gateway/Program.cs` | `Server/Gateway/main.cpp` | server-role | done | gateway role 진입점 |
| `Server/Gateway/GatewayHostFactory.cs` | `Server/Gateway/gateway_host_factory.hpp` | server-role | done | gateway host factory 책임을 role-local header로 분리했다. |
| `Server/Gateway/SpotService.Gateway.csproj` | `CMakeLists.txt` | build | not-needed | C++는 상위 CMake target에 통합된다. |
| `Server/Session/Program.cs` | `Server/Session/main.cpp` | server-role | done | session role 진입점 |
| `Server/Session/SessionHostFactory.cs` | `Server/Session/session_host_factory.hpp`, `Server/Shared/Endpoints/evidence_endpoint.hpp` | server-role | done | session host factory 책임을 role-local header로 분리했고, health/evidence/evidence-wait/shutdown/crash operational endpoint는 shared endpoint handler로 연결했다. |
| `Server/Session/SessionSupport.cs` | `Server/Shared/Support/env.hpp`, `Server/Shared/Support/codecs.hpp`, `Server/Shared/Endpoints/evidence_endpoint.hpp`, `Server/Shared/scenario_state.hpp` | support | done | env, codec, evidence snapshot, scenario state 책임을 shared support/endpoint 파일로 분리했다. |
| `Server/Session/SpotService.Session.csproj` | `CMakeLists.txt` | build | not-needed | C++는 상위 CMake target에 통합된다. |
| `Server/Session/Handlers/SessionControlHandlers.cs` | `Server/Shared/Handlers/channel_control_ping_handler.hpp` | handler | done | `/channel/control-ping` route-client probe는 play/session 공통 handler로 분리했고 SM-D11 focused run으로 검증했다. |
| `Server/Session/Handlers/SessionSessionHandlers.cs` | `Server/Session/Handlers/session_session_handlers.hpp` | handler | done | session stream lifecycle, auth binding, actor relay 책임을 role-local handler header로 분리했다. |
| `Server/Session/Handlers/SessionStageHandlers.cs` | `Server/Play/Spots/play_actor_model.hpp` | handler | not-needed | SM-A5는 Play role HTTP/spot 경로로 검증한다. C++ Session role은 stream lifecycle/auth/relay 책임만 분리하고 user spot stage handler를 별도로 두지 않는다. |
| `Server/Session/Spots/SessionActorModel.cs` | `Server/Play/Spots/play_actor_model.hpp`, `Server/Session/Handlers/session_session_handlers.hpp` | spot | done | `.NET` Session role의 actor/entry/user spot model 책임은 C++에서 Play role spot model로 재분류하고, stream session bind/relay 책임은 Session handler로 분리했다. |
| `Server/Session/Spots/SessionMultiNodeScenario.cs` | `feature-map.ko.md` | spot | not-needed | `.NET` 전용 SM-Q9 관련 파일이며 공통 Config 2 완료 범위에 넣지 않는다. |
| `Server/MultiNode/Program.cs` | `Server/MultiNode/main.cpp` | server-role | done | SM-Q9 focused runner가 multi-node A/B role을 실제 process로 실행한다. |
| `Server/MultiNode/MultiNodeHostFactory.cs` | `Server/MultiNode/multi_node_host_factory.hpp` | server-role | done | public framework API로 route mesh, spot mesh, HTTP evidence endpoint를 구성하고 SM-Q9 runtime proof에서 검증한다. |
| `Server/MultiNode/MultiNodeSupport.cs` | `Server/MultiNode/multi_node_host_factory.hpp`, `Server/Shared/Support/env.hpp`, `Server/Shared/Support/codecs.hpp`, `Server/Shared/Endpoints/evidence_endpoint.hpp`, `Server/Shared/scenario_state.hpp` | support | done | MultiNode role이 shared env/codec/evidence support를 재사용하며 SM-Q9 runtime proof에서 검증한다. |
| `Server/MultiNode/SpotService.MultiNode.csproj` | `CMakeLists.txt` | build | not-needed | C++는 상위 CMake target에 통합된다. |
| `Server/MultiNode/Handlers/MultiNodeControlHandlers.cs` | `Server/MultiNode/Handlers/multi_node_handlers.hpp`, `Server/MultiNodeRequester/main.cpp` | handler | done | create-local HTTP bridge는 MultiNode role에 두고, route-to-spot state request는 C++ requester role의 server-owned public route client가 수행한다. SM-Q9 runtime proof에서 검증한다. |
| `Server/MultiNode/Handlers/MultiNodeSessionHandlers.cs` | `Server/Session/Handlers/session_session_handlers.hpp` | handler | done | multi-node stream session binding/relay 책임은 session handler 파일로 분리했다. |
| `Server/MultiNode/Handlers/MultiNodeStageHandlers.cs` | `Server/MultiNode/Spots/multi_node_spots.hpp` | handler | done | MultiNode spot의 state request handler로 대응한다. SM-A5의 stage/timer 흐름은 Play role에서 별도로 검증한다. |
| `Server/MultiNode/Spots/MultiNodeActorModel.cs` | `Server/MultiNode/Spots/multi_node_spots.hpp` | spot | done | SM-Q9에 필요한 MultiNode spot state model을 role-local spot 파일로 구현한다. |
| `Server/MultiNode/Spots/MultiNodeMultiNodeScenario.cs` | `Server/MultiNode/Spots/multi_node_spots.hpp`, `Server/MultiNode/Handlers/multi_node_handlers.hpp` | spot | done | MultiNode spot scaffold를 runtime scenario로 승격하고 SM-Q9 focused runner에서 검증한다. |

## 현재 검증

- 2026-07-08 현재 worktree 재검증:
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_session zlink_cpp_e2e_spot_service_gateway zlink_cpp_e2e_spot_service_multinode zlink_cpp_e2e_spot_service_multinode_requester zlink_cpp_e2e_spot_service_client -j2`
    - 결과: passed
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 300s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-092246-796157`
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 E2E_START_ORDER=reverse timeout 300s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-092304-797032`
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 E2E_START_ORDER=shuffle:20260708 timeout 300s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-092327-797879`
  - 비고: `SM-B9`는 framework-level reconnect loop나 control-ping timeout 확장 없이 기본
    설정으로 통과했다. 서버 간 시작 순서가 forward, reverse, shuffle 이어도 같은 public
    readiness probe와 scenario evidence가 통과했다. framework 공통 spec에는 transport
    재접속을 framework 기능으로 다시 구현하지 않고 하부 zlink socket 책임으로 둔다는 정책을
    `framework/doc/framework/common/spec/framework-api.ko.md`에 추가했다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 2400s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: failed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-092418-799125`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`
    - 실패 child: `SM-D9`
    - 비고: `play-b`가 health 응답 전 startup 중 segmentation fault로 종료되어 full sweep이
      실패했다. 같은 시각 외부 작업이 core runtime build/link와 core ctest를 수행하고 있어
      이 full run은 완료 proof로 쓰지 않는다. 이 실패는 framework-level retry나 reconnect로
      가릴 대상이 아니며, 재현되면 crash backtrace를 확보해야 한다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 ZLINK_CPP_E2E_GDB_ROLES=play-b timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-092753-809896`
    - 비고: gdb wrapper를 켠 focused run에서는 startup crash가 재현되지 않았다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 300s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D9`
    - 결과: passed, 3회 반복
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-092809-810769`,
      `framework/languages/cpp/e2e/SpotService/logs/20260708-092831-812430`,
      `framework/languages/cpp/e2e/SpotService/logs/20260708-092903-814770`
    - 비고: focused 기본 run에서도 startup crash는 재현되지 않았다.
  - 과거 실패 기록. 이 실행은 현재 runner 정책에서 허용하지 않는 외부 Redis endpoint 재사용 명령으로
    수행했으므로, 재실행용 명령으로 싣지 않는다.
    - 결과: failed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-093553-835429`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`, `SM-D12`, `SM-D14`
    - 실패 child: `SM-D15`
    - 비고: 당시에는 runner 내부 Docker 지연을 피하려고 별도 Redis 컨테이너를 먼저 띄운 뒤 external
      Redis endpoint로 full sweep을 실행했다. 현재 C++ runner 정책은 사용자 환경에서 넘긴 외부
      Redis endpoint 재사용을 허용하지 않는다. `SM-D15`에서 `play-b`가 health 응답 전 startup 중
      segmentation fault로 종료되어 실패했다. 이는 framework-level retry/reconnect로 가릴 대상이
      아니며, startup crash backtrace 확보가 필요하다.
  - 과거 진단 기록. 이 실행은 현재 runner 정책에서 허용하지 않는 외부 Redis endpoint 재사용 명령으로
    수행했으므로, 재실행용 명령으로 싣지 않는다.
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-094407-852559`
    - 비고: gdb wrapper를 켠 focused run에서는 startup crash가 재현되지 않아 backtrace를 얻지
      못했다. 이 run은 이미 떠 있던 Redis container를 고유 key prefix로 사용한 진단용 실행이므로,
      full 완료 proof로 쓰지 않는다.
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_framework zlink_cpp_e2e_spot_service_gateway zlink_cpp_e2e_spot_service_client -j1`
    - 결과: passed
    - 비고: `actor_directory_t` 기본 DI 구현 등록과 SM-D15 client error detail 보강 후 재빌드했다.
  - `timeout 120s nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D15`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-122943-1258183`
    - 비고: `SM-D15` gateway actor push 실패 원인은 gateway handler가 요구하는 public
      `actor_directory_t` service가 framework 기본 DI에 등록되지 않은 것이었다. 기본 구현은 location
      store의 actor row에서 `actor_ref_t`를 반환하며, gateway는 이 ref로
      `request_to_actor(actor_ref_t, ...)`를 호출한다.
  - `timeout 420s nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-123007-1259169`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`, `SM-D12`, `SM-D14`, `SM-D15`, `SM-C1`,
      `SM-C2`, `SM-C3`, `SM-C5`, `SM-E4`, `SM-E1`, `SM-E2`, `SM-E3`, `SM-A7`,
      `SM-A8`, `SM-C4`, `SM-A3`, `SM-A6`, `SM-B4`, `SM-B7`, `SM-A5`, `SM-A1`,
      `SM-A2`, `SM-A4`, `SM-F1`, `SM-F2`, `SM-F6`, `SM-G2`, `SM-G3`, `SM-G4`,
      `SM-G1`, `SM-Q9`
    - 비고: 중간 `Killed` 출력은 scenario 종료 시 runner cleanup이 자신이 시작한 role process를
      종료하면서 나온 메시지이며, 전체 runner exit code는 0이다. 이전 full sweep 실패 구간인
      `SM-A5`, `SM-B5`, `SM-D15`, `SM-Q9`가 모두 통과했다.
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_session zlink_cpp_e2e_spot_service_multinode zlink_cpp_e2e_spot_service_multinode_requester zlink_cpp_e2e_spot_service_client -j2`
    - 결과: passed
    - 비고: `SM-Q9` route readiness 수정과 `spot_runtime` actor request dispatch의 detached
      thread 제거 후 재빌드했다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D10`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-100606-914870`
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-100613-915436`
    - 비고: `SM-Q9`는 고정 `sleep`이나 framework-level retry 없이 requester HTTP
      `/route/control-ping`이 실제 multi-node route handler까지 왕복한 뒤 client scenario를
      시작하도록 바꿨다. `control-ping multi-a passed`, `control-ping multi-b passed`,
      `scenario SM-Q9 evidence passed`를 확인했다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 900s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: failed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-100122-908007`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`
    - 실패 child: `SM-D10`
    - 비고: `session-a`가 health 응답 전 startup 중 segmentation fault로 종료했다. 같은
      scenario를 gdb wrapper와 focused 기본 run 3회로 다시 실행했을 때는 재현되지 않았다.
      이 실패는 retry/reconnect로 가릴 대상이 아니며, startup crash backtrace 확보가 필요하다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 900s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: incomplete
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-100641-916442`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`
    - 마지막 child: `SM-D12`
    - 비고: parent 출력이 `SM-D12` child 시작 뒤 끊겼고 completion marker가 없으므로 full
      완료 proof로 쓰지 않는다. 별도 `ZLINK_CPP_E2E_GDB_ROLES=play-a` focused `SM-D12`
      run은 `framework/languages/cpp/e2e/SpotService/logs/20260708-101036-925791`에서
      통과했지만 startup crash backtrace는 얻지 못했다.
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_session zlink_cpp_e2e_spot_service_gateway zlink_cpp_e2e_spot_service_multinode zlink_cpp_e2e_spot_service_multinode_requester zlink_cpp_e2e_spot_service_client -j2`
    - 결과: passed
    - 비고: `spot_runtime`의 actor route request dispatch와 내부 spot actor route request dispatch가
      handler coroutine executor를 사용하도록 바꾼 뒤 재빌드했다. detached thread가 framework
      lifecycle 밖에서 request body와 service reference를 잡고 실행하지 않도록 정리한 변경이다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D4`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-101904-941761`
    - 비고: 이전 gdb full sweep에서 `SM-D4 second actor push failed`와 `spot_route`
      `invalid_frame`이 관측된 경로를 focused run으로 재검증했다. `scenario SM-D4 evidence
      passed`를 확인했고 server stderr는 비어 있었다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D10`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-102004-943409`
    - 비고: 이전 full sweep의 `session-a` startup segmentation fault는 focused run에서 재현되지
      않았다. `scenario SM-D10 evidence passed`를 확인했고 server stderr는 비어 있었다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-102305-948394`
    - 비고: requester HTTP `/route/control-ping`이 multi-node route handler까지 왕복한 뒤
      scenario가 시작되었다. `control-ping multi-a passed`, `control-ping multi-b passed`,
      `scenario SM-Q9 evidence passed`를 확인했다. cleanup 단계에서 role process 종료 로그가
      출력되었지만 pass marker와 evidence 검증 뒤의 정리 동작이다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 2400s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: failed
    - 로그: parent `framework/languages/cpp/e2e/SpotService/logs/20260708-102931-955579`,
      실패 child `framework/languages/cpp/e2e/SpotService/logs/20260708-103715-978103`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`, `SM-D12`, `SM-D14`, `SM-D15`, `SM-C1`, `SM-C2`,
      `SM-C3`, `SM-C5`, `SM-E4`, `SM-E1`, `SM-E2`, `SM-E3`, `SM-A7`, `SM-A8`,
      `SM-C4`, `SM-A3`, `SM-A6`, `SM-B4`, `SM-B7`, `SM-A5`, `SM-A1-A2-A4-F1-F2`,
      `SM-F6`, `SM-G2`, `SM-G3`, `SM-G4`, `SM-G1`
    - 실패 child: `SM-Q9`
    - 비고: `multi-a` route readiness는 통과했지만 `multi-b` requester가
      `MultiNodeRoutePing`을 계속 전송하고 multi-b server가 수신하지 못해 `errno=113`으로
      실패했다. 이는 완료 proof가 아니며, retry나 timeout 확장으로 가릴 대상이 아니다.
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_multinode zlink_cpp_e2e_spot_service_multinode_requester -j2`
    - 결과: passed
    - 비고: MultiNode server의 route mesh self/peer manual connect를 제거하고, 서버 간 route
      endpoint 수렴은 Redis location runtime의 desired set이 맡도록 바꿨다. requester는 검증용
      client bridge이므로 target server가 준비된 뒤 시작하도록 SM-Q9 runner를 조정했다. framework
      reconnect loop나 retry는 추가하지 않았다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-110655-1032448`
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 E2E_START_ORDER=reverse timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-110722-1034483`
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 E2E_START_ORDER=shuffle:20260708 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-110936-1042463`
    - 비고: SM-Q9는 실제 서버 역할인 `multi-a`/`multi-b`에만 start-order 변형을 적용하고,
      requester bridge는 client/runner 대기 조건으로 서버 readiness 뒤에 시작한다. forward,
      reverse, shuffle 모두 `control-ping multi-a passed`, `control-ping multi-b passed`,
      `scenario SM-Q9 evidence passed`를 확인했다. full child sweep은 이 수정 뒤 아직 다시
      실행해야 한다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 2400s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: failed
    - 로그: parent `framework/languages/cpp/e2e/SpotService/logs/20260708-111106-1045282`,
      실패 child `framework/languages/cpp/e2e/SpotService/logs/20260708-111123-1046154`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`
    - 실패 child: `SM-B5`
    - 비고: `play-b`가 HTTP health를 열기 전에 segmentation fault로 종료되어 `play-b`
      stdout/stderr가 비어 있었다. 이는 완료 proof가 아니며 retry, sleep, timeout 확장으로
      가릴 대상이 아니다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 ZLINK_CPP_E2E_GDB_ROLES=play-b timeout 240s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B5`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-111419-1050493`
    - 비고: 일반 full sweep에서 보인 `play-b` startup segmentation fault는 `play-b` gdb
      focused run에서 재현되지 않았다. focused pass는 crash 원인 해소 proof가 아니므로,
      non-gdb full sweep 재검증이 남아 있다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 ZLINK_CPP_E2E_GDB_ROLES=play-b timeout 2400s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: failed
    - 로그: parent `framework/languages/cpp/e2e/SpotService/logs/20260708-111433-1051176`,
      실패 child `framework/languages/cpp/e2e/SpotService/logs/20260708-111902-1072043`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`, `SM-D12`, `SM-D14`, `SM-D15`, `SM-C1`, `SM-C2`,
      `SM-C3`, `SM-C5`, `SM-E4`, `SM-E1`, `SM-E2`, `SM-E3`, `SM-A7`, `SM-A8`,
      `SM-C4`, `SM-A3`, `SM-A6`, `SM-B4`, `SM-B7`
    - 실패 child: `SM-A5`
    - 비고: gdb wrapper가 붙은 `play-b` 환경에서 `SM-A5` client가 `StageProbeReq` 응답을
      기다리다 HTTP timeout으로 실패했다. `play-b.stdout.log`에는 `SIGSEGV`나 backtrace가
      남지 않았다. 이 실행은 gdb timing 영향을 받으므로 완료 proof로 쓰지 않는다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 240s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A5`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-111938-1073633`
    - 비고: gdb full sweep에서 보인 `SM-A5` HTTP timeout은 일반 focused run에서 재현되지
      않았다. 그래도 전체 완료 proof는 아니므로, CPU 부하가 낮은 시점에 non-gdb full sweep을
      다시 실행해야 한다.
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_framework zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_multinode zlink_cpp_e2e_spot_service_multinode_requester -j2`
    - 결과: passed
    - 비고: `native_route_backend_t`에 남아 있던 unused `reconnect_endpoints` 생성자/멤버를
      제거했다. framework-level reconnect 기능을 추가한 것이 아니라, transport 재접속 책임이
      socket에 있다는 정책과 충돌하는 내부 이름을 제거한 것이다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 E2E_START_ORDER=shuffle:20260708 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: failed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-112213-1078492`
    - 실패 위치: `sm-q9-requester-a-route-ready`
    - 비고: `multi-a-requester-flow.log`에는 `MultiNodeRoutePing` request가 반복 전송된 기록만
      있고 reply가 없다. stderr는 `request_failed`, `errno=113`을 반환했다. 이는 route
      desired set 또는 auto-connect 수렴 문제로 남기며, retry/sleep/timeout 확장으로 완료 처리하지
      않는다. 이 실행 시점의 시스템 load가 20코어를 넘어 추가 재실행은 보류했다.
  - `nice -n 10 cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_multinode_requester zlink_cpp_e2e_spot_service_multinode -j2`
    - 결과: passed
    - 비고: MultiNode requester route mesh client를 manual endpoint connect가 아니라 discovery
      기반 `enable_client()`로 바꾼 뒤 관련 target을 다시 빌드했다. requester는 서버 readiness 뒤에
      시작하지만 route connection 자체는 location auto-connect desired set이 peer routing id와 함께
      관리해야 한다.
  - `nice -n 10 env ZLINK_CPP_E2E_SKIP_BUILD=1 E2E_START_ORDER=shuffle:20260708 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-112628-1090689`
    - 비고: `control-ping multi-a passed`, `control-ping multi-b passed`,
      `operation SpotService.sm-q9 passed`, `scenario SM-Q9 evidence passed`를 확인했다.
      pass marker 이후 cleanup 단계에서 role process `Killed` 출력이 있었지만, scenario/evidence
      검증 뒤의 정리 출력이다. 시스템 load가 20코어를 넘어 full child sweep은 이 시점에 실행하지
      않았다.
- 2026-07-08 startup crash 진단 sweep:
  - `ZLINK_CPP_E2E_SKIP_BUILD=1 ZLINK_CPP_E2E_GDB_ROLES=all ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: interrupted
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-071018-484619`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`, `SM-D12`, `SM-D14`, `SM-D15`,
      `SM-C1`, `SM-C2`, `SM-C3`, `SM-C5`, `SM-E4`, `SM-E1`
    - 비고: 모든 server role을 gdb batch wrapper로 실행했으며, 이전 full sweep에서 보였던
      `SM-D13`/`SM-D10`/`SM-D6` 주변 startup segfault는 재현되지 않았다. `SM-C5`는
      full child sweep 안에서도 target spot subscriber evidence로 통과했다. 다만 `SM-E2`
      child 시작 단계에서 Redis `docker run -d` CLI가 container 생성 후에도 반환하지 않아,
      CPU 부하를 고려해 내가 시작한 sweep만 중단했다. 이 기록은 전체 완료 proof가 아니며,
      Docker CLI 지연이 없는 상태에서 non-gdb full sweep 재검증이 필요하다.
- 2026-07-08 non-gdb focused child sweep 재검증:
  - `ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 1800s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh`
    - 결과: timed out
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-073201-543296`
    - 통과 child: `SM-B1`, `SM-B2`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B8`, `SM-B9`,
      `SM-D1`, `SM-D6`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D7`, `SM-D8`, `SM-D9`,
      `SM-D11`, `SM-D13`, `SM-D10`, `SM-D12`, `SM-D14`, `SM-D15`,
      `SM-C1`, `SM-C2`, `SM-C3`, `SM-C5`, `SM-E4`, `SM-E1`, `SM-E2`, `SM-E3`,
      `SM-A7`, `SM-A8`, `SM-C4`, `SM-A3`, `SM-A6`, `SM-B4`, `SM-B7`, `SM-A5`,
      `SM-A1-A2-A4-F1-F2`, `SM-F6`, `SM-G2`, `SM-G3`, `SM-G4`
    - 비고: 이전에 산발적으로 보였던 startup segfault는 재현되지 않았다. `SM-G1`과
      `SM-Q9`에 도달하기 전에 1800초 timeout이 만료되어 전체 완료 proof로 쓰지 않는다.
      실행 중 여러 child에서 Redis `docker run -d` CLI가 container 생성 후 늦게 반환하는
      환경 지연이 있었다.
  - `ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G1`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-080544-622418`
    - 비고: `.NET` `SmG1Scenario`와 같이 crash된 owner lease 만료 전까지 recovered auth를
      lease window 안에서 public stream request로 재시도하도록 맞춘 뒤 통과했다.
  - `ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 420s ./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-080610-623696`
    - 비고: multi-node route-to-spot evidence가 통과했다. cleanup 단계의 shell `Killed`
      출력은 child pass marker 이후 role process 종료에서 나온 출력이다.
- 2026-07-03 location store 전환 proof:
  - `cmake --build framework/languages/cpp/build-redis-vcpkg --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_session zlink_cpp_e2e_spot_service_gateway zlink_cpp_e2e_spot_service_multinode zlink_cpp_e2e_spot_service_multinode_requester zlink_cpp_e2e_spot_service_client -j2`
    - 결과: passed
  - `ZLINK_CPP_E2E_BUILD_DIR=/home/hep7/project/kairos/zlink/framework/languages/cpp/build-redis-vcpkg timeout 260s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-C1`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260703-194053-28429`
    - 비고: native route backend가 request timeout을 transport disconnect로 오인해 peer를 끊던 문제를 수정한 뒤, timeout 이후 정상 spot route request가 유지되는지 확인했다.
  - `ZLINK_CPP_E2E_BUILD_DIR=/home/hep7/project/kairos/zlink/framework/languages/cpp/build-redis-vcpkg timeout 900s framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260703-194121-30065`
    - 비고: child sweep 전체가 Redis `redis_location_store_t`와 key prefix 격리 상태에서 통과했다. parent와 각 child 출력은 `spot-service e2e result=passed`로 끝났다.
- 2026-07-02 최신 focused proof:
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F1`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-074110-72004`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F2`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-074115-72505`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F3`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-074121-73025`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F4`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-074127-73547`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F5`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-074203-74354`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D13`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-080928-26665`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-C3`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-081439-53179`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A5`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-081827-75327`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A1-A2-A4-F1-F2`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-082321-99573`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B6`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-082640-12176`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B7`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-083633-46949`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G3`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-084820-77205`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G4`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-084855-81660`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G1`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-084837-78951`
  - `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
    - 결과: passed
    - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-084837-78969`
- 2026-07-02 parent `all` runner:
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260702-092843-42150`
  - 비고: child sweep이 SM-B1~SM-Q9를 순서대로 실행했고, SM-G3 포함 모든 child가 passed marker를
    남긴 뒤 parent도 `spot-service e2e result=passed`로 끝났다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_client`
  - 결과: passed
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_session zlink_cpp_e2e_spot_service_client`
  - 결과: passed
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D6`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-045614-2707220`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D6`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-054426-2816196`
  - 비고: `Server/Play/Handlers/play_session_handlers.hpp` 분리 후 `/spot/push-bound-session` 경로를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-C1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-045614-2707204`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-C3`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-043829-2669425`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A6`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-050307-2724272`
  - 비고: `/evidence/wait` POST endpoint를 직접 호출해 lifecycle evidence wait 응답을 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A7`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-044250-2678660`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-C4`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-051426-2749225`
  - 비고: `/spot/publish/wait` POST endpoint를 직접 호출해 publish evidence wait 응답을 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D11`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-050158-2721357`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-E1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-050848-2738250`
  - 비고: `/spot/missing-handler/request`, `/spot/missing-handler/command`, `/spot/missing-target/request` endpoint를 직접 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-052350-2774338`
  - 비고: `Client/Scenarios/sm_f1_scenario.hpp` 분리 후 direct spot request/send evidence를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-052350-2774348`
  - 비고: `Client/Scenarios/sm_f2_scenario.hpp` 분리 후 direct spot request/send evidence를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F4`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-052322-2773321`
  - 비고: `Client/Scenarios/sm_f4_scenario.hpp` 분리 후 missing target negative와 recovery evidence를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-053352-2791240`
  - 비고: `Client/Scenarios/sm_g1_scenario.hpp` 분리 후 crash observation/recovery evidence를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-055119-2834665`
  - 비고: crash recovery client가 죽은 play-a route endpoint와 readiness probe endpoint를 재사용하지 않도록 runner를 조정한 뒤 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D10`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260707-232826-3943806`
  - 비고: `max_received_messages` bounded queue와 느린 push callback 조건에서 session 유지 및 다른 session push를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D12`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-000758-4138303`
  - 비고: session-a에서 session-b로 actor session transfer를 검증했다. actor route sink는 transfer bind에서 교체하고, 일반 actor-client 요청은 기존 bound-session route를 빼앗지 않도록 분리했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D15`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260708-000805-4139060`
  - 비고: gateway HTTP actor request가 기존 stream session route를 덮어쓰지 않고, actor handler의 bound-session push가 client stream에 도착하는지 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-044905-2692120`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B3`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-044932-2693624`
  - 비고: runner build 단계에서 짧은 clock skew 경고가 출력됐지만 scenario와 evidence marker는 통과했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B5`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-044958-2694818`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-045023-2695549`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-B4`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-045049-2696262`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A8`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-045117-2697176`
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-055133-2835811`
  - 비고: base suite, stream suite, SM-G1 crash/recovery evidence를 통합 runner에서 검증했다.
- `bash -n framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_multinode`
  - 결과: passed
  - 비고: MultiNode scaffold build만 확인했다. SM-Q9 runtime proof는 없으므로 완료 판정에 포함하지 않는다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A4`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-061609-2901434`
  - 비고: 실패한 SM-Q9 runner branch 제거 후 기존 common route-to-spot proof가 유지되는지 확인했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_client`
  - 결과: passed
  - 비고: SM-E2 timer evidence 추가 후 play/client target build를 확인했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-E2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-062148-2925039`
  - 비고: `user_spot_t`가 public `spot_context_t::add_timer<THandler>`로 등록한 timer tick을 `/evidence/wait`와 runner evidence assertion으로 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-E3`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-063649-2977469`
  - 비고: public spot create lifecycle에서 등록한 idle timer가 `spot_context_t::close()`로 spot을 닫고, 닫힌 spot request가 실패하는지 검증했다.
- `bash -n framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_client`
  - 결과: passed
  - 비고: SM-E4 overrun timer evidence 추가 후 play/client target build를 확인했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-E4`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-064906-3006249`
  - 비고: public `timer_options_t` overrun policy와 `timer_tick_t` delivery/scheduled/skipped evidence를 runner assertion으로 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-E2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-065027-3010019`
  - 비고: SM-E4 payload decode 순서 변경 뒤 기존 spot timer tick 경로가 유지되는지 확인했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-E3`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-065054-3010912`
  - 비고: SM-E4 payload decode 순서 변경 뒤 idle-close timer create payload가 유지되는지 확인했다.
- `bash -n framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
- `cmake --build framework/languages/cpp/build --target zlink_framework test_cpp_framework_contract_headers test_cpp_framework_module_hosted zlink_cpp_e2e_spot_service_session zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: build tree timestamp clock skew 경고가 한 번 출력됐지만 target build는 완료됐다.
- `./framework/languages/cpp/build/test_cpp_framework_contract_headers`
  - 결과: passed
- `./framework/languages/cpp/build/test_cpp_framework_module_hosted`
  - 결과: passed
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D14`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-071842-3086927`
  - 비고: TLS strict certificate rejection, skip-validation connect, stream bind, relay, push evidence를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-071910-3088627`
  - 비고: stream host 변경 뒤 기존 TCP stream bind/push 경로가 유지되는지 확인했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: SM-A8 spot-level worker endpoint 추가 뒤 play/client target build를 확인했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A8`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-073736-3137405`
  - 비고: `/spot/worker/start`와 `/spot/worker/complete` endpoint, worker 중 state request interleave, owner evidence order를 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A4`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-073801-3138786`
  - 비고: `spot_state_route_req_t` 확장 뒤 기존 key-based owner route가 유지되는지 확인했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: integrated base remote actor join 경로와 SM-C3 retry parity 조정 뒤 play/client target build를 확인했다.
- `bash -n framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: `Client/Support/client_support.hpp` 분리 뒤 client target build를 확인했다.
- `timeout 180s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-174754-600016`
  - 비고: `Client/Support/client_options.hpp` 분리 뒤 entry spot join과 evidence 검증 경로가 유지되는지 확인했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_client zlink_cpp_e2e_spot_service_session -j 4`
  - 결과: passed
  - 비고: `Shared/spot_service_contracts.hpp`의 generic JSON stream payload hook과 SM-D13 retry loop 적용 뒤 client/session target build를 확인했다.
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D13`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-175548-640631`
  - 비고: `.NET`과 같은 heartbeat-enabled stream 유지 경로, 후속 `ActorPingReq`, play/session evidence를 확인했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: `Client/Support/spot_lifecycle_order_context.hpp`와 SM-A1/A2/A4/F1/F2 grouped mode 추가 뒤 client target build를 확인했다.
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A1-A2-A4-F1-F2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-180648-679962`
  - 비고: `.NET`의 `RunA1A2A4F1F2Async`처럼 같은 lifecycle context를 공유하며 SM-A1, SM-A4, SM-F1, SM-F2, SM-A2 순서와 evidence를 검증했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_play zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: SM-A5 stage DTO, spot handler, HTTP route bridge, client scenario 추가 뒤 play/client target build를 확인했다.
- `bash -n framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-A5`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-181455-697805`
  - 비고: `.NET` SM-A5처럼 spot create, state route readiness, stage request, stage timer tick, spot close evidence를 focused run으로 검증했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_client zlink_cpp_e2e_spot_service_multinode -j 4`
  - 결과: passed
  - 비고: SM-Q9 client scenario와 MultiNode role route mesh self/client 연결 변경 뒤 client/multinode target build를 확인했다.
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-Q9`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-185724-816862`
  - 비고: multi-node A/B process를 띄우고 server-owned requester role이 각 node의 target spot id로 state request를 보내 `.NET` SmQ9Scenario와 같은 state/evidence 검증을 수행했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-075600-3183386`
  - 비고: base suite, stream suite, SM-G1 crash/recovery evidence를 통합 runner에서 다시 검증했다.
- `./framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-080501-3203717`
  - 비고: `all` runner가 base/stream/crash 외에 SM-B3, SM-B4, SM-B7, SM-D3, SM-D8, SM-D10, SM-D14, SM-E2, SM-E3, SM-E4, SM-G2, SM-G3, SM-G4 focused evidence gate도 함께 실행하도록 확장한 뒤 검증했다.
- `timeout 600s framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: failed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-190027-827802`
  - 비고: SM-Q9를 `all` focused list에 추가한 뒤 재실행했지만, SM-Q9 도달 전 stream readiness 구간에서 `SM-D2 stream auth session mismatch`가 발생했다. 이후 `.NET` 기준처럼 session-a stream에서 play-b actor를 relay하도록 검증식을 수정했다.
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh stream`
  - 결과: failed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-190241-838617`
  - 비고: SM-D2 검증식 수정 뒤 stream focused를 재실행했지만, stream client 실행 전 session-a control ping이 play-a로 route되지 않아 HTTP 500으로 타임아웃됐다. SM-Q9 focused proof와 별개로 stream readiness 후속 조사가 필요하다.
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D2`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-205103-1167276`
  - 비고: `.NET` SmD2Scenario처럼 session-a에서 play-b로 control-ping readiness를 확인한 뒤 remote stream relay와 push notify를 검증했다.
- `timeout 240s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G1`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-204440-1154566`
  - 비고: crash observation과 play-b recovery를 focused runner에서 다시 검증했다.
- `timeout 900s framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-205514-1176161`
  - 비고: base, stream, crash/recovery evidence와 SM-B3, SM-B4, SM-B7, SM-D3, SM-D8, SM-D10, SM-D14, SM-E2, SM-E3, SM-E4, SM-G2, SM-G3, SM-G4, SM-Q9 focused sweep를 통과했다. 이 기록은 과거 실행 기록이며, 최신 완료 근거는 아래의 child 재실행 없는 full sweep 결과다.
- `timeout 1200s framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260630-232540-1458783`
  - 비고: `.NET` runner처럼 full `all`을 focused scenario child sweep로 실행했다. 이 기록은 일부 child의 첫 실행 실패를 포함한 과거 실행 기록이며, 최신 완료 근거는 아래의 child 재실행 없는 full sweep 결과다.
- `bash -n framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: SM-F3/SM-F5 인라인 검증을 `Client/Scenarios/sm_f3_scenario.hpp`,
    `Client/Scenarios/sm_f5_scenario.hpp`로 분리한 뒤 client target build를 확인했다.
- `timeout 300s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F3`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260701-014358-1695850`
  - 비고: 같은 route mesh channel에서 일반 route request와 target spot route request가 함께
    처리되는지 focused runner와 play-b evidence로 확인했다.
- `timeout 300s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-F5`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260701-014425-1696662`
  - 비고: spot route negative 뒤 같은 route channel의 정상 route request와 target spot route request가
    계속 성공하는지 focused runner와 play-b evidence로 확인했다.
- `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_spot_service_client -j 4`
  - 결과: passed
  - 비고: SM-G3 client flow를 `.NET`처럼 순차 auth/bind 뒤 동시 ping/leave 구조로 맞춘 뒤 client target
    build를 확인했다.
- `timeout 300s framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-G3`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260701-024519-1802586`,
    `framework/languages/cpp/e2e/SpotService/logs/20260701-024547-1803383`
  - 비고: SM-G3의 actor join/leave와 session StreamBound evidence가 수정 후 연속 focused run에서 통과했다.
- `timeout 1200s framework/languages/cpp/e2e/SpotService/run_e2e.sh`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260701-024713-1805184`
  - 비고: SM-G3 수정 뒤 `.NET`식 focused child sweep 기반 full `all` runner가 통과했다. SM-D3와
    SM-G1은 첫 시도 실패 뒤 두 번째 child 실행에서 통과했고, SM-G3는 full sweep 안에서
    `scenario SM-G3 evidence passed`를 출력했다.
- `ZLINK_CPP_E2E_BUILD_DIR=framework/languages/cpp/build ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 1200s framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260701-145532-9009`
  - 비고: child retry 없이 focused child sweep 전체가 통과했다. route readiness는 기본 3초 settle 뒤 단일 probe로 검증했고, server role은 discovery-only route mesh, e2e client route-ready는 manual endpoint 경로로 분리했다.
- `ZLINK_CPP_E2E_BUILD_DIR=framework/languages/cpp/build ZLINK_CPP_E2E_SKIP_BUILD=1 timeout 1200s framework/languages/cpp/e2e/SpotService/run_e2e.sh all`
  - 결과: passed
  - 로그: `framework/languages/cpp/e2e/SpotService/logs/20260701-183404-20982`
  - 비고: child retry 없이 focused child sweep 전체가 통과했다. Play/Session role은 runner가 넘긴 local route endpoint를 명시적으로 연결하고, route/control readiness는 기본 3초 settle 뒤 단일 3초 probe로 검증한다. client actor relay는 public `session_actor_t::relay_request(packet_name, payload)` overload를 사용하며 SpotService target의 `framework/src` 내부 include에 의존하지 않는다. route request backend는 같은 native ROUTER socket을 여러 dispatch worker가 동시에 쓰지 않도록 framework 내부에서 직렬화했다. stream host shutdown은 worker 목록을 mutex로 보호해 accept thread와 stop thread가 동시에 `_workers`를 갱신하지 않게 했다. SM-Q9 child output은 `scenario SM-Q9 passed`와 `scenario SM-Q9 evidence passed` marker를 남긴다.

## 완료 판정

- server runtime 통합 header는 제거했고, 남은 공통 support는 shared support/endpoint 파일로 분리했다.
- 최신 `.NET`식 focused child sweep 기반 full `all` runner 검증은 child retry 없이 통과했다. route/control
  readiness는 기본 3초 settle 뒤 단일 3초 probe로 검증한다.
- SpotService target은 `framework/src` 내부 include 없이 build되고, SM-Q9 child output은 scenario/evidence
  marker를 모두 남긴다.
- `feature-map.ko.md`는 public API 또는 harness gap을 별도로 기록한다.
- play, session, gateway, multi-node host factory 책임은 role-local header로 분리했고, Redis location
  store 등록 helper는 `Server/Shared/Support/location_store.hpp`에 둔다.
- play actor model 책임은 `Server/Play/Spots/play_actor_model.hpp`로 분리했다.
- play spot route handler 책임은 `Server/Play/Handlers/play_spot_route_handlers.hpp`로 분리했고 SM-C1/SM-C3
  focused runtime 검증을 통과했다.
- play/session control handler 책임은 `Server/Play/Handlers/play_control_handlers.hpp`와
  `Server/Shared/Handlers/channel_control_ping_handler.hpp`로 분리했고 SM-A6/SM-A7/SM-C4/SM-D11
  focused runtime 검증을 통과했다.
- play actor/channel handler 책임은 `Server/Play/Handlers/play_actor_handlers.hpp`로 분리했고
  SM-A2/SM-B2/SM-B3/SM-B4/SM-B5/SM-A8 focused runtime 검증을 통과했다.
- play endpoint mapping 책임은 `Server/Play/Endpoints/` 아래 파일로 분리했고 SM-A6/SM-C1/SM-D6
  focused runtime 검증을 다시 통과했다.
- play session HTTP bridge 책임은 `Server/Play/Handlers/play_session_handlers.hpp`로 분리했고,
  session lifecycle/auth/relay는 `Server/Session/Handlers/session_session_handlers.hpp` 대응으로
  정리했다.
- play/session operational endpoint의 evidence wait, shutdown, crash route를 shared endpoint handler로
  연결했고, `/evidence/wait`는 SM-A6 focused runtime 검증에서 직접 호출했다.
- play failure endpoint의 missing-handler/missing-target route를 public route client 경로로 연결했고,
  SM-E1 focused runtime 검증에서 직접 호출했다.
- play interaction endpoint의 publish-wait route를 public evidence wait 경로로 연결했고, SM-C4
  focused runtime 검증에서 직접 호출했다.
- play interaction endpoint의 worker start/complete route를 public spot handler와 evidence wait
  경로로 연결했고, SM-A8 focused runtime 검증에서 직접 호출했다.
- play interaction endpoint의 stage request/timer route를 public spot handler와 `spot_context_t::add_timer`
  경로로 연결했고, SM-A5 focused runtime 검증에서 직접 호출했다.
- SM-F1/SM-F2/SM-F4 scenario 책임은 `Client/Scenarios/sm_f*_scenario.hpp` 파일로 분리했고
  focused runtime 검증에서 route evidence를 확인했다.
- SM-F3/SM-F5 scenario 책임도 공통 E2E scenario ID에 맞춰 `Client/Scenarios/sm_f3_scenario.hpp`,
  `Client/Scenarios/sm_f5_scenario.hpp` 파일로 분리했고 focused runtime 검증에서 route evidence를
  확인했다.
- SM-B8 destroy scenario는 stream auth 뒤 public actor destroy를 호출하고, destroy evidence와
  post-destroy request failure를 확인한다.
- SM-G1 crash/recovery scenario 책임은 `Client/Scenarios/sm_g1_scenario.hpp` 파일로 분리했다.
  `.NET`처럼 `session-a`/`session-b`를 각각 `play-a`/`play-b`에 bind하고, `play-a` crash 뒤
  `play-b` survivor ping과 `play-b` recovery rebind evidence를 확인한다.
- SM-D10 stream backpressure scenario는 C++ stream connector의 public bounded receive queue 정책에
  맞춰 `Client/Scenarios/sm_d10_scenario.hpp`로 구현했고, 최신 focused runtime 검증에서
  `logs/20260707-232826-3943806`로 통과했다.
- SM-D12 stream session transfer scenario는 `Client/Scenarios/sm_d12_scenario.hpp`에서
  session-a 연결 종료 후 session-b가 같은 actor state를 이어받는 경로를 검증하며, 최신 focused
  runtime 검증에서 `logs/20260708-000758-4138303`로 통과했다.
- SM-D15 gateway actor push scenario는 `Client/Scenarios/sm_d15_scenario.hpp`에서 gateway HTTP
  actor request와 기존 stream bound-session route 보존을 함께 검증하며, 최신 focused runtime
  검증에서 `logs/20260708-000805-4139060`로 통과했다.
- SM-E2 spot timer tick scenario는 public `spot_context_t::add_timer<THandler>` 경로로
  `Client/Scenarios/sm_e2_scenario.hpp`와 focused runtime 검증을 통과했다.
- SM-E3 idle timer close scenario는 public spot create lifecycle과 `spot_context_t::close()` 경로로
  `Client/Scenarios/sm_e3_scenario.hpp`와 focused runtime 검증을 통과했다.
- SM-E4 overrun timer scenario는 public `timer_options_t`와 `timer_tick_t` evidence 경로로
  `Client/Scenarios/sm_e4_scenario.hpp`와 focused runtime 검증을 통과했다.
- SM-D14 stream TLS scenario는 public stream node TLS server 설정과 C++ stream connector TLS 옵션으로
  `Client/Scenarios/sm_d14_scenario.hpp`와 focused runtime 검증을 통과했다.
- MultiNode scaffold는 runtime proof로 승격했고, `.NET` SmQ9Scenario에 대응하는 target spot route
  request를 focused runner에서 검증했다.
- 현재 남은 `gap` 행은 없다.
