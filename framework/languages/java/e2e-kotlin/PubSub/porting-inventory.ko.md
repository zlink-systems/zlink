# Kotlin PubSub .NET 기준 포팅 인벤토리

기준 구현: `framework/languages/dotnet/e2e/PubSub`

공통 문서: `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`

현재 Kotlin PubSub E2E는 `.NET` 기준처럼 `Shared`, `Client`, `Server/Publisher`,
`Server/Subscriber` Gradle project로 역할을 나눈다. registry role은 location store 전환 뒤 삭제했다.
runner는 publisher/subscriber role binary를 별도 process로 실행하고, 두 role에 같은 Redis location
store endpoint와 실행별 key prefix를 넘긴다. client scenario는 publisher HTTP endpoint로 publish를
트리거한 뒤 실제 subscriber 역할 server의 bounded `/evidence/wait`와 snapshot evidence를 확인한다.

상태 값:

- `done`: 현재 파일이 목표 위치와 의미를 만족한다.
- `gap`: public contract 또는 runtime 지원이 없어 완료로 주장할 수 없다.
- `not-needed`: Kotlin 구조에서는 별도 파일이 필요 없고 다른 책임 위치에서 같은 의미를 만족한다.

| .NET 기준 파일 | Kotlin 대응 파일 | 분류 | 상태 | 비고 |
|----------------|------------------|------|------|------|
| `.gitignore` | `.gitignore` | config-root | done | Gradle 산출물과 logs 제외는 유지한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | `.NET feature-map`처럼 Pub/Sub subscriber 역할 server의 bounded `/evidence/wait`를 성공 기준으로 반영했다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | registry role 없이 publisher/subscriber role project binary를 시작하고 Redis location endpoint/key prefix, readiness, cleanup, 실패 로그 출력을 처리한다. |
| `Shared/PubSub.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Kotlin Shared project로 분리했다. |
| `Shared/Messages.cs` | `Shared/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/shared/Messages.kt` | shared | done | 기존 `Contracts.kt`의 `EventMsg`, `EvidenceEntry`, `EvidenceSnapshot`을 Shared로 옮겼다. |
| `Client/PubSub.Client.csproj` | `Client/build.gradle.kts` | build | done | Client application project로 분리했다. |
| `Client/Program.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Program.kt` | client-entry | done | Client는 Spring framework client를 들지 않고 publisher role HTTP endpoint를 호출한다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Support/ClientOptions.kt` | support | done | Client CLI option parsing으로 분리했다. |
| `Client/Support/Evidence.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Support/ScenarioContext.kt` | support | done | subscriber `/evidence/wait`와 snapshot helper를 client support context로 분리했다. |
| `Client/Support/ScenarioAssert.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Support/ScenarioContext.kt` | support | done | assertion과 wait helper를 client support context로 분리했다. |
| `Client/Support/ServerProcessLauncher.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Support/ServerProcessLauncher.kt` | support | done | PS-A4 reconnect subscriber와 PS-B2 restarted publisher는 Client scenario/support가 직접 시작하고 종료하며, 재시작 process에도 같은 Redis location endpoint/key prefix를 전달한다. |
| `Client/Scenarios/FanoutBasicDeliveryScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/FanoutBasicDeliveryScenario.kt` | scenario | 10.0.0 전환 대상 | 세 subscriber의 `ConnectionReady` 뒤 측정 구간을 시작하는 readiness evidence가 남아 있다. |
| `Client/Scenarios/TopicFilterScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/TopicFilterScenario.kt` | scenario | done | PS-A2 scenario entry가 support context를 호출한다. |
| `Client/Scenarios/LateSubscriberScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/LateSubscriberScenario.kt` | scenario | done | PS-A3 scenario entry가 support context를 호출한다. |
| `Client/Scenarios/SubscriberReconnectScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/SubscriberReconnectScenario.kt` | scenario | done | PS-A4 scenario entry가 support context를 호출한다. |
| `Client/Scenarios/SlowSubscriberScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/SlowSubscriberScenario.kt` | scenario | done | PS-B1 scenario entry가 support context를 호출한다. |
| `Client/Scenarios/PublisherRestartScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/PublisherRestartScenario.kt` | scenario | 10.0.0 전환 대상 | 같은 endpoint 재시작과 row 교체 뒤 새 publisher `ConnectionReady`를 직접 확인해야 한다. |
| `Client/Scenarios/MissingMessageNameScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/client/Scenarios/MissingMessageNameScenario.kt` | scenario | done | PS-C1 scenario entry가 support context를 호출한다. |
| `Server/Registry/PubSub.Registry.csproj` | 없음 | build | not-needed | location store 전환 뒤 registry role project를 삭제했다. |
| `Server/Registry/Program.cs` | 없음 | server-entry | not-needed | registry process를 띄우지 않으므로 별도 진입점이 없다. |
| `Server/Registry/RegistryHostFactory.cs` | 없음 | server-role | not-needed | publisher/subscriber가 Redis location store를 직접 등록하므로 embedded registry host가 필요 없다. |
| `Server/Registry/Configuration/HostFactorySupport.cs` | 없음 | configuration | not-needed | registry CLI option은 더 이상 없다. |
| `Server/Registry/Configuration/RegistryOptions.cs` | 없음 | configuration | not-needed | registry pub/router endpoint를 받지 않는다. |
| `Server/Registry/Configuration/ServerArgs.cs` | 없음 | configuration | not-needed | registry role argument parser가 필요 없다. |
| `Server/Registry/OperationalEndpoints.cs` | 없음 | endpoints | not-needed | registry health endpoint를 기다리지 않는다. |
| `Server/Registry/EvidenceStore.cs` | 없음 | infrastructure | not-needed | scenario evidence는 subscriber role이 기록한다. |
| `Server/Publisher/PubSub.Publisher.csproj` | `Server/Publisher/build.gradle.kts` | build | done | Publisher role application project로 분리했다. |
| `Server/Publisher/Program.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Program.kt` | server-entry | done | publisher와 client 책임을 분리했다. |
| `Server/Publisher/PublisherHostFactory.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Program.kt` (`PublisherApplication`) | server-role | done | fanout server configuration과 Redis location store bean 등록을 publisher role로 옮겼다. |
| `Server/Publisher/Configuration/HostFactorySupport.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Configuration/PublisherOptions.kt` | configuration | done | Kotlin publisher role은 host factory helper 대신 `PublisherOptions`로 필요한 실행 설정을 주입한다. |
| `Server/Publisher/Configuration/PublisherOptions.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Configuration/PublisherOptions.kt` | configuration | done | publisher endpoint, HTTP endpoint, Redis location endpoint, key prefix, log dir를 CLI option으로 파싱한다. |
| `Server/Publisher/Configuration/ServerArgs.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Configuration/PublisherOptions.kt` | configuration | done | 공통 server argument parser 역할은 `PublisherOptions.parse()`가 맡는다. |
| `Server/Publisher/Endpoints/OperationalEndpoints.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Endpoints/PublisherEndpoints.kt` | endpoints | done | `/health`와 PS-B2용 `/shutdown` operational endpoint를 publisher role에 둔다. |
| `Server/Publisher/Endpoints/PublisherEndpoints.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Endpoints/PublisherEndpoints.kt` | endpoints | done | Client가 직접 fanout client를 들지 않고 publisher HTTP endpoint를 호출한다. |
| `Server/Publisher/EvidenceDispatchErrorObserver.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Program.kt` | handlers | not-needed | PS-C1의 negative oracle은 subscriber dispatch observer evidence다. publisher submit에는 dispatch error marker가 없으므로 별도 publisher observer를 두지 않는다. |
| `Server/Publisher/EvidenceStore.cs` | `Server/Publisher/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/publisher/Endpoints/PublisherEndpoints.kt` | infrastructure | not-needed | Publisher role은 publish endpoint만 제공하고 scenario evidence는 subscriber role에 기록한다. |
| `Server/Subscriber/PubSub.Subscriber.csproj` | `Server/Subscriber/build.gradle.kts` | build | done | Subscriber role application project로 분리했다. |
| `Server/Subscriber/Program.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Program.kt` | server-entry | done | 별도 subscriber 실행 진입점으로 분리했다. |
| `Server/Subscriber/SubscriberHostFactory.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Program.kt` (`SubscriberApplication`) | server-role | done | subscriber handler 등록, delay/topic 설정, Redis location store bean 등록을 role project로 옮겼다. |
| `Server/Subscriber/Configuration/HandlerDelayOptions.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Configuration/SubscriberOptions.kt` | configuration | done | handler delay option은 `SubscriberOptions.handlerDelayMillis`로 파싱한다. |
| `Server/Subscriber/Configuration/HostFactorySupport.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Configuration/SubscriberOptions.kt` | configuration | done | Kotlin subscriber role은 host factory helper 대신 `SubscriberOptions`로 필요한 실행 설정을 주입한다. |
| `Server/Subscriber/Configuration/ServerArgs.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Configuration/SubscriberOptions.kt` | configuration | done | 공통 server argument parser 역할은 `SubscriberOptions.parse()`가 맡는다. |
| `Server/Subscriber/Configuration/SubscriberOptions.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Configuration/SubscriberOptions.kt` | configuration | done | subscriber rid, topic, endpoint, delay, HTTP endpoint, Redis location endpoint, key prefix, log dir를 CLI option으로 파싱한다. |
| `Server/Subscriber/OperationalEndpoints.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Endpoints/OperationalEndpoints.kt` | endpoints | done | subscriber `/health`, `/evidence`, bounded `/evidence/wait` endpoint를 제공한다. |
| `Server/Subscriber/EvidenceStore.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Infrastructure/EvidenceStore.kt` | infrastructure | done | 기존 `ScenarioState.kt`를 subscriber evidence store로 옮겼다. |
| `Server/Subscriber/Handlers/EventMsgHandler.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Handlers/EventMsgHandler.kt` | handlers | done | 기존 `EventMsgHandler.kt`를 subscriber handler package로 옮겼다. |
| `Server/Subscriber/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Subscriber/src/main/kotlin/systems/zlink/e2e/kotlin/pubsub/subscriber/Program.kt` (`setMessageFlowObserver`) | handlers | done | dispatch error evidence 기록은 subscriber role dispatch observer로 둔다. |

