# Kotlin RuntimeMonitoring .NET 포팅 inventory

## 10.0.0 목표 판정

Config 7은 MeshNode·peer·ChannelName readiness, Spot Logical Multicast backpressure·drop, runtime health를
공개 monitoring 표면으로 검증해야 한다. 아래 파일 대응과 기존 MON marker는 현재 구현 inventory이며,
이 목표 축을 모두 충족하기 전까지 RuntimeMonitoring 포팅 상태는 `10.0.0 전환 대상`이다.


기준 구현은 `framework/languages/dotnet/e2e/RuntimeMonitoring`이다. 기존 Kotlin `Monitoring`
구현은 `RuntimeMonitoring`으로 이름을 맞춰 옮겼고, 현재 통과하는 monitoring scenario는 보존했다.
`MON-A1`, `MON-A2`, `MON-A3`, `MON-A4`, `MON-A5`, `MON-B1`, `MON-C1`, `MON-D1` client 흐름은
`src/main/kotlin/.../client/scenarios/` 아래의 Kotlin scenario 파일로 분리했고,
`MON-B2` validation 흐름도 client Kotlin scenario 파일로 옮겼다. message/evidence contract는 `Shared`
Gradle module로 분리했고, Client는 framework runtime에 참여하지 않는 plain JVM HTTP/evidence driver다.
framework request가 필요한 흐름은 Trigger HTTP endpoint가 public framework client 또는 transient framework
lifecycle로 수행한다. client, service, filtered service, throwing service, trigger role은 각각 `Client`,
`Server/Service`, `Server/FilteredService`, `Server/ThrowingService`, `Server/Trigger` Gradle module과 전용
binary로 분리했다. registry role은 Redis location store 전환 뒤 제거했다.
`logs/20260704-043031-38623` full runner에서 marker와 다섯 module binary 실행을 확인했다. Trigger는
`MON-A1`, `MON-A4`, `MON-B1`, `MON-B2`, `MON-C1`, `MON-D1`에서 지원하는 HTTP endpoint를 제공한다. 따라서 role/project split과 현재
지원 가능한 scenario runner는 통과 상태다. Java/Kotlin public monitoring options는 같은 source 이름을
다시 등록할 때 configuration error로 거부하도록 맞췄고, `MON-B2` duplicate-source validation endpoint도
client scenario에서 호출한다.

## Scenario 상태

| scenario | .NET 기준 파일 | Kotlin 현재 대응 | 상태 | 비고 |
|----------|----------------|------------------|------|------|
| `MON-A1` | `Client/Scenarios/MonA1SocketEventsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA1SocketEventsScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt` | done | `logs/20260704-043031-38623`에서 marker를 확인했다. Client는 Trigger disconnect request endpoint를 호출한다. |
| `MON-A2` | `Client/Scenarios/MonA2RegistryEventsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA2RegistryEventsScenario.kt` | done | `logs/20260704-043031-38623`에서 service host의 `ops-locations` source marker를 확인했다. |
| `MON-A3` | `Client/Scenarios/MonA3SpotEventsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA3SpotEventsScenario.kt`, `Server/Service/src/main/kotlin/.../service/MonitoringSpot.kt` | done | client scenario와 service spot handler를 Kotlin 파일로 분리했고 `logs/20260704-043031-38623`에서 marker를 확인했다. |
| `MON-A4` | `Client/Scenarios/MonA4AvailabilityTransitionScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA4AvailabilityTransitionScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerApplication.kt`, `Server/Service/src/main/kotlin/.../service/EvidenceHttpServer.kt` | 10.0.0 전환 대상 | 현재 marker는 weight 0/복원과 `PEER_ADMISSION_CHANGED`, location `TOPOLOGY_CHANGED`를 확인한다. 별도 observer의 replacement·`SIGKILL` failover 검증은 남아 있다. |
| `MON-A5` | `Client/Scenarios/MonA5FixedKindsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA5FixedKindsScenario.kt` | done | malformed connection과 fixed kind marker를 `logs/20260704-043031-38623`에서 확인했다. |
| `MON-B1` | `Client/Scenarios/MonB1KindFilterScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonB1KindFilterScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/FilteredService/src/main/kotlin/.../filteredservice/FilteredServiceApplication.kt` | done | client scenario가 Trigger service-b endpoint와 FilteredService evidence를 확인하고 `logs/20260704-043031-38623`에서 marker를 확인했다. |
| `MON-B2` | `Client/Scenarios/MonB2RegistrationValidationScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonB2RegistrationValidationScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/Trigger/src/main/kotlin/.../trigger/MonitoringValidationApplication.kt` | done | client scenario가 Trigger HTTP endpoint로 duplicate source, 비양수 interval, missing socket source, missing spot source 검증을 호출하고 `logs/20260704-043031-38623`에서 marker를 확인했다. |
| `MON-C1` | `Client/Scenarios/MonC1DispatchFailureScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonC1DispatchFailureScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/ThrowingService/src/main/kotlin/.../throwingservice/ThrowingServiceApplication.kt`, `Server/Service/src/main/kotlin/.../service/MonitoringEventHandlers.kt` | done | client scenario가 Trigger throwing-service endpoint와 ThrowingService evidence를 확인하고 `logs/20260704-043031-38623`에서 marker를 확인했다. |
| `MON-D1` | `Client/Scenarios/MonD1FailureRecoveryScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonD1FailureRecoveryScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/Service/src/main/kotlin/.../service/EvidenceHttpServer.kt` | done | `logs/20260704-043031-38623`에서 `scenario MON-D1 passed` marker를 확인했다. service-b 종료/재시작, Trigger transient service-b request, restarted service socket evidence, location topology evidence를 확인한다. |

