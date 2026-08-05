# Java RegistryMessaging .NET 포팅 inventory

기준:
- `.NET`: `framework/languages/dotnet/e2e/LocationMessaging`
- 공통 문서: `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md`

상태 의미:
- `done`: Java 파일이 실제로 존재하고 같은 책임을 수행한다.
- `partial`: 일부 scenario는 새 계약으로 검증됐지만 같은 파일 안에 남은 legacy 책임이 있다.
- `gap`: 파일 또는 책임이 아직 `.NET` 기준 의미까지 대응되지 않는다.
- `not-needed`: Java 구조에서는 별도 파일이 필요 없고, 근거를 비고에 적었다.

| .NET 기준 파일 | Java 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 로그와 build 출력 제외. |
| `README.ko.md` | `README.ko.md` | config-doc | done | Java 실행 구조와 실행 방법을 기록한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | 구현 scenario와 검증 로그를 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 실행별 전용 Redis location store와 필요한 role만 설치·실행한다. `all` runner도 registry role 없이 통과한다. readiness는 bounded health check로 확인한다. |
| `Shared/RegistryMessaging.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Java shared DTO project. |
| `Shared/Messages.cs` | `Shared/src/main/java/systems/zlink/e2e/registrymessaging/shared/Contracts.java` | shared | done | request, reply, route, evidence DTO 대응. |
| `Client/RegistryMessaging.Client.csproj` | `Client/build.gradle.kts` | build | done | Java client application. |
| `Client/Program.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Program.java` | client-entry | done | HTTP client를 만들고 scenario catalog를 실행한다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Support/ClientOptions.java` | support | done | 환경 변수 기반 실행 옵션. |
| `Client/Support/ScenarioAssert.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Support/ScenarioAssert.java` | support | done | scenario assertion helper. 양쪽 provider evidence 대기는 background HTTP wait를 남기지 않도록 polling으로 확인한다. |
| `Client/Support/DynamicClusterLauncher.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Support/DynamicClusterLauncher.java` | support | done | dynamic provider와 consumer process에 Redis location store 입력을 넘기고, public peer query로 lifecycle 전환을 기다린다. |
| 없음 | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Support/RegistryMessagingHttp.java` | support | done | 역할 server별 HTTP client를 구성한다. |
| 없음 | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Support/ScenarioCatalog.java` | support | done | scenario 이름에 따라 RM-* 실행 순서를 고른다. |
| 없음 | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Support/ScenarioWait.java` | support | done | timeout/backpressure scenario의 명시적인 대기 helper. lifecycle phase signal 책임은 제거했다. |
| `Client/Scenarios/RmA1DiscoveryRequestScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmA1DiscoveryRequestScenario.java` | scenario | done | Redis location store 자동 연결로 consumer request를 보내고, public runtime query의 peer row와 provider evidence를 검증한다. |
| `Client/Scenarios/RmA2ManualEndpointScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmA2ManualEndpointScenario.java` | scenario | done | manual endpoint request 검증. `RM-A2` runner 통과. |
| `Client/Scenarios/RmA4SameRidFailoverScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmA4SameRidFailoverScenario.java` | scenario | done | consumer 재시작 없이 같은 rid provider handover를 검증한다. `RM-A4` runner 통과. |
| `Client/Scenarios/RmA6MultipleChannelsScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmA6MultipleChannelsScenario.java` | scenario | done | discovery consumer가 API channel과 workflow channel을 각각 location store 자동 연결로 호출해 channel 분리를 검증한다. `RM-A6` runner 통과. |
| `Client/Scenarios/RmB1ScaleOutScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmB1ScaleOutScenario.java` | scenario | done | provider 추가 뒤 consumer가 location store row를 보고 새 provider를 대상에 포함하는지 검증한다. `RM-B1` runner 통과. |
| `Client/Scenarios/RmB2ScaleInScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmB2ScaleInScenario.java` | scenario | done | provider 정상 종료 뒤 row 제거와 남은 provider routing을 검증한다. `RM-B2` runner 통과. |
| `Client/Scenarios/RmC1RequestSendScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC1RequestSendScenario.java` | scenario | done | request와 send happy path 검증. `RM-C1` runner 통과. |
| `Client/Scenarios/RmC2TargetedRouteScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC2TargetedRouteScenario.java` | scenario | done | target rid route request와 missing rid 실패 검증. `RM-C2` runner 통과. |
| `Client/Scenarios/RmC3MultiProviderDistributionScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC3MultiProviderDistributionScenario.java` | scenario | done | direct consumer multi-endpoint 분산 검증. `RM-C3` runner 통과. |
| `Client/Scenarios/RmC4TimeoutIsolationScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC4TimeoutIsolationScenario.java` | scenario | done | timeout 뒤 정상 request 복구 검증. `RM-C4` runner 통과. |
| `Client/Scenarios/RmC5MissingPacketScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC5MissingPacketScenario.java` | scenario | done | missing request/send 뒤 정상 request 복구 검증. `RM-C5` runner 통과. |
| `Client/Scenarios/RmC7WeightedProviderScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC7WeightedProviderScenario.java` | scenario | done | weighted provider 분산 검증. `RM-C7` runner 통과. |
| `Client/Scenarios/RmC8PayloadRoundTripScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC8PayloadRoundTripScenario.java` | scenario | done | 1 byte, 4 KiB, 256 KiB, 1 MiB payload 왕복 검증. payload evidence는 location store 연결으로 선택될 수 있는 양쪽 provider를 합산한다. `RM-C8` runner 통과. |
| `Client/Scenarios/RmC9BackpressureScenario.cs` | `Client/src/main/java/systems/zlink/e2e/registrymessaging/client/Scenarios/RmC9BackpressureScenario.java` | scenario | done | one-way send pressure 제출과 recovery를 검증한다. public send는 bounded-failure oracle을 노출하지 않는다. recovery evidence는 양쪽 provider를 합산한다. follow-up HTTP timeout은 consumer endpoint의 framework request timeout보다 길게 잡아 client가 먼저 끊지 않게 했다. `RM-C9` runner 통과. |
| `Server/Registry/RegistryMessaging.Registry.csproj` | 없음 | build | not-needed | Config 1은 Redis location store 기반으로 실행하므로 Java registry role을 제거했다. |
| `Server/Registry/Program.cs` | 없음 | server-role | not-needed | Registry process entry는 새 location store runner에서 쓰지 않는다. |
| `Server/Registry/RegistryHostFactory.cs` | 없음 | server-role | not-needed | Registry host 구성은 제거된 public registry 계약에 속한다. |
| `Server/Registry/Configuration/ServerOptions.cs` | 없음 | configuration | not-needed | Registry endpoint 환경 변수는 새 runner에서 쓰지 않는다. |
| `Server/Registry/Endpoints/RegistryMessagingEndpoints.cs` | 없음 | endpoints | not-needed | Registry topology endpoint 대신 consumer의 public location runtime query를 사용한다. |
| `Server/Registry/Infrastructure/EvidenceStore.cs` | 없음 | infrastructure | not-needed | Registry evidence는 scenario oracle이 아니다. |
| `Server/Provider/RegistryMessaging.Provider.csproj` | `Server/Provider/build.gradle.kts` | build | done | Redis location store extension을 참조한다. |
| `Server/Provider/Program.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Program.java` | server-role | done | `useDiscovery()` 없이 Redis location store bean을 등록한다. |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Program.java` | server-role | done | Java는 host factory 책임을 Program의 bean 구성으로 둔다. |
| `Server/Provider/Configuration/ServerOptions.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Configuration/ServerOptions.java` | configuration | done | provider endpoint, rid, weight 환경 변수. |
| `Server/Provider/Endpoints/ProviderEndpoints.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Endpoints/ProviderEndpoints.java` | endpoints | done | health, evidence, request, send, route endpoint 제공. |
| `Server/Provider/Handlers/ProviderHandlers.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Handlers/ProfileReqHandler.java`, `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Handlers/ProfileMsgHandler.java`, `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Handlers/PayloadReqHandler.java`, `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Handlers/RouteReqHandler.java` | handlers | done | request, send, payload, route handler 분리. |
| `Server/Provider/Infrastructure/EvidenceStore.cs` | `Server/Provider/src/main/java/systems/zlink/e2e/registrymessaging/provider/Infrastructure/ScenarioState.java` | infrastructure | done | evidence와 provider identity를 보관한다. |
| `Server/Workflow/RegistryMessaging.Workflow.csproj` | `Server/Workflow/build.gradle.kts` | build | done | Java workflow role application. |
| `Server/Workflow/Program.cs` | `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Program.java` | server-role | done | workflow process entry와 Spring framework 설정. |
| `Server/Workflow/WorkflowHostFactory.cs` | `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Program.java` | server-role | done | Java는 host factory 책임을 Program의 bean 구성으로 둔다. |
| `Server/Workflow/Configuration/ServerOptions.cs` | `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Configuration/ServerOptions.java` | configuration | done | workflow endpoint, rid, weight 환경 변수. |
| `Server/Workflow/Endpoints/WorkflowEndpoints.cs` | `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Endpoints/WorkflowEndpoints.java` | endpoints | done | health, evidence, workflow request endpoint 제공. |
| `Server/Workflow/Handlers/WorkflowHandlers.cs` | `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Handlers/ProfileReqHandler.java`, `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Handlers/ProfileMsgHandler.java`, `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Handlers/RouteReqHandler.java`, `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Handlers/WorkflowReqHandler.java` | handlers | done | workflow role handler 분리. |
| `Server/Workflow/Infrastructure/EvidenceStore.cs` | `Server/Workflow/src/main/java/systems/zlink/e2e/registrymessaging/workflow/Infrastructure/ScenarioState.java` | infrastructure | done | evidence와 workflow identity를 보관한다. |
| `Server/Consumer/RegistryMessaging.Consumer.csproj` | `Server/Consumer/build.gradle.kts` | build | done | Redis location store extension을 참조한다. |
| `Server/Consumer/Program.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/registrymessaging/consumer/Program.java` | server-role | done | discovery mode consumer는 endpoint 없이 API/workflow client channel을 열고 Redis location store로 자동 연결한다. |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/registrymessaging/consumer/Program.java` | server-role | done | Java는 host factory 책임을 Program의 bean 구성으로 둔다. |
| `Server/Consumer/Configuration/ConsumerOptions.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/registrymessaging/consumer/Configuration/ConsumerOptions.java` | configuration | done | discovery/direct mode와 provider endpoint 환경 변수. |
| `Server/Consumer/Endpoints/ConsumerEndpoints.cs` | `Server/Consumer/src/main/java/systems/zlink/e2e/registrymessaging/consumer/Endpoints/ConsumerEndpoints.java` | endpoints | done | profile/workflow request API와 public runtime query peer row endpoint를 제공한다. |

## Location store 전환 진행

- 완료: `RM-A1`, `RM-A2`, `RM-A4`, `RM-A6`, `RM-B1`, `RM-B2`, `RM-C1`, `RM-C2`, `RM-C3`,
  `RM-C4`, `RM-C5`, `RM-C7`, `RM-C8`, `RM-C9`, `all`.
- `RM-A1`: `timeout 240s ./run_e2e.sh RM-A1` 통과
  (`logs/20260703-200744-25342/client-RM-A1.stdout.log`:
  `scenario RM-A1 passed`, `registry-messaging e2e result=passed`). 로그 파일은
  `client-RM-A1`만 생성되어 단일 scenario 입력이 유지됨을 확인했다.
- 추가 검증:
  - `RM-C4`: `logs/20260703-201126-38889/client-RM-C4.stdout.log`
  - `RM-C1`: `logs/20260703-201837-59704/client-RM-C1.stdout.log`
  - `RM-C5`: `logs/20260703-201906-63144/client-RM-C5.stdout.log`
  - `RM-A2`: `logs/20260703-201929-65452/client-RM-A2.stdout.log`
  - `RM-A4`: `logs/20260703-203441-25665/client-rm-a4.stdout.log`
  - `RM-A6`: `logs/20260703-203947-47286/client-RM-A6.stdout.log`
  - `RM-B1`: `logs/20260703-203700-34669/client-rm-b1.stdout.log`
  - `RM-B2`: `logs/20260703-203720-36761/client-rm-b2.stdout.log`
  - `RM-C3`: `logs/20260703-201954-68646/client-RM-C3.stdout.log`
  - `RM-C8`: `logs/20260703-202115-75588/client-RM-C8.stdout.log`
  - `RM-C9`: `logs/20260707-220422-3590936/client-RM-C9.stdout.log`
  - `RM-C2`: `logs/20260703-202210-80467/client-RM-C2.stdout.log`
  - `RM-C7`: `logs/20260703-202238-83024/client-RM-C7.stdout.log`
- 전체 runner: `nice -n 10 timeout 600s ./run_e2e.sh all` 통과
  (`logs/20260707-220606-3599616/`). common, weighted, scale-out, scale-in, failover 단계가 모두
  `registry-messaging e2e result=passed`를 출력했다. 이 실행은 registry fallback runner 경로와
  registry HTTP client 제거 뒤, runner가 전용 Redis location store를 준비하는 경로의 재검증이다.
