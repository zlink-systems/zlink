# Node.js RegistrationCodec E2E 포팅 인벤토리

기준 문서: `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md`

## Scenario

| Scenario | .NET 기준 파일 | Node.js 대상 파일 | 상태 | 비고 |
|----------|----------------|-------------------|------|------|
| `RC-A1` | `Client/Scenarios/AutoRegistrationScenario.cs` | `Client/Scenarios/AutoRegistrationScenario.ts` | done | provider discovery와 Node decorator variant의 request/send를 한 canonical scenario에서 검증한다. |
| `RC-A3` | `Client/Scenarios/ManualRegistrationScenario.cs` | `Client/Scenarios/ManualRegistrationScenario.ts` | done | builder 명시 등록 request/send |
| `RC-A4` | `Client/Scenarios/RcA4DiLifecycleScenario.cs` | `Client/Scenarios/RcA4DiLifecycleScenario.ts`, `Server/Main/Handlers/di-echo-handler.ts`, `Server/Main/Infrastructure/lifecycle-probes.ts` | done | singleton/scoped id evidence를 검증했다. Node/Nest public `ModuleRef`는 dispatch 뒤 context dispose API를 제공하지 않으므로 dispose counter를 완료 조건에 넣지 않는다. 최신 확인: `logs/20260702-065333-61321` |
| `RC-A5` | `Client/Scenarios/RcA5FilterOrderingScenario.cs` | `Client/Scenarios/RcA5FilterOrderingScenario.ts` | done | public `filters` registration option으로 filter ordering 확인 |
| `RC-A6` | `Client/Scenarios/InvalidRegistrationScenario.cs` | `Client/Scenarios/InvalidRegistrationScenario.ts` | done | duplicate packet startup failure |
| `RC-B1` | `Client/Scenarios/RcB1JsonCodecScenario.cs` | `Client/Scenarios/RcB1JsonCodecScenario.ts` | done | Main role에서 JSON content-type round-trip |
| `RC-B2` | `Client/Scenarios/RcB2ProtobufCodecScenario.cs` | `Client/Scenarios/RcB2ProtobufCodecScenario.ts`, `Server/Main/Endpoints/main-endpoints.ts` | done | Main role의 전역 codec registry에서 Protobuf와 MessagePack variant를 함께 확인한다. 최신 확인: `logs/20260702-065333-61321` |
| `RC-B4` | `Client/Scenarios/RcB4CodecCoexistenceScenario.cs` | `Client/Scenarios/RcB4CodecCoexistenceScenario.ts`, `Server/Main/Endpoints/main-endpoints.ts` | done | 한 host에서 JSON/Protobuf/MessagePack serializer를 함께 등록하고 payload class별 content-type을 검증했다. 최신 확인: `logs/20260702-065333-61321` |
| `RC-B5` | `Client/Scenarios/CodecMismatchScenario.cs` | `Client/Scenarios/CodecMismatchScenario.ts` | done | Protobuf requester와 JSON-only peer mismatch/recovery |

## File Mapping

