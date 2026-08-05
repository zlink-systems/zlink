# C++ RegistrationCodec .NET 기준 포팅 inventory

이 문서는 `framework/languages/dotnet/e2e/RegistrationCodec`의 파일을 기준으로 C++
`RegistrationCodec` E2E의 대응 파일을 기록한다. 현재 C++ 구현은 client scenario/support와
server configuration/handler/endpoint/support를 분리했고, invalid role, JSON-only peer role,
codec requester role도 별도
executable로 분리한다.

`.NET` 전용 attribute syntax와 assembly scan은 C++가 그대로 제공하지 않는다. 대신 공통 spec의
annotation 의미인 packet kind/name override를 C++ handler 타입 metadata와 DTO `packet_name`으로
표현한다. Protobuf/MessagePack은 C++ public codec extension을 사용해 실제 E2E로 검증한다.

## 기준

- 공통 문서: `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md`
- .NET 기준 구현: `framework/languages/dotnet/e2e/RegistrationCodec`
- C++ 대상: `framework/languages/cpp/e2e/RegistrationCodec`

## 파일 매핑

| .NET 기준 파일 | C++ 대응 파일 | 분류 | 상태 | 비고 |
|----------------|---------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | 실행 로그를 제외한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | C++ public API로 검증한 항목을 기록한다. |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | server/client, invalid role, JSON-only peer role, codec requester role을 각각 별도 executable로 실행한다. |
| `Shared/Messages.cs` | `Shared/registration_codec_contracts.hpp` | shared | done | JSON/Protobuf/MessagePack/custom serializer용 DTO와 evidence DTO가 대응한다. |
| `Shared/RegistrationCodec.Shared.csproj` | `Shared/registration_codec_contracts.hpp` | build | not-needed | C++ shared contract는 header로 포함된다. |
| `Client/Program.cs` | `Client/main.cpp` | client-entry | done | .NET 기준처럼 HTTP client로 server endpoint를 호출해 scenario를 dispatch한다. framework runtime은 client가 아니라 server/codec requester role이 소유한다. scenario 본문과 support helper는 별도 header로 분리했다. |
| `Client/RegistrationCodec.Client.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registration_codec_client` target이 대응한다. |
| `Client/Support/ClientOptions.cs` | `Client/Support/client_support.hpp`; `run_e2e.sh` | client-support | done | env parsing과 endpoint orchestration이 대응한다. |
| `Client/Support/CodecScenarioResult.cs` | not-needed | client-support | not-needed | C++ client는 typed reply를 직접 검사한다. |
| `Client/Support/EvidenceText.cs` | `Client/Support/client_support.hpp`; `Server/Infrastructure/scenario_state.hpp` | client-support | done | C++는 typed reply와 server evidence DTO를 직접 사용하고, HTTP evidence 조회 helper는 client support에 둔다. |
| `Client/Support/ProcessSupport.cs` | `run_e2e.sh` | runner-support | done | invalid startup process 실행은 shell runner가 담당한다. |
| `Client/Support/ScenarioAssert.cs` | `Client/Support/client_support.hpp`; `run_e2e.sh` | client-support | done | C++ `ensure`와 shell failure checks가 대응한다. |
| `Client/Scenarios/AutoRegistrationScenario.cs` | `Client/Scenarios/auto_registration_scenario.hpp` | scenario | done | `RC-A1` request/send handler group 등록을 검증한다. |
| `Client/Scenarios/AttributeRegistrationScenario.cs` | `Client/Scenarios/attribute_registration_scenario.hpp` | scenario | done | `RC-A2`는 C++에서 handler 타입의 `request_type`/`message_type`, `topic_name`, DTO의 `packet_name` metadata로 annotation 의미를 검증한다. |
| `Client/Scenarios/ManualRegistrationScenario.cs` | `Client/Scenarios/manual_registration_scenario.hpp` | scenario | done | `RC-A3` 수동 channel handler 등록을 HTTP endpoint 경유로 검증한다. |
| `Client/Scenarios/RcA4DiLifecycleScenario.cs` | `Client/Scenarios/rc_a4_di_lifecycle_scenario.hpp` | scenario | done | `RC-A4` scoped/singleton lifecycle을 검증한다. |
| `Client/Scenarios/RcA5FilterOrderingScenario.cs` | `Client/Scenarios/rc_a5_filter_ordering_scenario.hpp` | scenario | done | `RC-A5` filter before/after 순서를 검증한다. |
| `Client/Scenarios/InvalidRegistrationScenario.cs` | `run_e2e.sh`; `Server/Support/server_host.hpp` | scenario | done | `RC-A6` duplicate/wrong-group/unsupported-channel startup failure를 검증한다. |
| `Client/Scenarios/RcB1JsonCodecScenario.cs` | `Client/Scenarios/rc_b1_json_codec_scenario.hpp`; `Server/Handlers/codec_handlers.hpp` | scenario | done | `RC-B1` JSON round-trip을 검증한다. |
| `Client/Scenarios/RcB2ProtobufCodecScenario.cs` | `Client/Scenarios/rc_b2_protobuf_codec_scenario.hpp`; `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | scenario | done | C++ Protobuf codec extension request/send와 content-type evidence가 대응한다. |
| `Client/Scenarios/RcB3MessagePackCodecScenario.cs` | `Client/Scenarios/rc_b3_messagepack_codec_scenario.hpp`; `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | scenario | done | C++ MessagePack codec extension request/send와 content-type evidence가 대응한다. |
| `Client/Scenarios/RcB4CodecCoexistenceScenario.cs` | `Client/Scenarios/rc_b4_codec_coexistence_scenario.hpp`; `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | scenario | done | 별도 serializer가 없는 DTO의 기본 JSON 경로와 Protobuf, MessagePack, custom serializer 공존을 검증한다. |
| `Client/Scenarios/CodecMismatchScenario.cs` | `Client/Scenarios/codec_mismatch_scenario.hpp`; `Server/CodecRequester/main.cpp`; `Server/Support/server_host.hpp`; `run_e2e.sh` | scenario | done | HTTP-only client가 codec requester endpoint를 호출하고, requester가 JSON-only peer의 공개 `payload_decode_failed`와 JSON recovery를 검증한다. |
| `Server/Main/Program.cs` | `Server/main.cpp` | server-entry | done | 정상 server entry가 대응한다. |
| `Server/Main/RegistrationCodecServerHostFactory.cs` | `Server/Support/server_host.hpp` | server-role | done | 정상 framework 구성이 대응한다. |
| `Server/Main/ServerOptions.cs` | `Server/Configuration/server_options.hpp`; `run_e2e.sh` | configuration | done | env 기반 endpoint/log option이 대응한다. |
| `Server/Main/DispatchFilters.cs` | `Server/Handlers/filter_order_handlers.hpp` | filter | done | first/second filter가 대응한다. |
| `Server/Main/Handlers/RegistrationHandlers.cs` | `Server/Handlers/registration_handlers.hpp` | handler | done | auto, attribute 의미, manual 성격의 handler가 대응한다. |
| `Server/Main/Handlers/DiEchoRequestHandler.cs` | `Server/Handlers/di_lifecycle_handlers.hpp` | handler | done | scoped/singleton lifecycle handler가 대응한다. |
| `Server/Main/Handlers/CodecHandlers.cs` | `Server/Handlers/codec_handlers.hpp` | handler | done | JSON/Protobuf/MessagePack/custom/mismatch handler가 대응한다. |
| `Server/Main/Infrastructure/EvidenceStore.cs` | `Server/Infrastructure/scenario_state.hpp` | infrastructure | done | scenario state/evidence snapshot이 대응한다. |
| `Server/Main/Infrastructure/Probes.cs` | `Server/Support/server_host.hpp`; `Client/Scenarios/`; `run_e2e.sh` | infrastructure | done | C++는 typed reply와 startup failure로 probe를 직접 검증한다. 이 차이는 검증 배치 차이이며 RC scenario public 동작 차이가 아니다. |
| `Server/Main/Endpoints/OperationalEndpoints.cs` | `Server/Endpoints/operational_endpoints.hpp` | endpoint | done | `/health`와 `/evidence`가 대응한다. |
| `Server/Main/Endpoints/RegistrationScenarioEndpoints.cs` | `Server/Endpoints/operational_endpoints.hpp` | endpoint | done | C++도 .NET 기준처럼 HTTP scenario endpoint가 framework channel public API를 호출한다. |
| `Server/Main/RegistrationCodec.Server.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registration_codec_server` target이 대응한다. |
| `Server/InvalidDuplicate/Program.cs` | `Server/InvalidDuplicate/main.cpp` | server-entry | done | invalid role 전용 executable entry가 대응한다. |
| `Server/InvalidDuplicate/RegistrationCodecServerHostFactory.cs` | `Server/Support/server_host.hpp` | invalid-role | done | invalid mode별 startup failure 구성을 공통 host support에서 선택한다. |
| `Server/InvalidDuplicate/ServerOptions.cs` | `Server/Configuration/server_options.hpp`; `run_e2e.sh` | configuration | done | invalid mode와 endpoint option을 env로 전달한다. |
| `Server/InvalidDuplicate/DispatchFilters.cs` | `Server/Handlers/filter_order_handlers.hpp` | filter | done | invalid role은 같은 filter type을 재사용한다. |
| `Server/InvalidDuplicate/Handlers/RegistrationHandlers.cs` | `Server/Handlers/registration_handlers.hpp`; `Server/Support/server_host.hpp` | handler | done | duplicate/wrong-group/unsupported-channel startup failure를 만들 때 같은 handler type을 사용한다. |
| `Server/InvalidDuplicate/Handlers/CodecHandlers.cs` | `Server/Handlers/codec_handlers.hpp` | handler | done | invalid role과 정상 role이 같은 codec handler type을 공유한다. |
| `Server/InvalidDuplicate/Handlers/DiEchoRequestHandler.cs` | `Server/Handlers/di_lifecycle_handlers.hpp` | handler | done | invalid role과 정상 role이 같은 DI handler type을 공유한다. |
| `Server/InvalidDuplicate/Infrastructure/EvidenceStore.cs` | `Server/Infrastructure/scenario_state.hpp` | infrastructure | done | invalid role은 정상 role과 같은 state type을 공유하지만 startup failure에서는 evidence를 판정에 쓰지 않는다. |
| `Server/InvalidDuplicate/Infrastructure/Probes.cs` | `Server/Support/server_host.hpp`; `run_e2e.sh` | infrastructure | done | startup failure probe는 runner의 invalid executable exit code와 stderr 확인으로 수행한다. |
| `Server/InvalidDuplicate/OperationalEndpoints.cs` | `Server/Endpoints/operational_endpoints.hpp` | endpoint | done | invalid role이 정상 기동하지 않아 endpoint 판정에는 쓰이지 않지만 같은 endpoint handler가 대응한다. |
| `Server/InvalidDuplicate/RegistrationCodec.InvalidDuplicate.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registration_codec_invalid_duplicate` target이 대응한다. |
| `Server/JsonOnlyPeer/Program.cs` | `Server/JsonOnlyPeer/main.cpp` | server-entry | done | JSON-only peer 전용 executable entry가 대응한다. |
| `Server/JsonOnlyPeer/RegistrationCodecServerHostFactory.cs` | `Server/Support/server_host.hpp` | codec-role | done | `json-only-peer` mode는 binary codec을 등록하지 않는 peer로 실행된다. |
| `Server/JsonOnlyPeer/ServerOptions.cs` | `Server/Configuration/server_options.hpp`; `run_e2e.sh` | configuration | done | JSON-only peer endpoint/log option을 env로 전달한다. |
| `Server/JsonOnlyPeer/DispatchFilters.cs` | `Server/Handlers/filter_order_handlers.hpp` | filter | done | JSON-only peer는 정상 role과 같은 filter type을 공유한다. |
| `Server/JsonOnlyPeer/Handlers/RegistrationHandlers.cs` | `Server/Handlers/registration_handlers.hpp` | handler | done | JSON-only peer는 normal JSON recovery 검증에 필요한 registration handler를 공유한다. |
| `Server/JsonOnlyPeer/Handlers/CodecHandlers.cs` | `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | handler | done | mismatch handler와 JSON codec handler가 JSON-only peer에서 실행된다. |
| `Server/JsonOnlyPeer/Handlers/DiEchoRequestHandler.cs` | `Server/Handlers/di_lifecycle_handlers.hpp` | handler | done | JSON-only peer와 정상 role이 같은 DI handler type을 공유한다. |
| `Server/JsonOnlyPeer/Infrastructure/EvidenceStore.cs` | `Server/Infrastructure/scenario_state.hpp` | infrastructure | done | mismatch content-type evidence와 JSON recovery evidence가 대응한다. |
| `Server/JsonOnlyPeer/Infrastructure/Probes.cs` | `Client/Scenarios/codec_mismatch_scenario.hpp`; `Server/CodecRequester/main.cpp`; `run_e2e.sh` | infrastructure | done | codec requester가 JSON-only peer에 request를 보내고, HTTP-only client는 requester endpoint 결과를 검증한다. |
| `Server/JsonOnlyPeer/OperationalEndpoints.cs` | `Server/Endpoints/operational_endpoints.hpp` | endpoint | done | `/health`와 `/evidence` endpoint가 대응한다. |
| `Server/JsonOnlyPeer/RegistrationCodec.JsonOnlyPeer.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registration_codec_json_only_peer` target이 대응한다. |
| `Server/CodecRequester/Program.cs` | `Server/CodecRequester/main.cpp` | codec-role | done | Protobuf/MessagePack codec을 등록한 requester server role이 JSON-only peer channel endpoint로 request를 보낸다. |
| `Server/CodecRequester/CodecRequesterHostFactory.cs` | `Server/CodecRequester/main.cpp`; `Server/Endpoints/operational_endpoints.hpp` | codec-role | done | requester framework setup과 `/codec/mismatch` HTTP endpoint가 대응한다. |
| `Server/CodecRequester/CodecRequesterOptions.cs` | `Server/CodecRequester/main.cpp`; `run_e2e.sh` | configuration | done | codec requester channel endpoint, HTTP endpoint, log option을 env로 전달한다. |
| `Server/CodecRequester/RegistrationCodec.CodecRequester.csproj` | `framework/languages/cpp/CMakeLists.txt` | build | done | `zlink_cpp_e2e_registration_codec_codec_requester` target이 대응한다. |

