# Java RuntimeMonitoring .NET 포팅 inventory

## 10.0.0 목표 판정

Config 7은 MeshNode·peer·ChannelName readiness와 runtime health를
공개 monitoring 표면으로 검증해야 한다. 아래 파일 대응과 기존 MON marker는 현재 구현 inventory이며,
이 목표 축을 모두 충족하기 전까지 RuntimeMonitoring 포팅 상태는 `10.0.0 전환 대상`이다.


기준 문서:

- `framework/doc/framework/common/e2e/config-7-monitoring.ko.md`
- `framework/languages/dotnet/e2e/RuntimeMonitoring/feature-map.ko.md`
- `framework/languages/dotnet/e2e/RuntimeMonitoring/`

## 요약

기존 Java config 이름은 `Monitoring`이었고 단일 Gradle application에서 `ZLINK_JAVA_E2E_ROLE`로 role을
전환했다. 현재 디렉터리는 `.NET` 기준 이름인 `RuntimeMonitoring`으로 맞췄고, `Shared`, `Client`,
`Server/Service`, `Server/FilteredService`, `Server/ThrowingService`, `Server/Trigger` Gradle
subproject로 분리했다. 제거된 public registry 계약에 의존하던 registry role은 location runtime
monitoring source로 대체했다.

Java 구현은 public monitoring API와 public runtime API만 사용한다. Client는 HTTP driver이고,
framework channel traffic은 `Server/Trigger` role이 수행한다. `.NET`에 있는 별도
`FilteredService`, `ThrowingService` role은 Java Gradle subproject와 process로 존재한다.

## Inventory