## .NET 파일 대응

| .NET 기준 파일 | Kotlin 대응 파일 | 분류 | 상태 | 비고 |
|----------------|------------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | RuntimeMonitoring 경로로 이동했다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | 기존 구현/gap 분류를 RuntimeMonitoring 이름으로 갱신했다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | `Client`, `Server/Service`, `Server/FilteredService`, `Server/ThrowingService`, `Server/Trigger` binary를 함께 빌드/실행하고 filtered/throwing service와 Trigger HTTP endpoint를 client에 넘긴다. `logs/20260704-043031-38623` runner는 통과한다. |
| `Shared/RuntimeMonitoring.Shared.csproj` | `Shared/build.gradle.kts` | shared-project | done | `Shared` Gradle module로 분리했다. |
| `Shared/Messages.cs` | `Shared/src/main/kotlin/.../runtimemonitoring/Contracts.kt` | shared | done | message/evidence 타입을 `Shared` module-local Kotlin `@JvmRecord` data class로 옮겼다. |
| `Client/RuntimeMonitoring.Client.csproj` | `Client/build.gradle.kts` | client-project | done | Client 전용 Gradle module과 binary를 추가했다. |
| `Client/Program.cs` | `Client/src/main/kotlin/.../client/Program.kt` | entrypoint | done | Client 전용 program은 plain JVM driver로 scenario를 실행한다. Spring/framework host는 Client에서 제거했다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/kotlin/.../client/ClientOptions.kt`, `Shared/src/main/kotlin/.../Env.kt` | support | merged | client runner 입력은 client option class와 공용 env helper로 합쳤다. role별 server option class는 각 role application이 직접 읽는 env helper로 대체한다. |
| `Client/Support/ScenarioAssert.cs` | `src/main/kotlin/.../client/ScenarioAssert.kt` | support | done | assertion helper를 scenario monolith 밖으로 분리했다. |
| `Client/Scenarios/MonA1SocketEventsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA1SocketEventsScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt` | scenario | done | `MON-A1` marker는 `logs/20260704-043031-38623`에서 확인했다. |
| `Client/Scenarios/MonA2RegistryEventsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA2RegistryEventsScenario.kt` | scenario | done | `MON-A2` marker는 `logs/20260704-043031-38623`에서 확인했다. |
| `Client/Scenarios/MonA3SpotEventsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA3SpotEventsScenario.kt` | scenario | done | `MON-A3` marker는 `logs/20260704-043031-38623`에서 확인했다. |
| `Client/Scenarios/MonA4AvailabilityTransitionScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA4AvailabilityTransitionScenario.kt` | scenario | 10.0.0 전환 대상 | 현재 weight 변경 marker는 확인했으나 replacement·failover 전이 검증은 남아 있다. |
| `Client/Scenarios/MonA5FixedKindsScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonA5FixedKindsScenario.kt` | scenario | done | `MON-A5` marker는 `logs/20260704-043031-38623`에서 확인했다. |
| `Client/Scenarios/MonB1KindFilterScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonB1KindFilterScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/FilteredService/src/main/kotlin/.../filteredservice/FilteredServiceApplication.kt` | scenario | done | `MON-B1` marker는 `logs/20260704-043031-38623`에서 확인했다. |
| `Client/Scenarios/MonB2RegistrationValidationScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonB2RegistrationValidationScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/Trigger/src/main/kotlin/.../trigger/MonitoringValidationApplication.kt` | scenario | done | client scenario가 Trigger HTTP endpoint를 호출해 duplicate source, 비양수 interval, missing socket source, missing spot source validation을 확인한다. |
| `Client/Scenarios/MonC1DispatchFailureScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonC1DispatchFailureScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/ThrowingService/src/main/kotlin/.../throwingservice/ThrowingServiceApplication.kt` | scenario | done | `MON-C1` marker는 `logs/20260704-043031-38623`에서 확인했다. |
| `Client/Scenarios/MonD1FailureRecoveryScenario.cs` | `Client/src/main/kotlin/.../client/scenarios/MonD1FailureRecoveryScenario.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt` | scenario | done | service-b 종료/재시작과 Trigger transient service-b request를 확인하고 `logs/20260704-043031-38623`에서 marker를 확인했다. |
| `Server/Registry/RuntimeMonitoring.Registry.csproj` | 없음 | server-project | not-needed | Redis location store 전환 뒤 registry role project를 제거했다. |
| `Server/Registry/Program.cs` | 없음 | server-role | not-needed | embedded registry binary를 실행하지 않는다. |
| `Server/Registry/RegistryHostFactory.cs` | 없음 | server-role | not-needed | service host의 public location runtime monitoring source로 대체했다. |
| `Server/Registry/Handlers/RegistryEventRecorders.cs` | `Server/Service/src/main/kotlin/.../service/MonitoringEventHandlers.kt` | handler | not-needed | location runtime recorder가 service host의 `ops-locations` source를 기록한다. |
| `Server/Registry/Handlers/RegistryHandlers.cs` | 없음 | handler | not-needed | registry monitoring handler 등록은 더 이상 필요 없다. |
| `Server/Registry/Support/RegistryEvidenceStore.cs` | 없음 | support | not-needed | location evidence는 Service role evidence store에 모인다. |
| `Server/Registry/Support/RegistryOptions.cs` | 없음 | support | not-needed | Redis location endpoint/key prefix는 runner env와 공용 `Env` helper로 읽는다. |
| `Server/Service/RuntimeMonitoring.Service.csproj` | `Server/Service/build.gradle.kts` | server-project | done | Service 전용 Gradle module과 binary를 추가했다. |
| `Server/Service/Program.cs` | `Server/Service/src/main/kotlin/.../service/Program.kt` | server-role | done | Service 전용 program이 `ServiceApplication.run()`을 실행한다. |
| `Server/Service/ServiceHostFactory.cs` | `Server/Service/src/main/kotlin/.../service/ServiceApplication.kt` | server-role | done | host 설정은 Service module-local Kotlin class로 옮겼다. |
| `Server/Service/Handlers/ServiceEventRecorders.cs` | `Server/Service/src/main/kotlin/.../service/MonitoringEventHandlers.kt` | handler | done | recorder와 throwing recorder helper는 Service module-local Kotlin class로 분리했다. |
| `Server/Service/Handlers/ServiceHandlers.cs` | `Server/Service/src/main/kotlin/.../service/WorkRequestHandler.kt`, `Server/Service/src/main/kotlin/.../service/MonitoringSpot.kt` | handler | done | work handler와 spot timer는 Service module-local Kotlin class로 분리했다. |
| `Server/Service/Support/ServiceEvidenceStore.cs` | `Server/Service/src/main/kotlin/.../service/EvidenceState.kt` | support | done | evidence store를 Service module-local Kotlin class로 분리했다. |
| `Server/Service/Support/ServiceOptions.cs` | `Shared/src/main/kotlin/.../Env.kt` | support | merged | service, filtered service, throwing service endpoint와 log directory option 읽기는 runner env와 공용 `Env` helper로 합쳤다. 별도 role option class는 현재 Kotlin 구조에서 두지 않는다. |
| `Server/FilteredService/RuntimeMonitoring.FilteredService.csproj` | `Server/FilteredService/build.gradle.kts` | server-project | done | FilteredService 전용 Gradle module과 binary를 추가했다. |
| `Server/FilteredService/Program.cs` | `Server/FilteredService/src/main/kotlin/.../filteredservice/Program.kt` | server-role | done | FilteredService 전용 program이 `FilteredServiceApplication.run()`을 실행한다. |
| `Server/FilteredService/FilteredServiceHostFactory.cs` | `Server/FilteredService/src/main/kotlin/.../filteredservice/FilteredServiceApplication.kt` | server-role | done | filtered socket source 등록은 전용 role의 Kotlin host 설정으로 분리했다. Service module-local support class를 재사용한다. |
| `Server/ThrowingService/RuntimeMonitoring.ThrowingService.csproj` | `Server/ThrowingService/build.gradle.kts` | server-project | done | ThrowingService 전용 Gradle module과 binary를 추가했다. |
| `Server/ThrowingService/Program.cs` | `Server/ThrowingService/src/main/kotlin/.../throwingservice/Program.kt` | server-role | done | ThrowingService 전용 program이 `ThrowingServiceApplication.run()`을 실행한다. |
| `Server/ThrowingService/ThrowingServiceHostFactory.cs` | `Server/ThrowingService/src/main/kotlin/.../throwingservice/ThrowingServiceApplication.kt` | server-role | done | throwing socket monitor handler는 전용 role의 Kotlin host 설정으로 분리했다. Service module-local support class를 재사용한다. |
| `Server/Trigger/RuntimeMonitoring.Trigger.csproj` | `Server/Trigger/build.gradle.kts` | server-project | done | Trigger module과 binary를 추가했고 `MON-A1` disconnect request, `MON-A4` drain request, `MON-B1`/`MON-D1` service-b request, `MON-B2` validation, `MON-C1` throwing-service request HTTP endpoint를 실행한다. |
| `Server/Trigger/Program.cs` | `Server/Trigger/src/main/kotlin/.../trigger/Program.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerApplication.kt` | server-role | done | Trigger Spring host를 Kotlin class로 옮겼다. framework client와 endpoint는 현재 Kotlin scenario가 요구하는 `MON-A4` drain, `MON-B2` validation, `MON-D1` restart request를 담당한다. |
| `Server/Trigger/TriggerHostFactory.cs` | `Server/Trigger/src/main/kotlin/.../trigger/TriggerApplication.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt` | server-role | done | framework client, monitoring recorder, lightweight HTTP host 설정을 Kotlin class로 옮겼다. |
| `Server/Trigger/TriggerEndpoints.cs` | `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt` | endpoint | done | `/health`, `/evidence`, `/profile/request`, `/profile/request/disconnect`, `/profile/request/service-b`, `/profile/request/throw`, `MON-B2` validation endpoint를 제공한다. log wait는 Kotlin `MON-C1`이 ThrowingService evidence와 후속 request로 대체하고, invalid handshake 유발은 `MonA5FixedKindsScenario.kt`가 raw TCP 연결로 직접 수행한다. |
| `Server/Trigger/TriggerHandlers.cs` | `Server/Trigger/src/main/kotlin/.../trigger/TriggerApplication.kt`, `Server/Service/src/main/kotlin/.../service/MonitoringEventHandlers.kt` | handler | merged | Trigger socket recorder 책임은 Kotlin monitoring handler bean으로 옮겼다. `.NET` validation request handler는 Kotlin validation host 안에서 테스트용 channel source 검증 로직으로 합쳤다. |
| `Server/Trigger/Support/TriggerClientRequests.cs` | `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt` | support | merged | transient request helper 책임은 Trigger의 disconnect, service-b, throwing-service request endpoint로 구현했다. invalid handshake 유발은 `MonA5FixedKindsScenario.kt`가 raw TCP 연결로 직접 수행한다. |
| `Server/Trigger/Support/TriggerLogReader.cs` | `Client/src/main/kotlin/.../client/scenarios/MonC1DispatchFailureScenario.kt`, service evidence endpoint | support | merged | Kotlin `MON-C1`은 stderr 파일 polling 대신 ThrowingService evidence endpoint와 후속 channel request로 dispatch failure 격리를 확인한다. |
| `Server/Trigger/Support/TriggerSupport.cs` | `Server/Trigger/src/main/kotlin/.../trigger/TriggerApplication.kt`, `Server/Trigger/src/main/kotlin/.../trigger/TriggerHttpServer.kt`, `Server/Service/src/main/kotlin/.../service/EvidenceState.kt`, `Shared/src/main/kotlin/.../Env.kt` | support | merged | evidence store는 Service 계열 support를 재사용하고, Trigger option parsing은 runner env와 공용 env helper로 합쳤다. |
| `Server/Trigger/Support/TriggerValidation.cs` | `Server/Trigger/src/main/kotlin/.../trigger/MonitoringValidationApplication.kt` | support | done | duplicate source, 비양수 interval, missing source validation을 Trigger HTTP endpoint로 연결했다. |

## 기존 Kotlin 파일 처리

| Kotlin 파일 | 판단 | 다음 작업 |
|-------------|------|-----------|
| `src/main/kotlin/.../Program.kt` | 유지 | role별 project/program으로 나눌 때 client/server entrypoint로 분리한다. |
| Client Spring host 파일 | 삭제 | Client module에서 Spring host와 framework 설정을 제거했다. Client는 plain JVM HTTP/evidence driver다. |
| `Shared/src/main/kotlin/.../Contracts.kt` | 유지 | message/evidence contract를 Kotlin `@JvmRecord` data class로 옮겼다. |
| `Shared/src/main/kotlin/.../Env.kt` | 유지 | role별 option 값 읽기는 현재 공용 Kotlin helper로 합쳐 둔다. 별도 option class가 필요해지면 role support로 분리한다. |
| `Server/Service/src/main/kotlin/.../EvidenceHttpServer.kt` | 유지 | Service, FilteredService, ThrowingService가 공유하는 evidence/admin HTTP endpoint support를 Kotlin class로 옮겼다. |
| `Server/Service/src/main/kotlin/.../EvidenceState.kt` | 유지 | Service module-local Kotlin class로 나눴다. |
| `Server/Service/src/main/kotlin/.../MonitoringEventHandlers.kt` | 유지 | service recorder와 throwing handler helper를 Service module-local Kotlin class로 옮겼다. FilteredService와 ThrowingService role은 이 helper를 재사용한다. |
| `Server/Service/src/main/kotlin/.../MonitoringSpot.kt` | 유지 | service spot timer 책임을 Service module-local Kotlin class로 옮겼다. |
| `Server/Trigger/src/main/kotlin/.../MonitoringValidationApplication.kt` | 유지 | validation Spring failure harness를 Kotlin class로 옮겼고, client scenario가 Trigger HTTP endpoint로 duplicate source, 비양수 interval, missing source 검증을 호출한다. |
| `Server/Trigger/src/main/kotlin/.../TriggerApplication.kt` | 유지 | Trigger host 설정을 Kotlin class로 옮겼다. |
| `Server/Trigger/src/main/kotlin/.../TriggerHttpServer.kt` | 유지 | Trigger HTTP endpoint support를 Kotlin class로 옮겼다. |
| `Server/Registry/src/main/kotlin/.../RegistryApplication.kt` | 제거 | Redis location store 전환 뒤 registry role source를 삭제했다. |
| `Server/Registry/src/main/kotlin/.../EvidenceHttpServer.kt` | 제거 | location evidence는 Service role evidence endpoint에서 제공한다. |
| `Server/Service/src/main/kotlin/.../ServiceApplication.kt` | 유지 | `Server/Service` role project의 host 설정을 Kotlin class로 옮겼다. `FilteredService`와 `ThrowingService`는 별도 role로 분리했다. |
| `Server/Service/src/main/kotlin/.../WorkRequestHandler.kt` | 유지 | Service module-local Kotlin handler로 옮겼다. |