## 공통 scenario ID 대응

| Scenario ID | C++ 대응 파일 | 상태 | 비고 |
|-------------|---------------|------|------|
| `RC-A1` | `Client/Scenarios/auto_registration_scenario.hpp`; `Server/Handlers/registration_handlers.hpp` | done | handler group 기반 request/send 등록을 검증한다. |
| `RC-A2` | `Client/Scenarios/attribute_registration_scenario.hpp`; `Server/Handlers/registration_handlers.hpp`; `Server/Support/server_host.hpp` | done | C++ 타입 metadata 기반 request/send handler 등록을 검증한다. |
| `RC-A3` | `Client/Scenarios/manual_registration_scenario.hpp`; `Server/Handlers/registration_handlers.hpp`; `Server/Support/server_host.hpp` | done | 수동 channel request/send handler 등록을 검증한다. |
| `RC-A4` | `Client/Scenarios/rc_a4_di_lifecycle_scenario.hpp`; `Server/Handlers/di_lifecycle_handlers.hpp` | done | scoped dependency 교체, singleton 유지, scoped dispose count를 검증한다. |
| `RC-A5` | `Client/Scenarios/rc_a5_filter_ordering_scenario.hpp`; `Server/Handlers/filter_order_handlers.hpp` | done | filter ordering을 검증한다. |
| `RC-A6` | `run_e2e.sh`; `Server/Support/server_host.hpp` | done | invalid startup failure를 검증한다. |
| `RC-B1` | `Client/Scenarios/rc_b1_json_codec_scenario.hpp`; `Server/Handlers/codec_handlers.hpp` | done | JSON round-trip을 검증한다. |
| `RC-B2` | `Client/Scenarios/rc_b2_protobuf_codec_scenario.hpp`; `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | done | Protobuf codec extension request/send와 content-type evidence를 검증한다. |
| `RC-B3` | `Client/Scenarios/rc_b3_messagepack_codec_scenario.hpp`; `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | done | MessagePack codec extension request/send와 content-type evidence를 검증한다. |
| `RC-B4` | `Client/Scenarios/rc_b4_codec_coexistence_scenario.hpp`; `Server/Handlers/codec_handlers.hpp`; `Server/Support/server_host.hpp` | done | 별도 serializer가 없는 DTO의 기본 JSON 경로와 Protobuf/MessagePack/custom serializer 공존을 검증한다. |
| `RC-B5` | `Client/Scenarios/codec_mismatch_scenario.hpp`; `Server/CodecRequester/main.cpp`; `Server/Support/server_host.hpp`; `run_e2e.sh` | done | codec requester가 JSON-only peer의 공개 `payload_decode_failed`와 JSON recovery를 검증한다. |

