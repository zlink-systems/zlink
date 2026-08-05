# Java PubSub .NET 기준 포팅 inventory

기준 문서:

- `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`
- `framework/languages/dotnet/e2e/PubSub/feature-map.ko.md`
- `framework/doc/plan/framework-java-e2e-dotnet-porting-plan.ko.md`

마지막 검증:

- 명령: `nice -n 10 timeout 600s ./run_e2e.sh all`
- 결과: passed
- 로그: `framework/languages/java/e2e/PubSub/logs/20260707-221458-3633382/`

## 완료 상태 요약

- 단일 `Program.java` role switch 구조를 제거하고 `:Client`, `:Shared`, `:Server:Publisher`,
  `:Server:Subscriber` Gradle subproject로 나누었다.
- discovery registry role은 실행 그래프에서 제외했다. runner는 실행별 Redis location store
  컨테이너를 준비하고, publisher와 subscriber는 공식 Redis location store extension을 같은 endpoint와
  실행별 key prefix로 등록한다.
- 기존 fanout, topic filter, late subscriber, subscriber reconnect, slow subscriber,
  publisher restart, missing message name scenario 구현을 보존하고 scenario별 class로 분리했다.
- PS-A4 subscriber reconnect와 PS-B2 publisher restart의 process lifecycle control은
  `Client/Support/ServerProcessLauncher.java`와 scenario class가 담당한다. runner는 기본 topology,
  readiness, client 실행, cleanup만 맡는다.
- publisher는 public `ZLinkFanoutClient`로 publish하고, client는 publisher HTTP endpoint를 호출해
  scenario를 구동한다.
- Pub/Sub fanout 검증은 공통 E2E README의 Pub/Sub 예외에 맞춰 subscriber 역할 server의 bounded
  `/evidence/wait` endpoint를 사용한다. client는 반복 snapshot polling으로 상태 변화를 기다리지
  않는다.

## .NET 파일 매핑

