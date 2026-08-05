# Kotlin RegistrationCodec .NET 기준 포팅 인벤토리

기준 구현: `framework/languages/dotnet/e2e/RegistrationCodec`

공통 문서: `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md`

현재 Kotlin RegistrationCodec E2E는 `Shared`, plain HTTP `Client`, `Server/Main`,
`Server/CodecRequester`, `Server/JsonOnlyPeer`, `Server/InvalidDuplicate` Gradle project로 process 역할을
나눠 실행한다. Client는 framework runtime에 참여하지 않고 `Server/Main`과 `Server/CodecRequester`의
HTTP endpoint를 호출한다. role별 package, scenario/support 파일, CLI option parser 책임 분리는 `.NET`
기준 인벤토리에 맞춰 완료했다.

상태 값:

- `done`: 현재 파일이 목표 위치와 의미를 만족한다.
- `not-needed`: Kotlin 구조에서 같은 파일 단위가 필요 없으며 비고에 근거를 적었다.
- `gap`: public contract 또는 runtime 지원이 없어 완료로 주장할 수 없다.

| .NET 기준 파일 | Kotlin 대응 파일 | 분류 | 상태 | 비고 |
|----------------|------------------|------|------|------|
| `.gitignore` | `.gitignore` | config-root | done | Gradle 산출물과 logs 제외는 유지한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | role별 Gradle project runner 검증 결과와 완료 상태를 반영했다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | role별 installDist binary를 시작하고 readiness, cleanup, 실패 로그 출력을 수행한다. |
| `Shared/RegistrationCodec.Shared.csproj` | `Shared/build.gradle.kts` | build | done | Shared Gradle project를 만들고 client/server role project가 의존한다. |
| `Shared/Messages.cs` | `Shared/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/Contracts.kt` | shared | done | request/reply/command/evidence 타입이 Shared project에 있다. 파일 이름은 Kotlin 기존 contract naming을 유지한다. |
| `Client/RegistrationCodec.Client.csproj` | `Client/build.gradle.kts` | build | done | Client application project는 framework/Spring dependency 없이 HTTP client driver로 실행된다. |
| `Client/Program.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/Program.kt` | client-entry | done | Client binary entry point가 `ClientScenario`를 plain JVM process로 실행한다. |
| `Client/Support/ClientOptions.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Support/ClientOptions.kt` | support | done | server HTTP endpoint, codec requester HTTP endpoint, log dir, mode를 CLI argument로 파싱한다. |
| `Client/Support/CodecScenarioResult.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Support/CodecScenarioResult.kt` | support | done | codec scenario response/result helper를 client support로 분리했다. |
| `Client/Support/EvidenceText.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Support/ScenarioHttpClient.kt` | support | done | HTTP POST와 bounded `/evidence/wait` 호출을 담당하는 client support로 분리했다. |
| `Client/Support/ProcessSupport.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Support/ProcessSupport.kt` | support | not-needed | process orchestration은 `run_e2e.sh`가 맡는다. Kotlin 파일은 client mode 상수만 둔다. |
| `Client/Support/ScenarioAssert.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Support/ScenarioAssert.kt` | support | done | assertion/wait helper를 client support로 분리했다. |
| `Client/Scenarios/AutoRegistrationScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/AutoRegistrationScenario.kt` | scenario | done | RC-A1 scenario file로 분리했다. |
| `Client/Scenarios/AttributeRegistrationScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/AttributeRegistrationScenario.kt` | scenario | done | RC-A2 scenario file로 분리했다. |
| `Client/Scenarios/ManualRegistrationScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/ManualRegistrationScenario.kt` | scenario | done | RC-A3 scenario file로 분리했다. |
| `Client/Scenarios/RcA4DiLifecycleScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/RcA4DiLifecycleScenario.kt` | scenario | done | RC-A4 scenario file로 분리했다. |
| `Client/Scenarios/RcA5FilterOrderingScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/RcA5FilterOrderingScenario.kt` | scenario | done | RC-A5 scenario file로 분리했다. |
| `Client/Scenarios/InvalidRegistrationScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/InvalidRegistrationScenario.kt` | scenario | not-needed | RC-A6은 invalid server startup failure라 client scenario file 대신 `run_e2e.sh`가 별도 role process 실패를 검증한다. |
| `Client/Scenarios/RcB1JsonCodecScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/RcB1JsonCodecScenario.kt` | scenario | done | RC-B1 scenario file로 분리했다. |
| `Client/Scenarios/RcB2ProtobufCodecScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/RcB2ProtobufCodecScenario.kt` | scenario | done | RC-B2 scenario file로 분리했다. |
| `Client/Scenarios/RcB3MessagePackCodecScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/RcB3MessagePackCodecScenario.kt` | scenario | done | RC-B3 scenario file로 분리했다. |
| `Client/Scenarios/RcB4CodecCoexistenceScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/RcB4CodecCoexistenceScenario.kt` | scenario | done | RC-B4 scenario file로 분리했다. |
| `Client/Scenarios/CodecMismatchScenario.cs` | `Client/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/client/Scenarios/CodecMismatchScenario.kt` | scenario | done | RC-B5 client-side mismatch flow를 scenario file로 분리했다. |
| `Server/Main/RegistrationCodec.Server.csproj` | `Server/Main/build.gradle.kts` | build | done | Main server role application project를 만들었다. |
| `Server/Main/Program.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/Program.kt` | server-entry | done | Main server binary entry point가 `main` package application을 실행한다. |
| `Server/Main/RegistrationCodecServerHostFactory.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Program.kt` (`ServerApplication`) | server-role | done | codec registry, channel, handler/filter 등록이 Main server role package에서 실행된다. |
| `Server/Main/ServerOptions.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Configuration/ServerOptions.kt` | configuration | done | server endpoint, HTTP endpoint, codec mode, log dir를 Spring application CLI argument로 파싱한다. |
| `Server/Main/Endpoints/OperationalEndpoints.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Endpoints/OperationalEndpoints.kt` | endpoints | done | health/evidence HTTP endpoint를 Main role endpoint package로 옮겼다. |
| `Server/Main/Endpoints/RegistrationScenarioEndpoints.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Endpoints/RegistrationScenarioEndpoints.kt` | endpoints | done | Client가 framework channel client를 직접 들지 않도록 `/registration/*`와 `/codec/*` HTTP endpoint에서 public framework call을 수행한다. |
| `Server/Main/DispatchFilters.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Handlers/DispatchFilters.kt` | handlers | done | order filters를 Main role handler package로 옮겼다. |
| `Server/Main/Handlers/CodecHandlers.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Handlers/CodecHandlers.kt` | handlers | done | codec handlers를 Main role handler package로 옮겼다. |
| `Server/Main/Handlers/DiEchoRequestHandler.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Handlers/RegistrationHandlers.kt` | handlers | done | DI lifecycle handler는 registration handler 파일에 남기고 DI dependency probes는 infrastructure로 분리했다. |
| `Server/Main/Handlers/RegistrationHandlers.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Handlers/RegistrationHandlers.kt` | handlers | done | registration handlers를 Main role handler package로 옮겼다. |
| `Server/Main/Infrastructure/EvidenceStore.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Infrastructure/EvidenceStore.kt` | infrastructure | done | role evidence store를 Main role infrastructure package로 옮겼다. |
| `Server/Main/Infrastructure/Probes.cs` | `Server/Main/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/main/Infrastructure/Probes.kt` | infrastructure | done | DI dispose count probe dependencies를 Main role infrastructure package로 옮겼다. |
| `Server/CodecRequester/RegistrationCodec.CodecRequester.csproj` | `Server/CodecRequester/build.gradle.kts` | build | done | CodecRequester role project를 만들었다. |
| `Server/CodecRequester/Program.cs` | `Server/CodecRequester/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/Program.kt` | server-entry | done | CodecRequester binary entry point가 `codecrequester` package application을 실행한다. |
| `Server/CodecRequester/CodecRequesterHostFactory.cs` | `Server/CodecRequester/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/codecrequester/Program.kt` (`CodecRequesterApplication`) | server-role | done | JSON 회복 요청과 Protobuf mismatch probe를 HTTP endpoint로 노출하는 별도 requester role을 만들었다. |
| `Server/CodecRequester/CodecRequesterOptions.cs` | `Server/CodecRequester/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/codecrequester/Configuration/CodecRequesterOptions.kt` | configuration | done | requester HTTP endpoint, channel endpoint, log dir를 CLI option으로 파싱한다. |
| `Server/JsonOnlyPeer/RegistrationCodec.JsonOnlyPeer.csproj` | `Server/JsonOnlyPeer/build.gradle.kts` | build | done | JSON-only peer role project를 만들었다. |
| `Server/JsonOnlyPeer/Program.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/Program.kt` | server-entry | done | JsonOnlyPeer binary entry point가 `jsononlypeer` package application을 실행한다. |
| `Server/JsonOnlyPeer/RegistrationCodecServerHostFactory.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Program.kt` (`ServerApplication`) | server-role | done | JsonOnlyPeer role package가 JSON-only codec mode로 실행된다. |
| `Server/JsonOnlyPeer/ServerOptions.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Configuration/ServerOptions.kt` | configuration | done | json-only peer endpoint, HTTP endpoint, log dir, codec mode를 Spring application CLI argument로 파싱한다. |
| `Server/JsonOnlyPeer/OperationalEndpoints.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Endpoints/OperationalEndpoints.kt` | endpoints | done | evidence HTTP endpoint를 JsonOnlyPeer role endpoint package로 옮겼다. |
| `Server/JsonOnlyPeer/DispatchFilters.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Handlers/DispatchFilters.kt` | handlers | done | dispatch filters를 JsonOnlyPeer role handler package로 옮겼다. |
| `Server/JsonOnlyPeer/Handlers/CodecHandlers.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Handlers/CodecHandlers.kt` | handlers | done | codec mismatch 검증에 필요한 handlers를 JsonOnlyPeer role handler package로 옮겼다. |
| `Server/JsonOnlyPeer/Handlers/DiEchoRequestHandler.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Handlers/RegistrationHandlers.kt` | handlers | done | JsonOnlyPeer role도 Main과 같은 registration/DI handler file layout을 따른다. |
| `Server/JsonOnlyPeer/Handlers/RegistrationHandlers.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Handlers/RegistrationHandlers.kt` | handlers | done | registration variant handlers를 JsonOnlyPeer role handler package로 옮겼다. |
| `Server/JsonOnlyPeer/Infrastructure/EvidenceStore.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Infrastructure/EvidenceStore.kt` | infrastructure | done | evidence store를 JsonOnlyPeer role infrastructure package로 옮겼다. |
| `Server/JsonOnlyPeer/Infrastructure/Probes.cs` | `Server/JsonOnlyPeer/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/jsononlypeer/Infrastructure/Probes.kt` | infrastructure | done | DI probe dependencies를 JsonOnlyPeer role infrastructure package로 옮겼다. |
| `Server/InvalidDuplicate/RegistrationCodec.InvalidDuplicate.csproj` | `Server/InvalidDuplicate/build.gradle.kts` | build | done | InvalidDuplicate role project를 만들었다. |
| `Server/InvalidDuplicate/Program.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/Program.kt` | server-entry | done | InvalidDuplicate binary entry point가 invalid duplicate application만 실행한다. |
| `Server/InvalidDuplicate/RegistrationCodecServerHostFactory.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/InvalidServerApplication.kt` (`InvalidDuplicateApplication`) | server-role | done | duplicate packet registration startup failure를 별도 role project에서 검증한다. |
| `Server/InvalidDuplicate/ServerOptions.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Configuration/ServerOptions.kt` | configuration | done | invalid server endpoint와 log dir를 CLI option으로 파싱한다. |
| `Server/InvalidDuplicate/OperationalEndpoints.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Endpoints/OperationalEndpoints.kt` | endpoints | not-needed | invalid duplicate role은 startup failure가 목표라 HTTP endpoint가 열리지 않아야 한다. |
| `Server/InvalidDuplicate/DispatchFilters.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Handlers/DispatchFilters.kt` | handlers | not-needed | duplicate registration 검증에는 filter가 필요 없다. |
| `Server/InvalidDuplicate/Handlers/CodecHandlers.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Handlers/CodecHandlers.kt` | handlers | not-needed | duplicate registration 검증에는 codec handler가 필요 없다. |
| `Server/InvalidDuplicate/Handlers/DiEchoRequestHandler.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Handlers/DiEchoRequestHandler.kt` | handlers | not-needed | duplicate registration 검증에는 DI handler가 필요 없다. |
| `Server/InvalidDuplicate/Handlers/RegistrationHandlers.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/handlers/RegistrationHandlers.kt` | handlers | done | duplicate packet registration을 만드는 최소 handler 구성을 별도 role project에 뒀다. |
| `Server/InvalidDuplicate/Infrastructure/EvidenceStore.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Infrastructure/EvidenceStore.kt` | infrastructure | not-needed | startup failure 검증이라 evidence store가 필요 없다. |
| `Server/InvalidDuplicate/Infrastructure/Probes.cs` | `Server/InvalidDuplicate/src/main/kotlin/systems/zlink/e2e/kotlin/registrationcodec/invalidduplicate/Infrastructure/Probes.kt` | infrastructure | not-needed | startup failure 검증이라 probes가 필요 없다. |