| .NET 기준 파일 | Java 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | multi-project build, `.gradle`, logs 산출물 제외 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 실행별 전용 Redis location store와 role별 installDist binary를 실행한다. MON-A1/A2/A3/A4/A5/D1 marker를 확인한다. cleanup은 runner가 기록한 PID와 전용 Redis container만 정리한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | Java 완료 scenario와 runner evidence를 기록한다. |
| `Shared/RuntimeMonitoring.Shared.csproj` | `Shared/build.gradle.kts` | build | done | shared Java library project |
| `Shared/Messages.cs` | `Shared/src/main/java/systems/zlink/e2e/runtimemonitoring/shared/Contracts.java` | shared | done | request, reply, evidence record |
| `Client/RuntimeMonitoring.Client.csproj` | `Client/build.gradle.kts` | build | done | client application project |
| `Client/Program.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Program.java` | client | done | HTTP driver entrypoint. `Server/Trigger`의 `/scenario/<id>`를 호출한다. |
| `Client/Support/ClientOptions.cs` | `Shared/src/main/java/systems/zlink/e2e/runtimemonitoring/shared/Env.java` | support | done | Java runner는 환경 변수 helper로 option을 읽는다 |
| `Client/Support/ScenarioAssert.cs` | `Server/Trigger/src/main/java/systems/zlink/e2e/runtimemonitoring/trigger/Program.java` | support | done | Trigger role의 `ensure(...)` helper로 scenario assertion 수행 |
| `Client/Scenarios/MonA1SocketEventsScenario.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Scenarios/MonA1SocketEventsScenario.java` | scenario | done | Client HTTP driver scenario file. socket `CONNECTED` 또는 `CONNECTION_READY` evidence |
| `Client/Scenarios/MonA2RegistryEventsScenario.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Scenarios/MonA2LocationEventsScenario.java` | scenario | done | Client HTTP driver scenario file. location runtime status/topology/summary event evidence |
| `Client/Scenarios/MonA3SpotEventsScenario.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Scenarios/MonA3SpotEventsScenario.java` | scenario | done | Client HTTP driver scenario file. spot status/peers/subjects/timer failure evidence |
| `Client/Scenarios/MonA4AvailabilityTransitionScenario.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Scenarios/MonA4AvailabilityTransitionScenario.java` | scenario | done | Trigger가 drain/restore를 실행하고 trigger socket `PEER_ADMISSION_CHANGED`, service admin marker, service location runtime `TOPOLOGY_CHANGED`를 확인한다. |
| `Client/Scenarios/MonA5FixedKindsScenario.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Scenarios/MonA5FixedKindsScenario.java` | scenario | done | Client HTTP driver scenario file. malformed connection, status, timer-stopped evidence |
| `Client/Scenarios/MonB1RemoteBackpressureScenario.cs` | 제거 | scenario | superseded | target별 publish result·event·snapshot count를 요구하던 시나리오는 CA-D77 계약과 함께 제거했다. 새 MON-B1은 publish 전용 관측값 부재를 검증해야 한다. |
| `Client/Scenarios/MonB2LocalTargetDropScenario.cs` | 제거 | scenario | superseded | local target별 publish result·event·snapshot count를 요구하던 시나리오는 CA-D77 계약과 함께 제거했다. 새 MON-B2는 publish 전용 관측값 부재를 검증해야 한다. |
| `Client/Scenarios/MonC1DispatchFailureScenario.cs` | 제거 | scenario | pending | monitoring observer 격리 자체는 공통 계약에 남아 있으므로 publish 집계와 분리한 MON-C1 시나리오를 다시 구성해야 한다. |
| `Client/Scenarios/MonD1FailureRecoveryScenario.cs` | `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Scenarios/MonD1FailureRecoveryScenario.java` | scenario | done | `svc-b` shutdown/restart 뒤 request가 재시작 service에서 처리되고 observer location runtime topology continuity를 확인한다. |
| `Server/Registry/*` | 없음 | server-role | not-needed | Config 7 Java는 service host의 public location runtime monitoring source를 사용하므로 제거된 registry role이 필요 없다. |
| `Server/Service/RuntimeMonitoring.Service.csproj` | `Server/Service/build.gradle.kts` | build | done | service application project |
| `Server/Service/Program.cs` | `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/Program.java` | server-role | done | channel, spot, Redis location store, monitoring source 설정 |
| `Server/Service/ServiceHostFactory.cs` | `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/Program.java` | server-role | done | Spring entrypoint가 host factory 역할 |
| `Server/Service/Support/ServiceOptions.cs` | `Shared/src/main/java/systems/zlink/e2e/runtimemonitoring/shared/Env.java` | support | done | service endpoint 환경 변수 |
| `Server/Service/Support/ServiceEvidenceStore.cs` | `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/support/EvidenceState.java`, `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/support/EvidenceHttpServer.java` | support | done | service evidence store와 admin drain/restore/shutdown endpoint |
| `Server/Service/Handlers/ServiceEventRecorders.cs` | `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/handlers/MonitoringEventHandlers.java` | server-role | done | socket/spot/location runtime/failing monitoring recorder |
| `Server/Service/Handlers/ServiceHandlers.cs` | `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/handlers/WorkReqHandler.java`, `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/handlers/MonitoringSpot.java` | server-role | done | request handler와 monitoring spot |
| `Server/FilteredService/*` | `Server/FilteredService/build.gradle.kts`, `Server/FilteredService/src/main/java/systems/zlink/e2e/runtimemonitoring/filteredservice/Program.java` | server-role | done | 별도 filtered service process. shared service configuration을 socket filter role로 실행한다 |
| `Server/ThrowingService/*` | `Server/ThrowingService/build.gradle.kts`, `Server/ThrowingService/src/main/java/systems/zlink/e2e/runtimemonitoring/throwingservice/Program.java` | server-role | done | 별도 throwing service process. shared service configuration을 throwing monitor role로 실행한다 |
| `Server/Trigger/RuntimeMonitoring.Trigger.csproj` | `Server/Trigger/build.gradle.kts` | build | done | trigger/validation application project |
| `Server/Trigger/Program.cs` | `Server/Trigger/src/main/java/systems/zlink/e2e/runtimemonitoring/trigger/Program.java` | server-role | done | framework client와 HTTP scenario endpoint entrypoint |
| `Server/Trigger/Support/*` | `Server/Trigger/src/main/java/systems/zlink/e2e/runtimemonitoring/trigger/Program.java`, `Server/Trigger/src/main/java/systems/zlink/e2e/runtimemonitoring/trigger/validation/*.java`, `Client/src/main/java/systems/zlink/e2e/runtimemonitoring/client/Support/TriggerScenarioClient.java`, `run_e2e.sh` | support | done | Java Trigger는 validation config, HTTP client helper, trigger-side socket monitoring, `svc-b` restart control을 통합한다. |
| `Server/Trigger/TriggerEndpoints.cs` | `Server/Trigger/src/main/java/systems/zlink/e2e/runtimemonitoring/trigger/Program.java` | server-role | done | Java Trigger가 `/health`, `/scenario/<id>` HTTP endpoint를 제공한다 |
| `Server/Trigger/TriggerHandlers.cs` | `Server/Service/src/main/java/systems/zlink/e2e/runtimemonitoring/service/handlers/WorkReqHandler.java` | server-role | done | request trigger handler |
| `Server/Trigger/TriggerHostFactory.cs` | `Server/Trigger/src/main/java/systems/zlink/e2e/runtimemonitoring/trigger/Program.java` | server-role | done | Spring validation entrypoint |