## 검증

- 2026-07-08: `timeout 420s nice -n 10 framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260708-124643-1329498`
  - 의미: 정상 server, JSON-only peer, codec requester, invalid role, HTTP-only client runner가
    모두 통과했다. 출력은 `scenario RC-A1 passed`부터 `scenario RC-B5 passed`, RC-A6 invalid
    startup checks, `registration-codec e2e result=passed`를 포함한다.
- 2026-07-03: `ZLINK_CPP_E2E_BUILD_DIR=/home/hep7/project/kairos/zlink/framework/languages/cpp/build-redis-vcpkg timeout 420s framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260703-200739-24698`
  - 의미: location store 포팅 이후 현재 트리에서 정상 server, JSON-only peer, codec requester,
    invalid role, HTTP-only client runner가 모두 통과했다. RC-A1, RC-A3, RC-A4, RC-A5, RC-A6,
    RC-B1, RC-B2, RC-B3, RC-B4, RC-B5를 검증했고, RC-A2는 그 당시 annotation/decorator 의미의
    C++ 대응을 아직 검증하지 않았다. Config-4는 수동 endpoint 연결 config라 Redis location store
    등록이 필요 없다.
- 2026-07-07: `timeout 420s framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260707-134236-1993396`
  - 의미: `RC-A2`를 C++ 타입 metadata 기반 등록 scenario로 추가한 뒤 정상 server, JSON-only peer,
    codec requester, invalid role, HTTP-only client runner가 모두 통과했다. 출력은 `scenario RC-A2
    passed`와 `registration-codec e2e result=passed`를 포함한다.
