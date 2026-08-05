# C++ PubSub .NET 기준 포팅 inventory

이 문서는 `framework/languages/dotnet/e2e/PubSub`의 파일을 기준으로 C++ `PubSub` E2E의 대응 파일과
남은 gap을 기록한다. C++ 구현은 `.NET` 기준처럼 registry 프로세스 없이 Redis location store를
공유하는 Publisher, Subscriber 역할 실행 파일로 분리한다. Pub/Sub fanout의 수신자는 subscriber 역할 server이므로, C++ 검증도 공통 README와
`.NET` feature-map이 허용한 bounded subscriber `/evidence/wait` marker를 성공 기준으로 사용한다.

## 기준

- 공통 문서: `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`
- .NET 기준 구현: `framework/languages/dotnet/e2e/PubSub`
- C++ 대상: `framework/languages/cpp/e2e/PubSub`

## 파일 매핑

| .NET 기준 파일 | C++ 대응 파일 | 분류 | 상태 | 비고 |
|----------------|---------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 실행 로그를 제외한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | PS-A/B/C 시나리오 구현 상태와 최신 `/evidence/wait` runner 증거를 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | Redis location store를 준비하고 publisher/subscriber/client process orchestration을 분리하며, `all` 또는 개별 PS scenario ID 실행을 지원한다. 검증은 subscriber role server의 `/evidence/wait`를 사용하며, publisher operational endpoint와 `verify.log` 증거도 runner가 남긴다. |
| `Shared/Messages.cs` | `Shared/pubsub_contracts.hpp` | shared | done | event/accepted evidence/ignored evidence/dispatch-error DTO와 evidence wait request가 대응된다. |
| `Shared/PubSub.Shared.csproj` | `Shared/pubsub_contracts.hpp` | build | not-needed | C++ shared contract는 header로 포함된다. |
| `Client/Program.cs` | `Client/main.cpp` | client-entry | done | scenario dispatch만 담당하고 Publisher role HTTP endpoint를 호출한다. |
| `Client/PubSub.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_pubsub_client` target이 대응한다. |
| `Client/Support/ClientOptions.cs` | `Client/Support/client_support.hpp`; `run_e2e.sh` | client-support | done | env parsing, marker 대기, Publisher HTTP 호출 helper가 대응한다. |
| `Client/Support/Evidence.cs` | `run_e2e.sh`; `Server/Subscriber/Endpoints/operational_endpoints.hpp` | client-support | done | runner Python helper가 subscriber role server의 bounded `/evidence/wait`를 호출해 evidence line을 검증하고, 각 검증 결과를 `verify.log`에 남긴다. |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/client_support.hpp`; `run_e2e.sh` | client-support | done | client process assert와 runner evidence assert가 분리되어 있고, scenario별 성공 조건은 `/evidence/wait` 결과와 `verify.log` marker로 확인한다. |
| `Client/Support/ServerProcessLauncher.cs` | `Client/Support/client_support.hpp` | client-support | done | PS-A4 reconnect subscriber와 PS-B2 restarted publisher는 Client support가 role executable을 시작하고 종료/재시작을 제어한다. 재시작 process에도 같은 Redis endpoint/key prefix를 전달한다. runner는 always-on baseline role 시작과 final cleanup만 담당한다. |
| `Client/Scenarios/FanoutBasicDeliveryScenario.cs` | `Client/Scenarios/fanout_basic_delivery_scenario.hpp`; `run_e2e.sh` | scenario | done | PS-A1 발행 흐름과 세 subscriber의 공통 sequence 수신을 `/evidence/wait`로 검증한다. |
| `Client/Scenarios/TopicFilterScenario.cs` | `Client/Scenarios/topic_filter_scenario.hpp`; `run_e2e.sh` | scenario | done | PS-A2 발행 흐름과 accepted/ignored topic evidence를 `/evidence/wait`로 검증한다. |
| `Client/Scenarios/LateSubscriberScenario.cs` | `Client/Scenarios/late_subscriber_scenario.hpp`; `run_e2e.sh` | scenario | done | PS-A3 발행 흐름과 late subscriber 합류/비replay 조건을 `/evidence/wait`와 negative line check로 검증한다. |
| `Client/Scenarios/SubscriberReconnectScenario.cs` | `Client/Scenarios/subscriber_reconnect_scenario.hpp`; `Client/Support/client_support.hpp` | scenario | done | PS-A4 발행 흐름과 subscriber restart orchestration, 재구독 이후 수신/끊김 구간 비replay 조건을 Client가 제어하고 검증한다. |
| `Client/Scenarios/SlowSubscriberScenario.cs` | `Client/Scenarios/slow_subscriber_scenario.hpp`; `run_e2e.sh` | scenario | done | PS-B1 발행 흐름과 빠른 subscriber 격리 수신을 `/evidence/wait`로 검증한다. |
| `Client/Scenarios/PublisherRestartScenario.cs` | `Client/Scenarios/publisher_restart_scenario.hpp`; `Client/Support/client_support.hpp` | scenario | done | PS-B2 발행 흐름과 Publisher role server shutdown/restart를 Client가 제어하고, restart 이후 수신을 `/evidence/wait`로 검증한다. |
| `Client/Scenarios/MissingMessageNameScenario.cs` | `Client/Scenarios/missing_message_name_scenario.hpp`; `run_e2e.sh` | scenario | done | PS-C1 negative 발행 흐름, subscriber dispatch error, 후속 정상 publish를 `/evidence/wait`로 검증한다. |
| `Server/Registry/*` | not-needed | server-role | not-needed | .NET 기준과 공통 문서는 registry role을 사용하지 않는다. C++ registry source와 target은 제거했고 Redis location store가 peer row를 담당한다. |
| `Server/Publisher/Configuration/HostFactorySupport.cs` | `Server/Shared/server_support.hpp` | server-role | done | 공통 logging/codec/flow helper가 대응한다. |
| `Server/Publisher/Configuration/PublisherOptions.cs` | `Server/Publisher/Configuration/publisher_options.hpp`; `run_e2e.sh` | configuration | done | publisher endpoint/log/http와 Redis endpoint/key prefix options가 대응한다. |
| `Server/Publisher/Configuration/ServerArgs.cs` | `run_e2e.sh` | configuration | done | runner env orchestration이 인자 역할을 담당한다. |
| `Server/Publisher/Endpoints/OperationalEndpoints.cs` | `Server/Publisher/main.cpp`; `Server/Shared/server_support.hpp` | endpoint | done | Publisher role이 `/health`, `/evidence`, `/evidence/clear`, `/shutdown`을 제공하고 runner가 최초 시작과 재시작 뒤 endpoint 동작을 검증한다. |
| `Server/Publisher/Endpoints/PublisherEndpoints.cs` | `Server/Publisher/Endpoints/publisher_endpoints.hpp` | endpoint | done | `/publish/event`와 `/publish/missing` endpoint가 Publisher role에서 framework publish를 실행한다. |
| `Server/Publisher/EvidenceDispatchErrorObserver.cs` | not-needed | handler | not-needed | 현재 C++ Publisher role evidence는 판정에 쓰지 않는다. subscriber dispatch error evidence로 PS-C1을 확인한다. |
| `Server/Publisher/EvidenceStore.cs` | `Server/Shared/server_support.hpp`; `Server/Publisher/Endpoints/publisher_endpoints.hpp` | infrastructure | done | Publisher role operational evidence store가 `/publish/event`와 `/publish/missing` 호출 marker를 기록하고 runner가 final snapshot을 남긴다. |
| `Server/Publisher/Program.cs` | `Server/Publisher/main.cpp` | server-entry | done | publisher 전용 executable이다. |
| `Server/Publisher/PublisherHostFactory.cs` | `Server/Publisher/main.cpp`; `Server/Shared/server_support.hpp` | server-role | done | publisher framework 구성이 role entry에 있다. |
| `Server/Publisher/PubSub.Publisher.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_pubsub_publisher` target이 대응하며 `zlink::framework_locations_redis`를 링크한다. |
| `Server/Subscriber/Configuration/HandlerDelayOptions.cs` | `Server/Subscriber/Configuration/subscriber_options.hpp`; `run_e2e.sh` | configuration | done | handler delay env parsing이 대응한다. |
| `Server/Subscriber/Configuration/HostFactorySupport.cs` | `Server/Shared/server_support.hpp` | server-role | done | 공통 logging/codec/flow helper가 대응한다. |
| `Server/Subscriber/Configuration/ServerArgs.cs` | `run_e2e.sh` | configuration | done | runner env orchestration이 인자 역할을 담당한다. |
| `Server/Subscriber/Configuration/SubscriberOptions.cs` | `Server/Subscriber/Configuration/subscriber_options.hpp`; `run_e2e.sh` | configuration | done | subscriber id/topic/http와 Redis endpoint/key prefix options가 대응한다. Subscriber는 publisher endpoint를 직접 받지 않고 location store에서 publisher peer row를 발견해 연결한다. |
| `Server/Subscriber/EvidenceStore.cs` | `Server/Subscriber/Infrastructure/evidence_store.hpp` | infrastructure | done | subscriber accepted/ignored/error evidence store와 bounded evidence wait matching이 대응한다. |
| `Server/Subscriber/Handlers/EventMsgHandler.cs` | `Server/Subscriber/Handlers/event_msg_handler.hpp` | handler | done | topic별 publish handler가 대응한다. |
| `Server/Subscriber/Handlers/EvidenceDispatchErrorObserver.cs` | `Server/Subscriber/Infrastructure/evidence_store.hpp`; `Server/Subscriber/main.cpp` | handler | done | dispatch observer가 evidence store에 error marker를 기록한다. |
| `Server/Subscriber/OperationalEndpoints.cs` | `Server/Subscriber/Endpoints/operational_endpoints.hpp` | endpoint | done | `/evidence`와 `/evidence/wait` endpoint가 대응한다. |
| `Server/Subscriber/Program.cs` | `Server/Subscriber/main.cpp` | server-entry | done | subscriber 전용 executable이다. |
| `Server/Subscriber/SubscriberHostFactory.cs` | `Server/Subscriber/main.cpp`; `Server/Shared/server_support.hpp` | server-role | done | subscriber framework 구성이 role entry에 있다. |
| `Server/Subscriber/PubSub.Subscriber.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_pubsub_subscriber` target이 대응하며 `zlink::framework_locations_redis`를 링크한다. |

## 공통 scenario ID 대응

| Scenario ID | C++ 대응 파일 | 상태 | 비고 |
|-------------|---------------|------|------|
| `PS-A1` | `Client/Scenarios/fanout_basic_delivery_scenario.hpp`; `run_e2e.sh` | done | fanout 발행과 세 subscriber의 공통 sequence 수신을 `/evidence/wait`로 검증한다. |
| `PS-A2` | `Client/Scenarios/topic_filter_scenario.hpp`; `run_e2e.sh` | done | 관심 topic accepted evidence와 비관심 topic ignored evidence를 `/evidence/wait`로 검증한다. |
| `PS-A3` | `Client/Scenarios/late_subscriber_scenario.hpp`; `run_e2e.sh` | done | late subscriber 합류 흐름과 합류 전 발행분 비replay를 검증한다. |
| `PS-A4` | `Client/Scenarios/subscriber_reconnect_scenario.hpp`; `Client/Support/client_support.hpp` | done | Client가 reconnect subscriber process를 중지/재시작하고 끊김 구간 비replay를 검증한다. |
| `PS-B1` | `Client/Scenarios/slow_subscriber_scenario.hpp`; `run_e2e.sh` | done | slow subscriber 격리 흐름을 빠른 subscriber evidence wait로 검증한다. |
| `PS-B2` | `Client/Scenarios/publisher_restart_scenario.hpp`; `Client/Support/client_support.hpp` | done | Client가 Publisher role server shutdown/restart를 제어하고 재시작 이후 발행한 값을 검증한다. |
| `PS-C1` | `Client/Scenarios/missing_message_name_scenario.hpp`; `run_e2e.sh` | done | missing message name error와 후속 정상 publish를 검증한다. |

## 검증

- 2026-07-08: `timeout 420s nice -n 10 framework/languages/cpp/e2e/PubSub/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260708-123833-1298240`
  - 의미: Redis location store 기반으로 PS-A1, PS-A2, PS-A3, PS-A4, PS-B1, PS-B2,
    PS-C1 전체가 bounded subscriber evidence wait 경로로 통과했다. Subscriber role은 publisher
    endpoint를 직접 인자로 받지 않고 location store에서 publisher peer row를 발견해 연결한다.
    runner 출력은 `verify basic/topic/late/reconnect/slow/publisher-restart/negative passed`,
    `snapshot publisher evidence written`, `pubsub e2e result=passed`를 포함한다.
- 2026-07-03: `ZLINK_CPP_E2E_BUILD_DIR=/home/hep7/project/kairos/zlink/framework/languages/cpp/build-redis-vcpkg timeout 900s framework/languages/cpp/e2e/PubSub/run_e2e.sh all`
  - 결과: 통과
  - 로그: `logs/20260703-200255-8623`
  - 의미: registry role 없이 Redis location store 기반으로 PS-A1, PS-A2, PS-A3, PS-A4, PS-B1,
    PS-B2, PS-C1 전체가 bounded subscriber evidence wait 경로로 통과했다. runner 출력은
    `verify basic/topic/late/reconnect/slow/publisher-restart/negative passed`,
    `snapshot publisher evidence written`, `pubsub e2e result=passed`를 포함한다.
- 2026-07-02: `timeout 240s framework/languages/cpp/e2e/PubSub/run_e2e.sh all`
  - 결과: 통과
  - 로그: `logs/20260702-085350-88854`
  - 의미: PS-A1, PS-A2, PS-A3, PS-A4, PS-B1, PS-B2, PS-C1 전체가 bounded subscriber
    evidence wait 경로로 통과했다. runner 출력은 `verify basic/topic/late/reconnect/slow/publisher-restart/negative passed`,
    `snapshot registry evidence written`, `snapshot publisher evidence written`,
    `pubsub e2e result=passed`를 포함한다.
- 2026-07-02: `timeout 420s framework/languages/cpp/e2e/PubSub/run_e2e.sh all`
  - 결과: 통과
  - 로그: `logs/20260702-093832-63001`
  - 의미: PS-A4 reconnect subscriber lifecycle과 PS-B2 publisher shutdown/restart lifecycle을 Client support로
    옮긴 뒤에도 PS-A1, PS-A2, PS-A3, PS-A4, PS-B1, PS-B2, PS-C1 전체가 통과했다.
    출력은 `verify basic/topic/late/reconnect/slow/publisher-restart/negative passed`와
    `pubsub e2e result=passed`를 포함한다.
- 2026-06-30: `./framework/languages/cpp/e2e/PubSub/run_e2e.sh all`
  - 결과: 통과
  - 로그: `logs/20260630-082052-3253217`
  - 의미: 당시 C++ PubSub의 PS-A1, PS-A2, PS-A3, PS-A4, PS-B1, PS-B2, PS-C1 흐름은 모두
    통과했지만 검증 경로 판정이 오래되어 현재 완료 판정의 근거로는 쓰지 않는다. 최신 판정은
    아래 `/evidence/wait` 검증 기록을 따른다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/PubSub/run_e2e.sh all`
  - 결과: 통과
  - 로그: `logs/20260701-170557-98147`
  - 의미: PS-A1, PS-A2, PS-A3, PS-A4, PS-B1, PS-B2, PS-C1 전체가 bounded subscriber
    evidence wait 경로로 통과했다. `verify.log`에는 `verify basic/topic/late/reconnect/slow/publisher-restart/negative passed`
    marker가 남고, `registry-operational.log`, `publisher-operational.log`,
    `publisher-restart-operational.log`, `registry-evidence-final.json`,
    `publisher-evidence-final.json`이 registry/publisher operational endpoint 증거를 남긴다.