## 남은 gap

프로젝트와 role inventory는 모두 대응하지만 scenario 계약에는 아직 `MON-A4`, `MON-B1`, `MON-B2`,
`MON-C1`, `MON-D1`의 세부 gap이 남아 있다. 정확한 범위와 최신 실행 증거는 같은 디렉터리의
`feature-map.ko.md`를 기준으로 확인한다.

## 검증

- `../../gradlew --project-cache-dir /tmp/zlink-rm-monitor-gradle-cache --no-daemon :Server:Service:compileJava :Server:FilteredService:compileJava :Server:Trigger:compileJava :Client:compileJava --console=plain`
  - 결과: `BUILD SUCCESSFUL`
- `ZLINK_JAVA_E2E_REDIS_LOCATION_ENDPOINT=127.0.0.1:57800 ZLINK_JAVA_E2E_LOCATION_KEY_PREFIX=zlink:e2e:runtime-monitoring:mon-a4-rerun4 timeout 420s ./run_e2e.sh MON-A4`
  - 결과: `scenario MON-A4 passed`, `monitoring e2e result=passed`
  - 로그: `logs/20260707-133634-1965923/`
- `ZLINK_JAVA_E2E_REDIS_LOCATION_ENDPOINT=127.0.0.1:57800 ZLINK_JAVA_E2E_LOCATION_KEY_PREFIX=zlink:e2e:runtime-monitoring:mon-d1-rerun1 timeout 420s ./run_e2e.sh MON-D1`
  - 결과: `scenario MON-D1 passed`, `monitoring e2e result=passed`
  - 로그: `logs/20260707-133713-1970260/`
- `ZLINK_JAVA_E2E_REDIS_LOCATION_ENDPOINT=127.0.0.1:57800 ZLINK_JAVA_E2E_LOCATION_KEY_PREFIX=zlink:e2e:runtime-monitoring:full-rerun1 timeout 600s ./run_e2e.sh`
  - 결과: 당시 `MON-A1`/`MON-A2`/`MON-A3`/`MON-A4`/`MON-A5`/`MON-B1`/`MON-B2`/`MON-C1`/`MON-D1` 통과,
    `monitoring e2e result=passed`
  - 로그: `logs/20260707-133745-1973862/`
- `nice -n 10 timeout 420s ./run_e2e.sh all`
  - 결과: 당시 `MON-A1`/`MON-A2`/`MON-A3`/`MON-A4`/`MON-A5`/`MON-B1`/`MON-B2`/`MON-C1`/`MON-D1` 통과,
    `monitoring e2e result=passed`
  - 로그: `logs/20260707-221130-3621759/`
- `ZLINK_LOCAL_PACKAGE_ROOT=/tmp/zlink-java-validation-1784476567 ./run_e2e.sh all`
  - 결과: 당시 `MON-A1`/`MON-A2`/`MON-A3`/`MON-A4`/`MON-A5`/`MON-B1`/`MON-B2`/`MON-C1`/`MON-D1` 통과,
    `monitoring e2e result=passed`
  - 로그: `logs/20260720-013848-1763980/`
