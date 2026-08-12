# Java ResilienceLifecycle .NET 포팅 inventory

기준 문서:

- `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md`
- `framework/languages/dotnet/e2e/ResilienceLifecycle/feature-map.ko.md`
- `framework/languages/dotnet/e2e/ResilienceLifecycle/`

## 요약

기존 Java 구현은 단일 Gradle application과 `ZLINK_JAVA_E2E_ROLE` 전환 구조였다. 현재 구조는
`.NET` 기준에 맞춰 `Shared`, `Client`, `Server/Consumer`, `Server/Provider` Gradle subproject로
분리했다. registry role은 location store 전환 뒤 삭제했다. Client는 HTTP driver이고, framework
channel traffic은 `Server/Consumer`가 담당한다. provider/consumer process lifecycle은
`Client/Support/ResilienceProcessManager.java`가 제어하고, runner는 build, Redis location store
설정 전달, Client suite 실행, 최종 marker 검증을 맡는다. Java 구현은 기존 public framework API
경로를 유지한다.

## Inventory

| .NET 기준 파일 | Java 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | multi-project build, `.gradle`, logs 산출물 제외 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | `installDist`, Redis location endpoint/key prefix 전달, Client suite 실행, marker 검증을 담당한다. provider/consumer lifecycle은 Client support가 수행한다. 최신 `all` runner는 `nice -n 10 timeout 900s ./run_e2e.sh all`로 `logs/20260707-221846-3647137`에서 통과했다. RL-A4 focused 검증은 `logs/20260707-134716-2010002`에서 통과했고, 이전 `all` runner는 `logs/20260707-134830-2018139`에서 통과했다. RL-B2 focused 검증은 `logs/20260707-143234-2199882`에서 통과했고, RL-C2 focused 검증은 `logs/20260707-143319-2202901`에서 통과했다. RL-C4 focused 검증은 `logs/20260707-184443-3120118`에서 통과했다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | Java scenario 완료 상태 유지 |
| `README.ko.md` | `README.ko.md` | docs | done | Java role 구조와 실행 방법 |
| `Shared/ResilienceLifecycle.Shared.csproj` | `Shared/build.gradle.kts` | build | done | shared Java library project |
| `Shared/Messages.cs` | `Shared/src/main/java/systems/zlink/e2e/resiliencelifecycle/shared/Contracts.java` | shared | done | request, command, reply, evidence record |
| `Client/ResilienceLifecycle.Client.csproj` | `Client/build.gradle.kts` | build | done | client application project |
| `Client/Program.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Program.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | client | done | HTTP driver entrypoint. suite mode에서 provider/consumer lifecycle을 Client support로 제어하고 `Server/Consumer`의 `/scenario/<mode>`를 호출한다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Support/ClientOptions.java`, `Shared/src/main/java/systems/zlink/e2e/resiliencelifecycle/shared/Env.java` | support | done | Client process manager가 사용할 Redis location endpoint/key prefix, provider, HTTP endpoint, build dir, log dir 환경 변수를 읽는다. |
| `Client/Support/LifecycleApiResult.cs` | 없음 | support | not-needed | Java는 provider evidence JSON을 Consumer role에서 `JsonNode`로 읽어 marker만 검증한다 |
| `Client/Support/ResilienceProcessManager.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Support/ResilienceProcessManager.java` | support | done | Java Client support가 provider/consumer role binary를 시작·종료하고 readiness, control signal, storm consumer wave를 제어한다. |
| `Client/Support/ScenarioAssert.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/resiliencelifecycle/consumer/scenarios/ConsumerScenario.java` | support | done | `ensure(...)` helper는 Consumer role의 scenario execution helper에 있다. |
| `Client/Support/TopologyEntryResult.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/resiliencelifecycle/consumer/scenarios/ConsumerScenario.java` | support | done | Java는 public `monitoringLocationRuntimeQuery().listPeerLocationsAsync(...)` result를 Consumer role에서 검사한다. |
| `Client/Scenarios/RlA1ProviderRestartScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlA1ProviderRestartScenario.java` | scenario | done | Client HTTP driver scenario file. Consumer `restart` mode에서 `scenario RL-A1 passed` marker를 낸다. |
| `Client/Scenarios/RlA2ProviderEndpointRemapScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlA2ProviderEndpointRemapScenario.java` | scenario | done | Client HTTP driver scenario file. Consumer `reschedule` mode에서 replacement endpoint topology를 검증한다. |
| `Client/Scenarios/RlA3ReconnectStormScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlA3ReconnectStormScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | Client HTTP driver scenario file. Client suite가 storm consumer process wave를 실행한다. |
| `Client/Scenarios/RlA4DrainAndGreenEndpointScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlA4DrainAndGreenEndpointScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | Client suite가 provider-b를 drain한 뒤 종료하고 green endpoint를 같은 routing id로 올린다. Consumer role은 public topology query와 provider evidence로 green 전환과 원래 endpoint 복구를 검증한다. |
| `Client/Scenarios/RlA5ProviderFlappingScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlA5ProviderFlappingScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | Client HTTP driver scenario file. Client suite가 provider flapping lifecycle과 control signal을 제어한다. |
| `Client/Scenarios/RlB1CancellationCleanupScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlB1CancellationCleanupScenario.java` | scenario | done | Client HTTP driver scenario file. timeout 후 follow-up request 성공. |
| `Client/Scenarios/RlB2CrashDuringInflightScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlB2CrashDuringInflightScenario.java` | scenario | done | provider-b의 slow in-flight request 중 강제 종료를 만들고, public failure, stale topology 제거, provider-a 수렴, provider-b 복구 후 traffic 회복을 검증한다. |
| `Client/Scenarios/RlB3GracefulShutdownScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlB3GracefulShutdownScenario.java` | scenario | done | Client HTTP driver scenario file. provider admin shutdown 후 topology 수렴. |
| `Client/Scenarios/RlB4RuntimeDrainScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlB4RuntimeDrainScenario.java` | scenario | done | Client HTTP driver scenario file. provider admin weight drain/restore. |
| `Client/Scenarios/RlB5DrainInflightScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlB5DrainInflightScenario.java` | scenario | done | Client HTTP driver scenario file. slow in-flight 완료 후 restore. |
| `Client/Scenarios/RlB6GrayFaultScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlB6GrayFaultScenario.java` | scenario | done | Client HTTP driver scenario file. gray failure와 healthy provider 성공 검증. |
| `Client/Scenarios/RlC1ClientHostLifecycleScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlC1ClientHostLifecycleScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | Client HTTP driver scenario file. Client suite가 cleanup consumer lifecycle을 제어한다. |
| `Client/Scenarios/RlC3NodePauseRecoveryScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlC3NodePauseRecoveryScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | Client HTTP driver scenario file. provider down/restart 복구를 RL-A1 flow에서 함께 검증. |
| `Client/Scenarios/RlC4RegistryOutageScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlC4RegistryOutageScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | runner가 실행별 Redis location store를 만들고 Client support가 store pause/unpause를 제어한다. Consumer role은 store outage 중 이미 연결된 channel request가 성공하고, store 복구 뒤 topology와 후속 request가 정상 동작하는지 검증한다. |
| `Client/Scenarios/RlD1HighFanoutScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlD1HighFanoutScenario.java`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/ResilienceLifecycleSuite.java` | scenario | done | Client HTTP driver scenario file. Client suite가 storm wave를 만든다. |
| `Client/Scenarios/RlD2ObserverFaultScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlD2ObserverFaultScenario.java` | scenario | done | provider observer fault 주입 뒤 public failure, observer fault evidence, 후속 request 정상 동작을 검증한다. |
| `Client/Scenarios/RlD3DispatchErrorEvidenceScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlD3DispatchErrorEvidenceScenario.java` | scenario | done | Client HTTP driver scenario file. dispatch error marker evidence 검증. |
| `Client/Scenarios/RlD4MissingRequestHandlerScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlD4MissingRequestHandlerScenario.java` | scenario | done | missing handler public failure, provider dispatch-error evidence, follow-up request 정상 동작을 검증한다. |
| `Client/Scenarios/RlD5MixedBurstScenario.cs` | `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Scenarios/RlD5MixedBurstScenario.java` | scenario | done | Client HTTP driver scenario file. request/send mixed workload marker. |
| `Server/Registry/ResilienceLifecycle.Registry.csproj` | 없음 | build | not-needed | location store 전환 뒤 registry role project를 삭제했다. |
| `Server/Registry/Program.cs` | 없음 | server-role | not-needed | embedded registry process를 띄우지 않는다. |
| `Server/Registry/Configuration/ServerOptions.cs` | 없음 | server-role | not-needed | registry endpoint 환경 변수는 더 이상 없다. |
| `Server/Registry/Endpoints/RegistryEndpoints.cs` | 없음 | server-role | not-needed | registry HTTP endpoint를 사용하지 않는다. |
| `Server/Registry/Endpoints/TopologyEntryResult.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/resiliencelifecycle/consumer/scenarios/ConsumerScenario.java` | support | not-needed | Consumer role이 public location peer query result를 직접 사용한다. |
| `Server/Registry/Handlers/RegistryHandlers.cs` | 없음 | server-role | not-needed | embedded registry option bean이 필요 없다. |
| `Server/Registry/Infrastructure/EvidenceStore.cs` | 없음 | server-role | not-needed | registry evidence endpoint 미사용. |
| `Server/Registry/Infrastructure/FaultState.cs` | `run_e2e.sh`, `Client/src/main/java/systems/zlink/e2e/resiliencelifecycle/client/Support/ResilienceProcessManager.java` | server-role | done | 별도 registry fault state 대신 runner가 만든 Redis container를 Client support가 pause/unpause한다. |
| `Server/Registry/RegistryHostFactory.cs` | 없음 | server-role | not-needed | registry host factory가 필요 없다. |
| `Server/Consumer/ResilienceLifecycle.Consumer.csproj` | `Server/Consumer/build.gradle.kts` | build | done | Consumer server role project |
| `Server/Consumer/Program.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/resiliencelifecycle/consumer/Program.java` | server-role | done | Spring framework participant. Redis location store와 channel client를 설정한다. |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/resiliencelifecycle/consumer/Program.java`, `Server/Consumer/src/main/java/systems/zlink/e2e/resiliencelifecycle/consumer/endpoints/ConsumerEndpoints.java` | server-role | done | `/health`, `/scenario/<mode>` HTTP endpoint를 제공한다. |
| `Server/Provider/ResilienceLifecycle.Provider.csproj` | `Server/Provider/build.gradle.kts` | build | done | provider application project |
| `Server/Provider/Program.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/Program.java` | server-role | done | provider entrypoint, Redis location store, framework 설정 |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/Program.java` | server-role | done | Spring entrypoint가 host factory 역할 |
| `Server/Provider/ProviderEndpoints.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/endpoints/EvidenceHttpServer.java` | server-role | done | health, evidence, drain, restore, fault, shutdown endpoint |
| `Server/Provider/ProviderSupport.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/infrastructure/ScenarioState.java` | server-role | done | evidence state, slow release, gray fault state |
| `Server/Provider/Handlers/ProviderHandlers.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/handlers/WorkReqHandler.java`, `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/handlers/WorkMsgHandler.java` | server-role | done | request/send handler |
| `Server/Provider/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/resiliencelifecycle/provider/Program.java` | server-role | done | message flow observer가 dispatch error marker를 evidence에 기록 |

## 남은 항목

- 없음.
