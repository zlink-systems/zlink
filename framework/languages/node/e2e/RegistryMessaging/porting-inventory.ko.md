# Node Location Messaging E2E porting inventory

기준 구현: `framework/languages/dotnet/e2e/LocationMessaging`

공통 기준 문서: `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md`

이 문서는 `.NET` Config 1 파일과 공통 scenario ID를 Node 파일로 매핑한다. 현재 runner가 검증하는
범위는 `done`, 10.0.0 목표 계약에 맞춘 검증이 더 필요한 범위는 `10.0.0 전환 대상`으로 기록한다.

## Scenario ID 매핑

| scenario ID | .NET scenario 파일 | Node 대응 파일 | 상태 | 비고 |
|-------------|--------------------|----------------|------|------|
| `RM-A1` | `Client/Scenarios/RmA1DiscoveryRequestScenario.cs` | `Client/Scenarios/rm-a1-discovery-request-scenario.ts` | done | Redis location store 자동 연결과 topology ready provider 확인 |
| `RM-A2` | `Client/Scenarios/RmA2ManualEndpointScenario.cs` | `Client/Scenarios/rm-a2-manual-endpoint-scenario.ts` | done | 수동 endpoint request |
| `RM-A4` | `Client/Scenarios/RmA4SameRidFailoverScenario.cs` | `Client/Scenarios/rm-a4-same-rid-failover-scenario.ts` | done | dynamic provider 교체 |
| `RM-A6` | `Client/Scenarios/RmA6MultipleChannelsScenario.cs` | `Client/Scenarios/rm-a6-multiple-channels-scenario.ts` | done | profile/workflow channel 독립성 |
| `RM-B1` | `Client/Scenarios/RmB1ScaleOutScenario.cs` | `Client/Scenarios/rm-b1-scale-out-scenario.ts` | done | traffic 중 provider 추가 |
| `RM-B2` | `Client/Scenarios/RmB2ScaleInScenario.cs` | `Client/Scenarios/rm-b2-scale-in-scenario.ts` | done | terminal Drained, peer row 즉시 제거, 전환 요청 성공 |
| `RM-B3` | `Client/Scenarios/RmB3ProviderCrashFailoverScenario.cs` | `Client/Scenarios/rm-b3-provider-crash-failover-scenario.ts` | done | SIGKILL, lease 만료, 남은 provider failover, targeted 오류 종류 |
| `RM-C1` | `Client/Scenarios/RmC1RequestSendScenario.cs` | `Client/Scenarios/rm-c1-request-send-scenario.ts` | done | request/send happy path |
| `RM-C2` | `Client/Scenarios/RmC2TargetedRouteScenario.cs` | `Client/Scenarios/rm-c2-targeted-route-scenario.ts` | done | target 정확성과 `RequestTargetNotFound`; known disconnected는 RM-B3에서 `RouteNotConnected` 검증 |
| `RM-C3` | `Client/Scenarios/RmC3MultiProviderDistributionScenario.cs` | `Client/Scenarios/rm-c3-multi-provider-distribution-scenario.ts` | done | 수동 multi-endpoint 분산 |
| `RM-C4` | `Client/Scenarios/RmC4TimeoutIsolationScenario.cs` | `Client/Scenarios/rm-c4-timeout-isolation-scenario.ts` | done | timeout 뒤 late reply 비오염 |
| `RM-C5` | `Client/Scenarios/RmC5MissingPacketScenario.cs` | `Client/Scenarios/rm-c5-missing-packet-scenario.ts` | done | missing request/send dispatch error |
| `RM-C7` | `Client/Scenarios/RmC7WeightedProviderScenario.cs` | `Client/Scenarios/rm-c7-weighted-provider-scenario.ts` | done | build-time provider weight 75/25 분산 검증 |
| `RM-C8` | `Client/Scenarios/RmC8PayloadRoundTripScenario.cs` | `Client/Scenarios/rm-c8-payload-round-trip-scenario.ts` | done | RouteMesh SS payload length/hash 왕복과 이후 정상 request 회복을 검증한다. StreamNode inbound 상한은 별도 runtime·unit contract다. |
| `RM-C9` | `Client/Scenarios/RmC9BackpressureScenario.cs` | `Client/Scenarios/rm-c9-backpressure-scenario.ts` | 10.0.0 전환 대상 | 현재 one-way send pressure 제출과 recovery를 검증한다. `submit()`의 최초 시도, bounded wait와 timeout 검증은 남아 있다. |

## 파일 매핑