- 2026-06-30: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_registration_codec_server zlink_cpp_e2e_registration_codec_invalid_duplicate zlink_cpp_e2e_registration_codec_json_only_peer zlink_cpp_e2e_registration_codec_client`
  - 결과: 통과
- 2026-06-30: `./framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-082445-3267605`
  - 의미: 정상 server, JSON-only peer executable, invalid role executable, client가 같은 gate에서 검증된다.
- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-163722-417936`
  - 의미: 최신 checkout에서 RC-A1, RC-A3, RC-A4, RC-A5, RC-A6, RC-B1, RC-B2, RC-B3,
    RC-B4, RC-B5가 통과했다. 당시에는 RC-A2를 C++ public attribute discovery 계약 미정 항목으로
    기록했지만, 현재 완료 근거는 위의 2026-07-07 RC-A2 검증 기록을 사용한다.
- 2026-06-30: `timeout 420s framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260630-182337-725659`
  - 의미: 현재 트리에서 정상 server, JSON-only peer, invalid role, client runner가 모두 통과했다.
    RC-A1, RC-A3, RC-A4, RC-A5, RC-A6, RC-B1, RC-B2, RC-B3, RC-B4, RC-B5를 검증했다.
    당시에는 RC-A2를 C++ public attribute discovery 계약 미정 항목으로 기록했지만, 현재 완료
    근거는 위의 2026-07-07 RC-A2 검증 기록을 사용한다.