## 기존 Kotlin 파일 처리

| 기존 Kotlin 파일 | 판단 | 목표 |
|------------------|------|------|
| `src/main/kotlin/.../Program.kt` | role env 분기를 제거하고 role별 project entry point로 나눴다. | role별 binary entry point로 유지한다. |
| `src/main/kotlin/.../ClientApplication.kt` | client framework 설정과 scenario 실행이 섞여 있었다. | 제거했다. Client는 plain HTTP driver이고 framework 설정은 role server 안에만 있다. |
| `src/main/kotlin/.../ClientScenario.kt` | 모든 scenario와 helper가 한 파일에 섞여 있다. | scenario ID별 파일과 `Client/Support`로 나눈다. |
| `src/main/kotlin/.../Contracts.kt` | shared message/evidence 타입을 `Shared` project로 옮겼다. | Kotlin 기존 contract naming으로 유지한다. |
| `src/main/kotlin/.../DiDependencies.kt` | server DI lifecycle support다. | Main server handler/infrastructure로 옮긴다. |
| `src/main/kotlin/.../Env.kt` | 전역 환경 변수 helper다. | role별 CLI option parser로 대체한다. |
| `src/main/kotlin/.../EvidenceHttpServer.kt` | server operational endpoint다. | Main role은 `Endpoints/OperationalEndpoints.kt`와 `RegistrationScenarioEndpoints.kt`로 evidence와 scenario HTTP endpoint를 함께 제공한다. |
| `src/main/kotlin/.../Filters.kt` | server dispatch filter다. | server role의 `Handlers/DispatchFilters.kt`로 옮긴다. |
| `src/main/kotlin/.../InvalidServerApplication.kt` | invalid duplicate role project로 옮겼다. | duplicate registration startup failure를 만드는 최소 host로 유지한다. |
| `src/main/kotlin/.../ServerApplication.kt` | `Server/Main`과 `Server/JsonOnlyPeer` project로 분리했다. | 각 role의 CLI option parser와 host package가 분리되어 있다. |
| `src/main/kotlin/.../ScenarioState.kt` | server evidence store다. | 각 server role의 `Infrastructure/EvidenceStore.kt`로 옮긴다. |
| `src/main/kotlin/.../handlers/CodecHandlers.kt` | codec handlers다. | 필요한 server role의 `Handlers/CodecHandlers.kt`로 옮긴다. |
| `src/main/kotlin/.../handlers/RegistrationHandlers.kt` | registration/DI handlers다. | 필요한 server role의 `Handlers/RegistrationHandlers.kt`와 `DiEchoRequestHandler.kt`로 나눈다. |