## Kotlin 전용 현재 파일 처리

| 기존 Kotlin 파일 | 판단 | 목표 |
|------------------|------|------|
| `src/main/kotlin/.../Program.kt` | role env 분기라 완료 구조가 아니다. | 각 role project의 `Program.kt`로 분리한다. |
| `src/main/kotlin/.../ClientApplication.kt` | publisher와 client 실행 책임이 섞여 있다. | `Server/Publisher`와 `Client`로 나눈다. |
| `src/main/kotlin/.../ClientScenario.kt` | 모든 scenario와 helper가 한 파일에 섞여 있다. | scenario ID별 파일과 `Client/Support`로 나눈다. |
| `src/main/kotlin/.../Contracts.kt` | shared message와 evidence 타입이 섞여 있다. | `Shared/.../Messages.kt`로 옮긴다. |
| `src/main/kotlin/.../Env.kt` | 전역 환경 변수 helper다. | role별 CLI option parser로 대체한다. |
| `src/main/kotlin/.../RegistryApplication.kt` | location store 전환 뒤 필요 없다. | registry role은 삭제하고 publisher/subscriber Redis location store bean으로 대체한다. |
| `src/main/kotlin/.../SubscriberApplication.kt` | subscriber role 코드는 재사용 가능하지만 handler, endpoint, delay 설정이 섞여 있다. | `Server/Subscriber` 하위 package로 재분류한다. |
| `src/main/kotlin/.../EventMsgHandler.kt` | subscriber handler다. | `Server/Subscriber/Handlers`로 옮긴다. |
| `src/main/kotlin/.../EvidenceHttpServer.kt` | subscriber operational endpoint다. | `Server/Subscriber/Endpoints`로 옮긴다. |
| `src/main/kotlin/.../ScenarioState.kt` | subscriber evidence store다. | `Server/Subscriber/Infrastructure`로 옮긴다. |