| .NET 기준 파일 | Java 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | root | done | logs와 모든 subproject build 산출물을 제외한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | PS-A1~PS-C1 구현 상태와 bounded evidence wait 검증 경로를 기록했다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 실행별 Redis location store 컨테이너를 준비한 뒤 publisher/subscriber/client installDist binary를 시작하고 health, evidence, message flow marker를 검증한다. PS-A4/PS-B2 process restart 제어는 Client support가 수행한다. |
| `README.ko.md` 없음 | `README.ko.md` | docs | done | `.NET`에는 없지만 Java 산출물로 역할, 실행법, 검증 경로를 기록했다. |
| `Shared/PubSub.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Java `Shared` library project다. |
| `Shared/Messages.cs` | `Shared/src/main/java/systems/zlink/e2e/pubsub/shared/Contracts.java` | shared | done | channel, packet name, `EventMsg`, evidence record를 공유한다. |
| `Client/PubSub.Client.csproj` | `Client/build.gradle.kts` | build | done | Java client application project다. |
| `Client/Program.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Program.java` | client | done | scenario 실행 순서와 mode 분기를 담당한다. |
| `Client/Scenarios/FanoutBasicDeliveryScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/FanoutBasicDeliveryScenario.java` | scenario | done | PS-A1. warm-up barrier와 공통 연속 sequence를 검증한다. |
| `Client/Scenarios/TopicFilterScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/TopicFilterScenario.java` | scenario | done | PS-A2. `ZLinkPublishContext.topic()` 기반 application-level filter를 검증한다. |
| `Client/Scenarios/LateSubscriberScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/LateSubscriberScenario.java` | scenario | done | PS-A3. late subscriber no-replay와 이후 publish 수신을 검증한다. |
| `Client/Scenarios/SubscriberReconnectScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/SubscriberReconnectScenario.java` | scenario | done | PS-A4. disconnected interval no-replay와 재구독 후 수신을 검증한다. |
| `Client/Scenarios/SlowSubscriberScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/SlowSubscriberScenario.java` | scenario | done | PS-B1. 느린 subscriber가 빠른 subscriber를 막지 않는지 검증한다. |
| `Client/Scenarios/PublisherRestartScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/PublisherRestartScenario.java` | scenario | done | PS-B2. 실제 publisher process 재시작 뒤 새 publish 도달을 검증한다. |
| `Client/Scenarios/MissingMessageNameScenario.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Scenarios/MissingMessageNameScenario.java` | scenario | done | PS-C1. subscriber dispatch error marker와 정상 publish 회복을 검증한다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Support/ClientOptions.java` | support | done | client mode, publisher/subscriber endpoint, Redis location endpoint/key prefix, build/log dir, marker file env를 해석한다. |
| `Client/Support/Evidence.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Support/Evidence.java` | support | done | subscriber `/evidence/wait`로 bounded marker wait를 수행하고, 부재 대조가 필요할 때만 `/evidence` snapshot을 읽는다. |
| `Client/Support/ScenarioAssert.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Support/ScenarioAssert.java` | support | done | wait, assertion, common sequence helper를 모았다. |
| `Client/Support/ServerProcessLauncher.cs` | `Client/src/main/java/systems/zlink/e2e/pubsub/client/Support/ServerProcessLauncher.java` | support | done | PS-A4/PS-B2에서 subscriber/publisher role binary를 Client scenario가 직접 시작하고 종료한다. |
| `Server/Publisher/PubSub.Publisher.csproj` | `Server/Publisher/build.gradle.kts` | build | done | Java publisher application project다. |
| `Server/Publisher/Program.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/Program.java` | server-role | done | publisher role entrypoint다. |
| `Server/Publisher/PublisherHostFactory.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/PublisherApplication.java` | server-role | done | Spring host와 framework 설정을 담당한다. |
| `Server/Publisher/Configuration/PublisherOptions.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/Configuration/PublisherOptions.java` | configuration | done | HTTP endpoint, fanout endpoint, Redis location endpoint/key prefix, log dir env를 해석한다. |
| `Server/Publisher/Configuration/ServerArgs.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/Configuration/PublisherOptions.java` | configuration | not-needed | Java runner는 env로 role option을 전달한다. 별도 args parser가 필요 없다. |
| `Server/Publisher/Configuration/HostFactorySupport.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/PublisherApplication.java` | configuration | not-needed | Java Spring application class가 host setup을 직접 캡슐화한다. |
| `Server/Publisher/Endpoints/OperationalEndpoints.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/Endpoints/PublisherEndpoints.java` | endpoint | done | `/health`와 `/evidence`를 제공한다. |
| `Server/Publisher/Endpoints/PublisherEndpoints.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/Endpoints/PublisherEndpoints.java` | endpoint | done | `/publish/event`와 `/publish/missing`에서 public fanout client를 호출한다. |
| `Server/Publisher/EvidenceDispatchErrorObserver.cs` | 없음 | handler | not-needed | PS-C1 oracle은 subscriber dispatch error다. publisher dispatch marker는 공통 문서 완료 기준이 아니다. |
| `Server/Publisher/EvidenceStore.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/pubsub/publisher/Infrastructure/EvidenceStore.java` | infrastructure | done | publisher endpoint 호출 evidence를 보관한다. |
| `Server/Registry/*` | 없음 | server-role | not-needed | Config 3은 Redis location store 기반 fanout 연결을 검증하므로 registry role을 실행 그래프에서 제외했다. |
| `Server/Subscriber/PubSub.Subscriber.csproj` | `Server/Subscriber/build.gradle.kts` | build | done | Java subscriber application project다. |
| `Server/Subscriber/Program.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Program.java` | server-role | done | subscriber role entrypoint다. |
| `Server/Subscriber/SubscriberHostFactory.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/SubscriberApplication.java` | server-role | done | Spring host와 framework 설정을 담당한다. |
| `Server/Subscriber/Configuration/SubscriberOptions.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Configuration/SubscriberOptions.java` | configuration | done | subscriber rid, topic, HTTP endpoint, Redis location endpoint/key prefix, log dir를 해석한다. |
| `Server/Subscriber/Configuration/HandlerDelayOptions.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Configuration/HandlerDelayOptions.java` | configuration | done | PS-B1 handler delay env를 해석한다. |
| `Server/Subscriber/Configuration/ServerArgs.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Configuration/SubscriberOptions.java` | configuration | not-needed | Java runner는 env로 role option을 전달한다. 별도 args parser가 필요 없다. |
| `Server/Subscriber/Configuration/HostFactorySupport.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/SubscriberApplication.java` | configuration | not-needed | Java Spring application class가 host setup을 직접 캡슐화한다. |
| `Server/Subscriber/OperationalEndpoints.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Endpoints/OperationalEndpoints.java` | endpoint | done | `/health`와 `/evidence`를 제공한다. |
| `Server/Subscriber/EvidenceStore.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Infrastructure/EvidenceStore.java` | infrastructure | done | subscriber evidence와 topic interest를 보관한다. |
| `Server/Subscriber/Handlers/EventMsgHandler.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Handlers/EventMsgHandler.java` | handler | done | public publish handler 구현이다. |
| `Server/Subscriber/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Subscriber/src/main/java/systems/zlink/e2e/pubsub/subscriber/Handlers/EvidenceDispatchErrorObserver.java` | handler | done | missing message name dispatch error를 evidence로 기록한다. |

## 공통 시나리오 매핑

| 시나리오 | Java 위치 | 상태 | 비고 |
|----------|-----------|------|------|
| PS-A1 | `Client/.../Scenarios/FanoutBasicDeliveryScenario.java` | done | warm-up barrier와 공통 연속 sequence 검증을 보존했다. |
| PS-A2 | `Client/.../Scenarios/TopicFilterScenario.java` | done | `ZLinkPublishContext.topic()` 기반 application-level filter를 보존했다. |
| PS-A3 | `Client/.../Scenarios/LateSubscriberScenario.java` | done | pre-late publish와 late subscriber no-replay 검증을 보존했다. |
| PS-A4 | `Client/.../Scenarios/SubscriberReconnectScenario.java` | done | Client support가 subscriber process를 시작, 종료, 재시작하고 disconnected interval no-replay와 재구독 후 수신을 검증한다. |
| PS-B1 | `Client/.../Scenarios/SlowSubscriberScenario.java` | done | 느린 subscriber와 빠른 subscriber 격리 검증을 보존했다. |
| PS-B2 | `Client/.../Scenarios/PublisherRestartScenario.java` | done | Client support가 publisher process를 시작, 종료, 재시작하고 중단 중 publish 실패와 재시작 후 도달을 검증한다. |
| PS-C1 | `Client/.../Scenarios/MissingMessageNameScenario.java` | done | subscriber observer의 `HANDLER_MISSING`/`DROP` evidence와 정상 publish 회복을 검증한다. |

## 남은 확인 사항

현재 Java `PubSub` inventory에는 남은 `gap` 또는 `partial` 항목이 없다. Pub/Sub delivery 검증은
공통 E2E README가 허용한 subscriber 역할 server의 bounded `/evidence/wait` marker를 사용한다.
