# Node.js PubSub E2E 포팅 인벤토리

이 문서는 `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`와
`.NET` PubSub E2E 구현을 기준으로 Node.js 포팅 범위를 추적한다.

## Scenario

| Scenario | .NET 기준 파일 | Node.js 대상 파일 | 상태 | 검증 내용 |
|----------|----------------|-------------------|------|-----------|
| `PS-A1` | `Client/Scenarios/FanoutBasicDeliveryScenario.cs` | `Client/Scenarios/ps-a1-fanout-basic-delivery-scenario.ts` | done | 여러 subscriber가 같은 publish 구간을 받는 marker를 실제 subscriber 역할 server evidence로 확인 |
| `PS-A2` | `Client/Scenarios/TopicFilterScenario.cs` | `Client/Scenarios/ps-a2-topic-filter-scenario.ts` | done | handler가 관심 topic만 business event로 기록하는 marker를 실제 subscriber 역할 server evidence로 확인 |
| `PS-A3` | `Client/Scenarios/LateSubscriberScenario.cs` | `Client/Scenarios/ps-a3-late-subscriber-scenario.ts` | done | transport 차단 중 event와 실제 `ConnectionReady` 뒤 event를 한 번씩 발행해 ready 전 replay 부재와 ready 후 첫 전달을 확인 |
| `PS-A4` | `Client/Scenarios/SubscriberReconnectScenario.cs` | `Client/Scenarios/ps-a4-subscriber-reconnect-scenario.ts` | done | 같은 subscriber process의 transport fault·복구 뒤 기존 subscription 자동 재적용과 disconnect 구간 replay 부재를 확인 |
| `PS-B1` | `Client/Scenarios/SlowSubscriberScenario.cs` | `Client/Scenarios/ps-b1-slow-subscriber-scenario.ts` | done | 느린 subscriber handler가 다른 subscriber 수신을 막지 않는 marker를 실제 subscriber 역할 server evidence로 확인 |
| `PS-B2` | `Client/Scenarios/PublisherRestartScenario.cs` | `Client/Scenarios/ps-b2-publisher-restart-scenario.ts` | done | terminal `Drained`와 기존 row 제거 뒤 같은 rid/endpoint 재등록·`ConnectionReady`·첫 event 전달을 확인 |
| `PS-C1` | `Client/Scenarios/MissingMessageNameScenario.cs` | `Client/Scenarios/ps-c1-missing-message-name-scenario.ts` | done | 미등록 publish packet drop과 정상 publish marker를 실제 subscriber 역할 server evidence로 확인 |

## File Mapping