| .NET 기준 파일 | Node 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | Node dist, node_modules, logs 산출물 제외 |
| `README.ko.md` | `README.ko.md` | docs | done | Node 보충 설명과 gap을 과장 없이 기록 |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | 공통 scenario ID별 구현 기록 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | build, port allocation, role process, readiness, cleanup, failure log 출력 |
| `Shared/Messages.cs` | `Shared/messages.ts` | shared | done | request/reply/evidence DTO와 packet name 상수 |
| `Shared/RegistryMessaging.Shared.csproj` | `Shared/messages.ts` | project | done | Node는 별도 shared package 없이 TypeScript shared module로 대응 |
| `Client/RegistryMessaging.Client.csproj` | `Client/package.json`, `Client/tsconfig.json` | project | done | client 실행 앱 build 설정 |
| `Client/Program.cs` | `Client/main.ts` | client-entry | done | scenario 목록과 선택 실행만 담당 |
| `Client/Scenarios/RmA1DiscoveryRequestScenario.cs` | `Client/Scenarios/rm-a1-discovery-request-scenario.ts` | scenario | done | `RM-A1` |
| `Client/Scenarios/RmA2ManualEndpointScenario.cs` | `Client/Scenarios/rm-a2-manual-endpoint-scenario.ts` | scenario | done | `RM-A2` |
| `Client/Scenarios/RmA4SameRidFailoverScenario.cs` | `Client/Scenarios/rm-a4-same-rid-failover-scenario.ts` | scenario | done | `RM-A4` |
| `Client/Scenarios/RmA6MultipleChannelsScenario.cs` | `Client/Scenarios/rm-a6-multiple-channels-scenario.ts` | scenario | done | `RM-A6` |
| `Client/Scenarios/RmB1ScaleOutScenario.cs` | `Client/Scenarios/rm-b1-scale-out-scenario.ts` | scenario | done | `RM-B1` |
| `Client/Scenarios/RmB2ScaleInScenario.cs` | `Client/Scenarios/rm-b2-scale-in-scenario.ts` | scenario | done | `RM-B2` |
| `Client/Scenarios/RmC1RequestSendScenario.cs` | `Client/Scenarios/rm-c1-request-send-scenario.ts` | scenario | done | `RM-C1` |
| `Client/Scenarios/RmC2TargetedRouteScenario.cs` | `Client/Scenarios/rm-c2-targeted-route-scenario.ts` | scenario | done | `RM-C2` |
| `Client/Scenarios/RmC3MultiProviderDistributionScenario.cs` | `Client/Scenarios/rm-c3-multi-provider-distribution-scenario.ts` | scenario | done | `RM-C3` |
| `Client/Scenarios/RmC4TimeoutIsolationScenario.cs` | `Client/Scenarios/rm-c4-timeout-isolation-scenario.ts` | scenario | done | `RM-C4` |
| `Client/Scenarios/RmC5MissingPacketScenario.cs` | `Client/Scenarios/rm-c5-missing-packet-scenario.ts` | scenario | done | `RM-C5` |
| `Client/Scenarios/RmC7WeightedProviderScenario.cs` | `Client/Scenarios/rm-c7-weighted-provider-scenario.ts` | scenario | done | `RM-C7`, public `addRouteMesh(...).channel(...).server().setWeight(...)` 사용 |
| `Client/Scenarios/RmC8PayloadRoundTripScenario.cs` | `Client/Scenarios/rm-c8-payload-round-trip-scenario.ts` | scenario | done | `RM-C8` |
| `Client/Scenarios/RmC9BackpressureScenario.cs` | `Client/Scenarios/rm-c9-backpressure-scenario.ts` | scenario | 10.0.0 전환 대상 | 현재 pressure/recovery 경로는 구현되어 있으나 10.0.0 admission 결과 검증은 남아 있다. |
| `Client/Support/ClientOptions.cs` | `Client/Support/client-options.ts` | support | done | CLI option parsing |
| `Client/Support/DynamicClusterLauncher.cs` | `Client/Support/dynamic-cluster-launcher.ts` | support | done | dynamic consumer/provider process lifecycle |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/scenario-assert.ts` | support | done | assertion and evidence count helper |
| `Server/LocationProbe/*` | 없음 | server-role | done | application 역할이 없는 probe를 제거했다. peer row는 Consumer의 public location runtime query로 읽고 연결은 provider 처리 evidence로 확인한다. |
| `Server/Provider/RegistryMessaging.Provider.csproj` | `Server/Provider/package.json`, `Server/Provider/tsconfig.json` | project | done | provider role build 설정 |
| `Server/Provider/Program.cs` | `Server/Provider/main.ts` | server-entry | done | provider role 실행 진입점 |
| `Server/Provider/ProviderHostFactory.cs` | `Server/Provider/provider-host-factory.ts` | server-role | done | NestJS framework, location store, channel, route 설정 |
| `Server/Provider/Configuration/ServerOptions.cs` | `Server/Provider/Configuration/server-options.ts`, `feature-map.ko.md` | configuration | done | rid/channel/route/weight option 구현 |
| `Server/Provider/Endpoints/ProviderEndpoints.cs` | `Server/Provider/Endpoints/provider-endpoints.ts` | endpoints | done | profile, route, evidence, shutdown HTTP 표면 |
| `Server/Provider/Handlers/ProviderHandlers.cs` | `Server/Provider/Handlers/provider-handlers.ts` | handlers | done | profile request/send, payload, route, dispatch observer |
| `Server/Provider/Infrastructure/EvidenceStore.cs` | `Server/Provider/Infrastructure/evidence-store.ts` | infrastructure | done | provider evidence 저장과 bounded wait |
| `Server/Workflow/RegistryMessaging.Workflow.csproj` | `Server/Workflow/package.json`, `Server/Workflow/tsconfig.json` | project | done | workflow role build 설정 |
| `Server/Workflow/Program.cs` | `Server/Workflow/main.ts` | server-entry | done | workflow role 실행 진입점 |
| `Server/Workflow/WorkflowHostFactory.cs` | `Server/Workflow/workflow-host-factory.ts` | server-role | done | workflow channel 설정 |
| `Server/Workflow/Configuration/ServerOptions.cs` | `Server/Workflow/Configuration/server-options.ts` | configuration | done | workflow CLI option parsing |
| `Server/Workflow/Endpoints/WorkflowEndpoints.cs` | `Server/Workflow/Endpoints/workflow-endpoints.ts` | endpoints | done | workflow request, evidence, shutdown HTTP 표면 |
| `Server/Workflow/Handlers/WorkflowHandlers.cs` | `Server/Workflow/Handlers/workflow-handlers.ts` | handlers | done | workflow request handler와 dispatch observer |
| `Server/Workflow/Infrastructure/EvidenceStore.cs` | `Server/Workflow/Infrastructure/evidence-store.ts` | infrastructure | done | workflow evidence 저장과 bounded wait |
| `Server/Consumer/RegistryMessaging.Consumer.csproj` | `Server/Consumer/package.json`, `Server/Consumer/tsconfig.json` | project | done | consumer role build 설정 |
| `Server/Consumer/Program.cs` | `Server/Consumer/main.ts` | server-entry | done | consumer role 실행 진입점 |
| `Server/Consumer/ConsumerHostFactory.cs` | `Server/Consumer/consumer-host-factory.ts` | server-role | done | client-only profile channel 설정 |
| `Server/Consumer/Configuration/ConsumerOptions.cs` | `Server/Consumer/Configuration/consumer-options.ts`, `feature-map.ko.md` | configuration | done | provider endpoint와 Redis location store option을 구현한다. |
| `Server/Consumer/Endpoints/ConsumerEndpoints.cs` | `Server/Consumer/Endpoints/consumer-endpoints.ts` | endpoints | done | topology, batch, timeout, missing, payload, backpressure HTTP 표면 |
| 없음 | `logs/.gitignore` | config | done | Node 실행 로그 디렉터리 추적용 |

## Public Contract 확인 사항

- `RM-C7`은 `.NET`에서 `ChannelName(...).SetWeight(...)`를 사용한다. Node는 같은 의미의
  `addRouteMesh(...).channel(...).server().setWeight(...)`로 build-time provider weight를 설정한다.
- `RM-C9`의 10.0.0 Node exact interface는 one-way call에 bounded
  `submit()`을 제공한다. 현재 pressure/recovery 증거는 보존하되 최초 시도, bounded wait와 timeout의
  public admission 결과를 직접 검증해야 한다.
- `.NET` README에는 `RM-A6`이 빠져 있지만 `.NET feature-map`, `Client/Program.cs`, 공통 문서에는
  존재하므로 Node 포팅 대상에 포함한다.

## 후속 계약 판정

| Scenario | 판정 | 다음 작업 |
|----------|------|-----------|
| `RM-C9` | 10.0.0 전환 대상 | `submit()`의 최초 non-blocking 시도와 bounded admission 결과를 검증한다. |