## Scenario ID 매핑

| Scenario ID | 공통 우선순위 | .NET 기준 scenario 파일 | Kotlin 목표 파일 | 상태 |
|-------------|---------------|-------------------------|------------------|------|
| `PS-A1` | P0 | `Client/Scenarios/FanoutBasicDeliveryScenario.cs` | `Client/.../Scenarios/FanoutBasicDeliveryScenario.kt` | 10.0.0 전환 대상 |
| `PS-A2` | P0 | `Client/Scenarios/TopicFilterScenario.cs` | `Client/.../Scenarios/TopicFilterScenario.kt` | done |
| `PS-A3` | P0 | `Client/Scenarios/LateSubscriberScenario.cs` | `Client/.../Scenarios/LateSubscriberScenario.kt` | done |
| `PS-A4` | P1 | `Client/Scenarios/SubscriberReconnectScenario.cs` | `Client/.../Scenarios/SubscriberReconnectScenario.kt` | done |
| `PS-B1` | P1 | `Client/Scenarios/SlowSubscriberScenario.cs` | `Client/.../Scenarios/SlowSubscriberScenario.kt` | done |
| `PS-B2` | P1 | `Client/Scenarios/PublisherRestartScenario.cs` | `Client/.../Scenarios/PublisherRestartScenario.kt` | 10.0.0 전환 대상 |
| `PS-C1` | P0 | `Client/Scenarios/MissingMessageNameScenario.cs` | `Client/.../Scenarios/MissingMessageNameScenario.kt` | done |