| .NET 기준 파일 | Node.js 대상 파일 | 구분 | 상태 | 비고 |
|----------------|-------------------|------|------|------|
| `.gitignore` | `.gitignore`, `logs/.gitignore` | ignore | done | dist, node_modules, 실행 로그 제외 |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | scenario별 구현 상태와 검증 결과 기록 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | build, port allocation, role startup, readiness, cleanup, failure log 출력 |
| `Shared/Messages.cs` | `Shared/messages.ts` | shared-contract | done | packet 이름, topic, evidence wait request |
| `Shared/PubSub.Shared.csproj` | `Shared/messages.ts` | project | done | Node는 별도 shared package 없이 TypeScript shared module로 대응 |
| `Client/PubSub.Client.csproj` | `Client/package.json`, `Client/tsconfig.json` | project | done | client build 설정 |
| `Client/Program.cs` | `Client/main.ts` | client-entry | done | scenario 선택과 실행 |
| `Client/Scenarios/FanoutBasicDeliveryScenario.cs` | `Client/Scenarios/ps-a1-fanout-basic-delivery-scenario.ts` | scenario | done | `PS-A1` |
| `Client/Scenarios/TopicFilterScenario.cs` | `Client/Scenarios/ps-a2-topic-filter-scenario.ts` | scenario | done | `PS-A2` |
| `Client/Scenarios/LateSubscriberScenario.cs` | `Client/Scenarios/ps-a3-late-subscriber-scenario.ts` | scenario | done | `PS-A3` |
| `Client/Scenarios/SubscriberReconnectScenario.cs` | `Client/Scenarios/ps-a4-subscriber-reconnect-scenario.ts` | scenario | done | `PS-A4` |
| `Client/Scenarios/SlowSubscriberScenario.cs` | `Client/Scenarios/ps-b1-slow-subscriber-scenario.ts` | scenario | done | `PS-B1` |
| `Client/Scenarios/PublisherRestartScenario.cs` | `Client/Scenarios/ps-b2-publisher-restart-scenario.ts` | scenario | done | `PS-B2` |
| `Client/Scenarios/MissingMessageNameScenario.cs` | `Client/Scenarios/ps-c1-missing-message-name-scenario.ts` | scenario | done | `PS-C1` |
| `Client/Support/ClientOptions.cs` | `Client/Support/client-options.ts` | configuration | done | CLI option parsing |
| `Client/Support/Evidence.cs` | `Client/Support/evidence.ts` | assertion-support | done | evidence line 판정 |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/scenario-assert.ts` | assertion-support | done | eventually, connection failure 판정 |
| `Client/Support/ServerProcessLauncher.cs` | `Client/Support/server-process-launcher.ts` | harness-support | done | late/reconnect/restart process 실행 |
| `Server/Registry/*` | 없음 | server-role | not-needed | PubSub runner는 registry role 없이 publisher/subscriber endpoint를 직접 연결한다. |
| `Server/Registry/Configuration/ServerArgs.cs` | `Server/Shared/Configuration/server-options.ts` | configuration | done | publisher/subscriber가 공유하는 CLI argument parsing |
| `Server/Publisher/PubSub.Publisher.csproj` | `Server/Publisher/package.json`, `Server/Publisher/tsconfig.json` | project | done | publisher role build 설정 |
| `Server/Publisher/Program.cs` | `Server/Publisher/main.ts` | server-entry | done | publisher role 실행 |
| `Server/Publisher/PublisherHostFactory.cs` | `Server/Publisher/publisher-host-factory.ts` | server-role | done | fanout publisher, dispatch observer, HTTP server 구성 |
| `Server/Publisher/Configuration/HostFactorySupport.cs` | `Server/Publisher/publisher-host-factory.ts` | server-role | done | Node NestJS host factory 안에서 구성 |
| `Server/Publisher/Configuration/PublisherOptions.cs` | `Server/Publisher/Configuration/publisher-options.ts` | configuration | done | publisher endpoint와 evidence file option |
| `Server/Publisher/Configuration/ServerArgs.cs` | `Server/Publisher/Configuration/publisher-options.ts` | configuration | done | CLI argument parsing |
| `Server/Publisher/Endpoints/OperationalEndpoints.cs` | `Server/Publisher/Endpoints/publisher-endpoints.ts` | endpoints | done | health, evidence, clear, shutdown endpoint |
| `Server/Publisher/Endpoints/PublisherEndpoints.cs` | `Server/Publisher/Endpoints/publisher-endpoints.ts` | endpoints | done | publish event/missing endpoint |
| `Server/Publisher/EvidenceDispatchErrorObserver.cs` | `Server/Publisher/Handlers/evidence-dispatch-error-observer.ts` | observer | done | dispatch error evidence 기록 |
| `Server/Publisher/EvidenceStore.cs` | `Server/Publish../Infrastructure/evidence-store.ts` | infrastructure | done | publisher evidence 저장과 evidence file 기록 |
| `Server/Subscriber/PubSub.Subscriber.csproj` | `Server/Subscriber/package.json`, `Server/Subscriber/tsconfig.json` | project | done | subscriber role build 설정 |
| `Server/Subscriber/Program.cs` | `Server/Subscriber/main.ts` | server-entry | done | subscriber role 실행 |
| `Server/Subscriber/SubscriberHostFactory.cs` | `Server/Subscriber/subscriber-host-factory.ts` | server-role | done | fanout subscriber, handler, observer 구성 |
| `Server/Subscriber/Configuration/HandlerDelayOptions.cs` | `Server/Subscriber/Configuration/subscriber-options.ts` | configuration | done | slow handler delay 옵션 |
| `Server/Subscriber/Configuration/HostFactorySupport.cs` | `Server/Subscriber/subscriber-host-factory.ts` | server-role | done | Node NestJS host factory 안에서 구성 |
| `Server/Subscriber/Configuration/ServerArgs.cs` | `Server/Subscriber/Configuration/subscriber-options.ts` | configuration | done | CLI argument parsing |
| `Server/Subscriber/Configuration/SubscriberOptions.cs` | `Server/Subscriber/Configuration/subscriber-options.ts` | configuration | done | subscriber endpoint, evidence file, handler delay 옵션 |
| `Server/Subscriber/OperationalEndpoints.cs` | `Server/Subscriber/Endpoints/operational-endpoints.ts` | endpoints | done | health, evidence, wait, clear, shutdown endpoint |
| `Server/Subscriber/EvidenceStore.cs` | `Server/Subscrib../Infrastructure/evidence-store.ts` | infrastructure | done | subscriber evidence 저장과 bounded wait |
| `Server/Subscriber/Handlers/EventMsgHandler.cs` | `Server/Subscriber/Handlers/event-msg-handler.ts` | handler | done | topic filter, slow handler evidence |
| `Server/Subscriber/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Subscriber/Handlers/event-msg-handler.ts` | observer | done | missing packet dispatch error evidence |

## 검증 경로 판정

Pub/Sub fanout의 수신자는 client stream session이 아니라 subscriber 역할 server다. 공통 E2E README는
이 경우 subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로 사용할 수 있다고
정리한다. 따라서 이 inventory는 별도 client stream connector observer를 요구하지 않는다.
