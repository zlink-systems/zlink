# Kotlin ResilienceLifecycle .NET 기준 포팅 인벤토리

기준 구현: `framework/languages/dotnet/e2e/ResilienceLifecycle`

공통 문서: `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md`

현재 Kotlin ResilienceLifecycle E2E는 `Shared`, plain HTTP `Client`, `Server/Provider`,
`Server/Consumer` Gradle project로 process 역할을 나눠 실행한다. Client는 framework
runtime에 참여하지 않고 Consumer, Provider admin/evidence endpoint, control file만 호출한다. Consumer
role이 public `ZLinkClient`와 public location runtime query를 소유한다. client scenario/support,
provider/consumer role application, provider handler, shared message type은 Kotlin code path로
옮겼다. registry role은 Redis location store 전환 뒤 제거했다.

상태 값:

- `done`: 현재 파일이 목표 위치와 의미를 만족한다.
- `pending`: 현재 구현은 있으나 `.NET` 기준 위치와 파일 책임으로 아직 재분류하지 않았다.
- `not-needed`: Kotlin 구조에서 같은 파일 단위가 필요 없으며 비고에 근거를 적었다.
- `gap`: public contract 또는 runtime/harness 지원이 없어 완료로 주장할 수 없다.

| .NET 기준 파일 | Kotlin 대응 파일 | 분류 | 상태 | 비고 |
|----------------|------------------|------|------|------|
| `.gitignore` | `.gitignore` | config-root | done | Gradle 산출물과 logs 제외는 유지한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | role별 Gradle project runner와 Kotlin scenario/support 분리 상태, 남은 public API/harness gap을 반영했다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | role별 installDist binary를 시작하고 readiness, cleanup, 실패 로그 출력을 수행한다. |
| `Shared/ResilienceLifecycle.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Shared Gradle project를 만들고 client/server role project가 의존한다. |
| `Shared/Messages.cs` | `Shared/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Contracts.kt` | shared | done | request/reply/evidence 타입을 Shared Kotlin source로 옮겼다. |
| `Client/ResilienceLifecycle.Client.csproj` | `Client/build.gradle.kts` | build | done | Client application project는 framework/Spring dependency 없이 HTTP/process-control driver로 실행된다. |
| `Client/Program.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Program.kt` | client-entry | done | Client binary entry point가 plain JVM scenario driver만 실행한다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Support/ClientOptions.kt` | support | done | client mode, consumer HTTP endpoint, provider admin endpoint, control dir, log dir 입력을 Kotlin client option object로 모았다. |
| `Client/Support/LifecycleApiResult.cs` | `Client/src/main/kotlin/.../Support/ClientScenarioContext.kt` | support | not-needed | Kotlin client는 provider admin/evidence 응답을 typed DTO로 공개하지 않고 HTTP body와 marker wait helper만 사용한다. |
| `Client/Support/ResilienceProcessManager.cs` | `run_e2e.sh` | support | not-needed | process orchestration은 role별 binary를 시작하는 shell runner가 담당한다. Kotlin client process 안에 별도 process manager를 두지 않는다. |
| `Client/Support/ScenarioAssert.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Support/ClientScenarioContext.kt` | support | done | assertion, wait, HTTP helper를 Kotlin client support context로 분리했다. |
| `Client/Support/TopologyEntryResult.cs` | `Shared/src/main/kotlin/.../Contracts.kt`, `Server/Consumer/src/main/kotlin/.../ConsumerHttpServer.kt` | support | done | topology wait DTO와 public location peer query 책임은 Consumer role endpoint로 분리했다. |
| `Client/Scenarios/RlA1ProviderRestartScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlA1ProviderRestartScenario.kt` | scenario | done | RL-A1 restart scenario를 Kotlin scenario 파일로 분리했다. 같은 restart orchestration에서 관측하는 RL-C3 marker는 기존 runner 의미를 유지한다. |
| `Client/Scenarios/RlA2ProviderEndpointRemapScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlA2ProviderEndpointRemapScenario.kt` | scenario | done | RL-A2 endpoint remap scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlA3ReconnectStormScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlA3ReconnectStormScenario.kt` | scenario | done | RL-A3 reconnect storm scenario를 Kotlin scenario 파일로 분리했다. 같은 storm workload에서 RL-D1 marker도 유지한다. |
| `Client/Scenarios/RlA4DrainAndGreenEndpointScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlA4DrainAndGreenEndpointScenario.kt` | scenario | done | 공통 문서가 허용하는 rolling 전환으로 닫았다. runner가 기존 provider-b를 drain하고 종료한 뒤 같은 routing id의 green endpoint를 시작하며, client가 green topology와 provider evidence를 확인하고 원래 endpoint 복구까지 검증한다. |
| `Client/Scenarios/RlA5ProviderFlappingScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlA5ProviderFlappingScenario.kt` | scenario | done | RL-A5 flapping scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlB1CancellationCleanupScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlB1CancellationCleanupScenario.kt` | scenario | done | RL-B1 timeout cleanup scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlB2CrashDuringInflightScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlB2CrashDuringInflightScenario.kt` | scenario | done | provider A를 drain한 뒤 provider B의 slow request 시작 evidence를 확인하고, runner가 provider B를 `SIGKILL`한다. pending request의 public failure, surviving provider A follow-up, provider B 재시작 뒤 traffic 복구를 검증한다. |
| `Client/Scenarios/RlB3GracefulShutdownScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlB3GracefulShutdownScenario.kt` | scenario | done | RL-B3 graceful shutdown scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlB4RuntimeDrainScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlB4RuntimeDrainScenario.kt` | scenario | done | RL-B4 runtime drain/restore scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlB5DrainInflightScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlB5DrainInflightScenario.kt` | scenario | done | RL-B5 drain in-flight scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlB6GrayFaultScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlB6GrayFaultScenario.kt` | scenario | done | RL-B6 gray fault scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlC1ClientHostLifecycleScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlC1ClientHostLifecycleScenario.kt` | scenario | done | RL-C1 client host lifecycle marker를 Kotlin scenario file로 분리했다. 현재 cleanup workload는 RL-D5 mixed burst body와 함께 실행된다. |
| `Client/Scenarios/RlC2TopologyRecoveryScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlC2TopologyRecoveryScenario.kt` | scenario | done | runner가 provider-b를 `SIGKILL`해 owner lease stale row를 만들고 consumer를 새 discovery host로 재시작한다. public location runtime query에서 provider-b가 live topology에서 빠지는지, request가 provider-a로만 가는지, provider-b 재시작 뒤 다시 traffic을 받는지 확인한다. |
| `Client/Scenarios/RlC3NodePauseRecoveryScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlA1ProviderRestartScenario.kt` | scenario | not-needed | Kotlin runner는 provider restart orchestration에서 RL-C3 node pause/recovery marker를 함께 관측한다. 별도 file을 두면 같은 orchestration을 중복 실행하게 되어 만들지 않는다. |
| `Client/Scenarios/RlC4RegistryOutageScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlC4RegistryOutageScenario.kt` | scenario | done | runner-owned Redis를 pause/unpause해 store outage를 만들고, 이미 연결된 channel request가 계속 성공하는지, public topology read가 outage 중 infrastructure error로 실패하는지, store 복구 뒤 topology read와 follow-up request가 정상화되는지 확인한다. |
| `Client/Scenarios/RlD1HighFanoutScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlD1HighFanoutScenario.kt` | scenario | done | RL-D1 marker emission을 Kotlin scenario 파일로 분리했고 RL-A3 storm workload가 호출한다. |
| `Client/Scenarios/RlD2ObserverFaultScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlD2ObserverFaultScenario.kt` | scenario | done | provider dispatch-error observer가 예외를 던져도 missing-handler 오류 뒤 후속 request가 정상 처리되는지 확인한다. Java/Kotlin public monitoring event handler가 받은 runtime observer failure evidence도 함께 확인한다. |
| `Client/Scenarios/RlD3DispatchErrorEvidenceScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlD3DispatchErrorEvidenceScenario.kt` | scenario | done | RL-D3 dispatch error evidence scenario를 Kotlin scenario 파일로 분리했다. |
| `Client/Scenarios/RlD4MissingRequestHandlerScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlD4MissingRequestHandlerScenario.kt` | scenario | done | 미등록 request handler가 public 실패로 노출되고 provider dispatch-error evidence에 reason/action/packetName이 남으며, 이후 request가 정상 동작하는지 확인한다. 공통 문서의 code round-trip 검증은 server-side dispatch evidence 경로로 닫는다. |
| `Client/Scenarios/RlD5MixedBurstScenario.cs` | `Client/src/main/kotlin/.../Scenarios/RlD5MixedBurstScenario.kt` | scenario | done | RL-D5 mixed request/send burst body를 Kotlin scenario 파일로 분리했고 cleanup mode에서 RL-C1 marker와 함께 실행한다. |
| `Server/Registry/ResilienceLifecycle.Registry.csproj` | 없음 | build | not-needed | Redis location store 전환 뒤 registry role project를 제거했다. |
| `Server/Registry/Program.cs` | 없음 | server-entry | not-needed | embedded registry binary를 실행하지 않는다. |
| `Server/Registry/Configuration/ServerOptions.cs` | 없음 | configuration | not-needed | registry endpoint option은 Redis location endpoint/key prefix로 대체했다. |
| `Server/Registry/Endpoints/RegistryEndpoints.cs` | `Server/Consumer/src/main/kotlin/.../ConsumerHttpServer.kt` | endpoints | merged | Consumer role endpoint가 public location runtime query로 topology wait를 수행한다. |
| `Server/Registry/Endpoints/TopologyEntryResult.cs` | `Shared/src/main/kotlin/.../Contracts.kt`, `Server/Consumer/src/main/kotlin/.../ConsumerHttpServer.kt` | endpoints | merged | topology wait request/response DTO와 location peer 필터링을 Consumer role HTTP endpoint로 옮겼다. |
| `Server/Registry/Handlers/RegistryHandlers.cs` | `Server/Provider/src/main/kotlin/.../Handlers/EvidenceDispatchErrorObserver.kt`, `Server/Provider/src/main/java/.../handlers/*` | handlers | not-needed | Kotlin runner의 channel provider 책임은 Provider role이 맡는다. Registry role에는 handler channel을 열지 않는다. |
| `Server/Registry/Infrastructure/EvidenceStore.cs` | `Server/Provider/src/main/kotlin/.../Support/ScenarioState.kt` | infrastructure | not-needed | 현재 Kotlin evidence는 Provider role state에 모인다. Registry role evidence endpoint는 쓰지 않는다. |
| `Server/Registry/Infrastructure/FaultState.cs` | `Server/Provider/src/main/kotlin/.../Support/ScenarioState.kt` | infrastructure | not-needed | 현재 fault 주입은 Provider role admin endpoint와 state가 맡는다. Registry role fault state는 쓰지 않는다. |
| `Server/Registry/RegistryHostFactory.cs` | 없음 | server-role | not-needed | embedded registry configuration은 더 이상 필요 없다. |
| `Server/Provider/ResilienceLifecycle.Provider.csproj` | `Server/Provider/build.gradle.kts` | build | done | Provider role project를 만들었다. |
| `Server/Provider/Program.cs` | `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Program.kt` | server-entry | done | Provider binary entry point가 provider application만 실행한다. |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/ProviderApplication.kt` | server-role | done | provider channel, Redis location store, flow observer 구성을 Provider role Kotlin application으로 옮겼다. |
| `Server/Provider/ProviderEndpoints.cs` | `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Endpoints/EvidenceHttpServer.kt` | endpoints | done | health/evidence/admin endpoint를 Provider role Kotlin endpoint로 옮겼다. |
| `Server/Provider/ProviderSupport.cs` | `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Support/ScenarioState.kt` | support | done | provider state, weight, gray fault, slow request latch를 Kotlin support로 옮겼다. |
| `Server/Provider/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/Handlers/EvidenceDispatchErrorObserver.kt` | handlers | done | dispatch error observer를 Provider role Kotlin handler/support class로 분리했다. |
| `Server/Provider/Handlers/ProviderHandlers.cs` | `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/handlers/WorkRequestHandler.kt`, `Server/Provider/src/main/kotlin/systems/zlink/e2e/kotlin/resiliencelifecycle/handlers/WorkCommandHandler.kt` | handlers | done | provider request/send handlers를 Provider role Kotlin handler package로 옮겼다. |
| `Server/Consumer/ResilienceLifecycle.Consumer.csproj` | `Server/Consumer/build.gradle.kts` | build | done | Consumer role Gradle project와 installDist binary를 추가했다. |
| `Server/Consumer/Program.cs` | `Server/Consumer/src/main/kotlin/.../Program.kt` | server-entry | done | Consumer role entry point가 long-running Consumer application을 실행한다. |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/src/main/kotlin/.../ConsumerApplication.kt`, `Server/Consumer/src/main/kotlin/.../ConsumerHttpServer.kt` | server-role | done | Consumer role이 public `ZLinkClient`, public location runtime query, `/profile/*`, `/topology/wait`, `/health` endpoint를 소유한다. HTTP endpoint는 long-running request가 다른 scenario request를 막지 않도록 concurrent executor를 사용한다. |

## 기존 Kotlin/Java 파일 처리

| 기존 파일 | 판단 | 목표 |
|-----------|------|------|
| `src/main/kotlin/.../Program.kt` | role env 분기 entry point는 삭제했다. | 완료했다. |
| `src/main/java/.../ClientApplication.java` | client framework 설정과 scenario 실행 bean을 담당하던 Java class다. | 삭제했다. framework 설정은 `Server/Consumer` role로 옮기고 Client는 HTTP driver로 유지한다. |
| `src/main/java/.../ClientScenario.java` | Java monolith를 삭제하고 Kotlin dispatcher, scenario ID별 file, `Client/Support/ClientScenarioContext.kt`로 나눴다. | 완료했다. |
| `src/main/java/.../Contracts.java` | shared request/reply/evidence 타입이던 Java record 모음이다. | `Shared/src/main/kotlin/.../Contracts.kt`로 옮겼다. |
| `src/main/java/.../Env.java` | 모든 role이 공유하던 환경 변수 helper다. | `Shared/src/main/kotlin/.../Env.kt`로 옮겼고 role별 CLI option parser 전환은 이어서 진행한다. |
| `src/main/java/.../EvidenceHttpServer.java` | provider health/evidence/admin endpoint를 담당하던 Java class다. | `Server/Provider/src/main/kotlin/.../Endpoints/EvidenceHttpServer.kt`로 옮겼다. |
| `src/main/java/.../ProviderApplication.java` | provider host, discovery, channel, dispatch observer, handler registration을 담당하던 Java class다. | `Server/Provider/src/main/kotlin/.../ProviderApplication.kt`로 옮겼다. |
| `src/main/java/.../RegistryApplication.java` | embedded registry endpoint 설정을 담당하던 Java class다. | Redis location store 전환 뒤 registry role source를 삭제했다. |
| `src/main/java/.../ScenarioState.java` | provider evidence, drain weight, gray fault, slow handler latch를 담당하던 Java class다. | `Server/Provider/src/main/kotlin/.../Support/ScenarioState.kt`로 옮겼다. |
| `src/main/java/.../handlers/WorkCommandHandler.java` | provider send handler이던 Java class다. | `Server/Provider/src/main/kotlin/.../handlers/WorkCommandHandler.kt`로 옮겼다. |
| `src/main/java/.../handlers/WorkRequestHandler.java` | provider request handler이던 Java class다. | `Server/Provider/src/main/kotlin/.../handlers/WorkRequestHandler.kt`로 옮겼다. |

## Scenario ID 매핑

| Scenario ID | 공통 우선순위 | .NET 기준 scenario 파일 | Kotlin 목표 파일 | 상태 |
|-------------|---------------|-------------------------|------------------|------|
| `RL-A1` | P1 | `Client/Scenarios/RlA1ProviderRestartScenario.cs` | `Client/.../Scenarios/RlA1ProviderRestartScenario.kt` | done |
| `RL-A2` | P2 | `Client/Scenarios/RlA2ProviderEndpointRemapScenario.cs` | `Client/.../Scenarios/RlA2ProviderEndpointRemapScenario.kt` | done |
| `RL-A3` | P1 | `Client/Scenarios/RlA3ReconnectStormScenario.cs` | `Client/.../Scenarios/RlA3ReconnectStormScenario.kt` | done |
| `RL-A4` | P2 | `Client/Scenarios/RlA4DrainAndGreenEndpointScenario.cs` | `Client/.../Scenarios/RlA4DrainAndGreenEndpointScenario.kt` | done |
| `RL-A5` | P2 | `Client/Scenarios/RlA5ProviderFlappingScenario.cs` | `Client/.../Scenarios/RlA5ProviderFlappingScenario.kt` | done |
| `RL-B1` | P1 | `Client/Scenarios/RlB1CancellationCleanupScenario.cs` | `Client/.../Scenarios/RlB1CancellationCleanupScenario.kt` | done |
| `RL-B2` | P1 | `Client/Scenarios/RlB2CrashDuringInflightScenario.cs` | `Client/.../Scenarios/RlB2CrashDuringInflightScenario.kt` | done |
| `RL-B3` | P1 | `Client/Scenarios/RlB3GracefulShutdownScenario.cs` | `Client/.../Scenarios/RlB3GracefulShutdownScenario.kt` | done |
| `RL-B4` | P0 | `Client/Scenarios/RlB4RuntimeDrainScenario.cs` | `Client/.../Scenarios/RlB4RuntimeDrainScenario.kt` | done |
| `RL-B5` | P0 | `Client/Scenarios/RlB5DrainInflightScenario.cs` | `Client/.../Scenarios/RlB5DrainInflightScenario.kt` | done |
| `RL-B6` | P1 | `Client/Scenarios/RlB6GrayFaultScenario.cs` | `Client/.../Scenarios/RlB6GrayFaultScenario.kt` | done |
| `RL-C1` | P1 | `Client/Scenarios/RlC1ClientHostLifecycleScenario.cs` | `Client/.../Scenarios/RlC1ClientHostLifecycleScenario.kt` | done |
| `RL-C2` | P2 | `Client/Scenarios/RlC2TopologyRecoveryScenario.cs` | `Client/.../Scenarios/RlC2TopologyRecoveryScenario.kt` | done |
| `RL-C3` | P2 | `Client/Scenarios/RlC3NodePauseRecoveryScenario.cs` | `Client/.../Scenarios/RlA1ProviderRestartScenario.kt` | not-needed |
| `RL-C4` | P1 | `Client/Scenarios/RlC4RegistryOutageScenario.cs` | `Client/.../Scenarios/RlC4RegistryOutageScenario.kt` | done |
| `RL-D1` | P2 | `Client/Scenarios/RlD1HighFanoutScenario.cs` | `Client/.../Scenarios/RlD1HighFanoutScenario.kt` | done |
| `RL-D2` | P1 | `Client/Scenarios/RlD2ObserverFaultScenario.cs` | `Client/.../Scenarios/RlD2ObserverFaultScenario.kt` | done |
| `RL-D3` | P1 | `Client/Scenarios/RlD3DispatchErrorEvidenceScenario.cs` | `Client/.../Scenarios/RlD3DispatchErrorEvidenceScenario.kt` | done |
| `RL-D4` | P2 | `Client/Scenarios/RlD4MissingRequestHandlerScenario.cs` | `Client/.../Scenarios/RlD4MissingRequestHandlerScenario.kt` | done |
| `RL-D5` | P2 | `Client/Scenarios/RlD5MixedBurstScenario.cs` | `Client/.../Scenarios/RlD5MixedBurstScenario.kt` | done |