- 2026-07-01: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_registration_codec_client -j 4`
  - 결과: 통과
  - 의미: public send call surface가 fire-and-forget `submit()` 중심으로 정리된 현재 C++ framework에
    맞춰 RC-A1/RC-B1/RC-B2/RC-B3 client send 호출을 `.submit()`으로 수정한 뒤 client target이 빌드된다.
- 2026-07-01: `timeout 420s framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260701-162045-82879`
  - 의미: 현재 트리에서 정상 server, JSON-only peer, invalid role, client runner가 모두 통과했다.
    RC-A1, RC-A3, RC-A4, RC-A5, RC-A6, RC-B1, RC-B2, RC-B3, RC-B4, RC-B5를 검증했고,
    RC-A2는 그 당시 annotation/decorator 의미의 C++ 대응을 아직 검증하지 않았다. 최종
    `registration-codec e2e result=passed` marker는 RC-A6 invalid startup checks 뒤에 runner가
    한 번만 출력한다.
- 2026-07-02: `cmake --build framework/languages/cpp/build --target zlink_cpp_e2e_registration_codec_server zlink_cpp_e2e_registration_codec_invalid_duplicate zlink_cpp_e2e_registration_codec_json_only_peer zlink_cpp_e2e_registration_codec_codec_requester zlink_cpp_e2e_registration_codec_client`
  - 결과: 통과
  - 의미: HTTP-only client, 정상 server, JSON-only peer, codec requester, invalid role target이 빌드된다.
- 2026-07-02: `timeout 420s framework/languages/cpp/e2e/RegistrationCodec/run_e2e.sh`
  - 결과: 통과
  - 로그: `logs/20260702-070838-14667`
  - 의미: 현재 트리에서 HTTP-only client가 server/codec requester HTTP endpoint만 호출한다. RC-A1, RC-A3,
    RC-A4, RC-A5, RC-A6, RC-B1, RC-B2, RC-B3, RC-B4, RC-B5를 검증했고, RC-A2는
    그 당시 annotation/decorator 의미의 C++ 대응을 아직 검증하지 않았다.