| .NET 기준 파일 | Node.js 대상 파일 | 구분 | 상태 | 비고 |
|----------------|-------------------|------|------|------|
| `.gitignore` | `.gitignore`, `logs/.gitignore` | ignore | done | dist/log artifact 제외 |
| `Shared/Messages.cs`, `Shared/RegistrationCodec.Shared.csproj` | `Shared/messages.ts` | shared-contract | done | packet 이름과 evidence request |
| `Client/RegistrationCodec.Client.csproj` | `Client/package.json`, `Client/tsconfig.json` | project | done | client build 설정 |
| `Client/Program.cs` | `Client/main.ts` | client-entry | done | scenario 실행 |
| `Client/Support/ClientOptions.cs` | `Client/Support/client-options.ts` | configuration | done | CLI option parsing |
| `Client/Support/ProcessSupport.cs` | `Client/Support/process-support.ts` | support | done | invalid startup failure process 확인 |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/scenario-assert.ts` | assertion-support | done | assertion와 eventually |
| `Client/Support/EvidenceText.cs` | `Client/Scenarios/*` | assertion-support | done | evidence substring 검증 |
| `Client/Support/CodecScenarioRes.cs` | `Shared/messages.ts` | shared-contract | done | codec result DTO |
| `Server/Main/RegistrationCodec.Server.csproj` | `Server/Main/package.json`, `Server/Main/tsconfig.json` | project | done | main role build 설정 |
| `Server/Main/Program.cs` | `Server/Main/main.ts` | server-entry | done | main role 실행 진입점 |
| `Server/Main/RegistrationCodecServerHostFactory.cs` | `Server/Main/main-host.ts` | server-role | done | channel server/client와 handler 등록 |
| `Server/Main/ServerOptions.cs` | `Server/Main/Configuration/server-options.ts` | configuration | done | CLI option parsing |
| `Server/Main/Handlers/RegistrationHandlers.cs` | `Server/Main/Handlers/registration-handlers.ts` | handlers | done | 등록 variant handler |
| `Server/Main/Handlers/CodecHandlers.cs` | `Server/Main/Handlers/codec-handlers.ts` | handlers | done | JSON/Protobuf/MessagePack codec handler |
| `Server/Main/Handlers/DiEchoRequestHandler.cs` | `Server/Main/Handlers/di-echo-handler.ts` | handlers | done | singleton/scoped id evidence는 구현. dispose counter는 Node/Nest public context lifecycle 표면 밖이다. |
| `Server/Main/DispatchFilters.cs` | `Server/Main/Handlers/dispatch-filters.ts` | handlers | done | filter before/after evidence |
| `Server/Main/Infrastructure/EvidenceStore.cs` | `Server/Main/Infrastructure/evidence-store.ts` | infrastructure | done | evidence 저장과 wait |
| `Server/Main/Infrastructure/Probes.cs` | `Server/Main/Infrastructure/lifecycle-probes.ts` | infrastructure | done | singleton/scoped probe id는 구현. request scope dispose counter는 Node/Nest public context lifecycle 표면 밖이다. |
| `Server/Main/Endpoints/OperationalEndpoints.cs` | `Server/Main/Endpoints/operational-endpoints.ts` | endpoints | done | health/evidence/shutdown |
| `Server/Main/Endpoints/RegistrationScenarioEndpoints.cs` | `Server/Main/Endpoints/main-endpoints.ts` | endpoints | done | registration scenario와 Main-host codec trigger endpoints |
| `Server/InvalidDuplicate/RegistrationCodec.InvalidDuplicate.csproj` | `Server/InvalidDuplicate/package.json`, `Server/InvalidDuplicate/tsconfig.json` | project | done | duplicate startup failure role build 설정 |
| `Server/InvalidDuplicate/Program.cs` | `Server/InvalidDuplicate/main.ts` | server-entry | done | invalid duplicate role 실행 진입점 |
| `Server/InvalidDuplicate/RegistrationCodecServerHostFactory.cs` | `Server/InvalidDuplicate/invalid-duplicate-host-factory.ts` | negative-server | done | duplicate request handler registration을 독립 role에서 구성 |
| `Server/InvalidDuplicate/ServerOptions.cs` | `Server/InvalidDuplicate/Configuration/invalid-duplicate-options.ts` | configuration | done | invalid duplicate CLI option parsing |
| `Server/InvalidDuplicate/Handlers/RegistrationHandlers.cs` | `Server/InvalidDuplicate/Handlers/duplicate-handlers.ts` | handlers | done | duplicate registration에 필요한 handler |
| `Server/InvalidDuplicate/DispatchFilters.cs` | `feature-map.ko.md` | handlers | not applicable | RC-A6는 duplicate registration startup failure만 검증한다. filter ordering은 main role의 RC-A5에서 검증한다. |
| `Server/InvalidDuplicate/Handlers/CodecHandlers.cs` | `feature-map.ko.md` | handlers | not applicable | RC-A6 startup failure에는 codec handler 실행이 필요하지 않다. |
| `Server/InvalidDuplicate/Handlers/DiEchoRequestHandler.cs` | `feature-map.ko.md` | handlers | not applicable | InvalidDuplicate는 startup failure role이므로 DI lifecycle handler를 실행하지 않는다. RC-A4는 Main role에서 검증한다. |
| `Server/InvalidDuplicate/Infrastructure/EvidenceStore.cs` | `feature-map.ko.md` | infrastructure | not applicable | InvalidDuplicate는 HTTP/evidence endpoint를 열기 전에 startup failure를 검증한다. |
| `Server/InvalidDuplicate/Infrastructure/Probes.cs` | `feature-map.ko.md` | infrastructure | not applicable | InvalidDuplicate는 startup failure role이므로 DI lifecycle probe를 실행하지 않는다. RC-A4는 Main role에서 검증한다. |
| `Server/InvalidDuplicate/OperationalEndpoints.cs` | `feature-map.ko.md` | endpoints | not applicable | InvalidDuplicate role은 readiness 대상이 아니라 실패해야 하는 프로세스다. |
| `Server/CodecRequester/RegistrationCodec.CodecRequester.csproj` | `Server/CodecRequester/package.json`, `Server/CodecRequester/tsconfig.json` | project | done | codec requester build 설정 |
| `Server/CodecRequester/Program.cs` | `Server/CodecRequester/main.ts` | server-entry | done | codec requester 실행 진입점 |
| `Server/CodecRequester/CodecRequesterHostFactory.cs` | `Server/CodecRequester/codec-requester-host-factory.ts` | server-role | done | Protobuf requester client 구성 |
| `Server/CodecRequester/CodecRequesterOptions.cs` | `Server/CodecRequester/Configuration/codec-requester-options.ts` | configuration | done | requester CLI option parsing |
| `Server/JsonOnlyPeer/RegistrationCodec.JsonOnlyPeer.csproj` | `Server/JsonOnlyPeer/package.json`, `Server/JsonOnlyPeer/tsconfig.json` | project | done | JSON-only peer build 설정 |
| `Server/JsonOnlyPeer/Program.cs` | `Server/JsonOnlyPeer/main.ts` | server-entry | done | JSON-only peer 실행 진입점 |
| `Server/JsonOnlyPeer/RegistrationCodecServerHostFactory.cs` | `Server/JsonOnlyPeer/json-only-host-factory.ts` | server-role | done | JSON-only server/client 구성 |
| `Server/JsonOnlyPeer/ServerOptions.cs` | `Server/JsonOnlyPeer/Configuration/json-only-options.ts` | configuration | done | JSON-only CLI option parsing |
| `Server/JsonOnlyPeer/DispatchFilters.cs` | `feature-map.ko.md` | handlers | not applicable | JsonOnlyPeer는 RC-B5 mismatch/recovery만 담당한다. filter ordering은 main role의 RC-A5에서 검증한다. |
| `Server/JsonOnlyPeer/Handlers/CodecHandlers.cs` | `Server/JsonOnlyPeer/Handlers/json-only-handlers.ts` | handlers | done | JSON-only request handler와 mismatch rejection |
| `Server/JsonOnlyPeer/Handlers/RegistrationHandlers.cs` | `feature-map.ko.md` | handlers | not applicable | JsonOnlyPeer는 JSON-only codec mismatch peer라 registration variant scenario를 실행하지 않는다. |
| `Server/JsonOnlyPeer/Handlers/DiEchoRequestHandler.cs` | `feature-map.ko.md` | handlers | not applicable | JsonOnlyPeer는 codec mismatch/recovery role이므로 DI lifecycle handler를 실행하지 않는다. RC-A4는 Main role에서 검증한다. |
| `Server/JsonOnlyPeer/Infrastructure/EvidenceStore.cs` | `Server/JsonOnlyPeer/Infrastructure/evidence-store.ts` | infrastructure | done | JSON-only evidence store |
| `Server/JsonOnlyPeer/Infrastructure/Probes.cs` | `feature-map.ko.md` | infrastructure | not applicable | JsonOnlyPeer는 codec mismatch/recovery role이므로 DI lifecycle probe를 실행하지 않는다. RC-A4는 Main role에서 검증한다. |
| `Server/JsonOnlyPeer/OperationalEndpoints.cs` | `Server/JsonOnlyPeer/Endpoints/operational-endpoints.ts` | endpoints | done | health/evidence/shutdown endpoint |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | 구현 상태 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | build, process startup, scenario execution |

## Public Contract 확인 결과

`RC-A4`는 Node Nest adapter public handler dispatch path가 request마다 새 async scope를 만들도록 진전됐다.
`logs/20260630-102915-3514768`에서 singleton id는 같고 scoped id는 `scoped-1`, `scoped-2`로 갈라진다.
현재 Nest public `ModuleRef` 경로에는 `ContextIdFactory.create()`로 만든 request context를 dispatch 뒤
명시적으로 해제하는 API가 없다. 현재 설치된 Nest public export도 `ContextIdFactory`, `ModuleRef.resolve(...)`,
`registerRequestByContextId(...)`까지만 제공한다. 따라서 Node 완료 조건은 public dispatch path의 scoped id
분리와 singleton 안정성이다. 내부 wrapper 저장소를 직접 지우는 방식이나 테스트 전용 adapter는 완료 근거로
쓰지 않는다.
`RC-B2`는 Node Protobuf extension의 public content-type을 공통 문서가 요구하는 `application/x-protobuf`로
맞추고 MessagePack variant도 같은 scenario에서 검증한다. 검증은 Main role의 전역 codec registry에서
수행하며, 별도 codec 전용 peer role은 두지 않는다.

Node framework serializer는 `canSerialize` public predicate를 선택적으로 제공할 수 있다. `RC-B2`와
`RC-B4`는 Main host에 Protobuf와 MessagePack serializer를 함께 등록하고, 각 serializer가 맡을 payload
class를 predicate로 선언한 뒤 JSON fallback과 함께 검증한다. 이 경로는 raw frame 생성이나 수동 serialize
helper를 쓰지 않는다.

## 후속 계약 판정

| Scenario | 판정 | 다음 작업 |
|----------|------|-----------|
| `RC-A4` | 구현 | per-dispatch scope evidence는 public dispatch 경로로 통과했다. Node/Nest public API에는 dispatch context 해제 표면이 없으므로 내부 lifecycle probe나 Nest 내부 wrapper 삭제 없이 scoped id 분리와 singleton 안정성을 완료 조건으로 둔다. |