## Scenario ID 매핑

| Scenario ID | 공통 우선순위 | .NET 기준 scenario 파일 | Kotlin 목표 파일 | 상태 |
|-------------|---------------|-------------------------|------------------|------|
| `RC-A1` | P0 | `Client/Scenarios/AutoRegistrationScenario.cs` | `Client/.../Scenarios/AutoRegistrationScenario.kt` | done |
| `RC-A2` | P0 | `Client/Scenarios/AttributeRegistrationScenario.cs` | `Client/.../Scenarios/AttributeRegistrationScenario.kt` | done |
| `RC-A3` | P0 | `Client/Scenarios/ManualRegistrationScenario.cs` | `Client/.../Scenarios/ManualRegistrationScenario.kt` | done |
| `RC-A4` | P1 | `Client/Scenarios/RcA4DiLifecycleScenario.cs` | `Client/.../Scenarios/RcA4DiLifecycleScenario.kt` | done |
| `RC-A5` | P1 | `Client/Scenarios/RcA5FilterOrderingScenario.cs` | `Client/.../Scenarios/RcA5FilterOrderingScenario.kt` | done |
| `RC-A6` | P0 | `Client/Scenarios/InvalidRegistrationScenario.cs` | `run_e2e.sh` invalid role startup failure check | not-needed |
| `RC-B1` | P0 | `Client/Scenarios/RcB1JsonCodecScenario.cs` | `Client/.../Scenarios/RcB1JsonCodecScenario.kt` | done |
| `RC-B2` | P0 | `Client/Scenarios/RcB2ProtobufCodecScenario.cs` | `Client/.../Scenarios/RcB2ProtobufCodecScenario.kt` | done |
| `RC-B3` | P1 | `Client/Scenarios/RcB3MessagePackCodecScenario.cs` | `Client/.../Scenarios/RcB3MessagePackCodecScenario.kt` | done |
| `RC-B4` | P0 | `Client/Scenarios/RcB4CodecCoexistenceScenario.cs` | `Client/.../Scenarios/RcB4CodecCoexistenceScenario.kt` | done |
| `RC-B5` | P1 | `Client/Scenarios/CodecMismatchScenario.cs` | `Client/.../Scenarios/CodecMismatchScenario.kt` | done |
