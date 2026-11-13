# #3867 C++ request 경로의 제품 변경

완료: 제품 hunk 검토, main encoder에서 생성한 영구 fixture, 전체 빌드 620/620 단계와 테스트 수정 후 증분 빌드 2/2 단계, unit/contract **298/298**, wire 12개 case와 고의 변조 거부, 최종 C++ format 검사가 통과했다. 남은 테스트 실패는 없다. 새 파일 3개를 포함한 27개 파일의 최종 변경은 `final.diff`에 저장했다.

작업 위치는 `/root/worktree/zlink-3867e`, 브랜치는 `perf/3867-cpp-request-encode-once`다. 시작 때 작업 트리는 깨끗했다. `prototype.diff`를 `git apply --exclude='framework/bench/*'`로 적용했다. 제품 변경은 C++ Framework와 그 테스트에 한정했다. Bench, run_local.sh, Core, binding, spec 변경은 없다. Commit·push·브랜치 변경·PR 생성은 하지 않았다.

## 단계별 변경

아래 코드 경로는 `framework/languages/cpp/` 기준이다.

| 단계 | 변경과 근거 위치 |
|---|---|
| 1. 준비된 transport entry | `framework/src/runtime/channels/channel_runtime.hpp:194`에서 route 이름 set과 MeshNode map을 `nodes` entry map으로 합쳤다. Entry가 실제 transport, channel fragment, request encoder를 보유한다. `framework/src/runtime/channels/channel_runtime.cpp:1749`에서 request는 atomic snapshot 한 번과 map 조회 한 번으로 entry를 읽는다. `framework/src/runtime/channels/channel_runtime_manager.cpp:184`, `:204`에서 route 객체를 함께 게시한다. Route 우선순위와 snapshot 수명을 유지한다. 남은 send lookup도 `framework/src/runtime/channels/channel_runtime.cpp:118`에서 성공값만 감싸던 `result_t`를 제거했다. |
| 2. Bind/type plan | `framework/include/zlink/framework/contracts/codecs/serializer.hpp:237`, `:460`에서 기존 serializer state가 type fragment와 content type을 소유하고, aliasing shared_ptr로 수명을 전달한다. 기존 1024개 bounded serializer cache를 사용한다. `framework/src/runtime/channels/channel_runtime.cpp:768`에서 bind entry가 channel fragment와 encoder 함수를 준비한다. `framework/include/zlink/framework/contracts/channels/channel.hpp:917`과 `framework/src/runtime/channels/channel_runtime.cpp:1879`가 그 plan을 전달한다. 해당 request 경로의 TLS matching과 정적 fragment 재선택을 제거했다. |
| 3. 최종 service payload에 header 작성 | `framework/src/runtime/messaging/client_call_codec.cpp:83`, `framework/src/runtime/messaging/envelope_codec.cpp:529`에서 E1에 유효성·크기·flow snapshot을 확정하고 body와 plan을 소유한 encoder를 만든다. `framework/src/runtime/protocol/service_wire_codec.cpp:3262`에서 기존 공통 wire writer가 최종 native service frame에 JSON header를 한 번 쓴다. 별도 JSON header native frame과 이를 service payload로 복사하는 단계를 제거했다. `framework/src/runtime/messaging/envelope_codec.cpp:37`과 `framework/src/runtime/protocol/service_wire_codec.cpp:2979`은 기존 parts/bytes 표현과 새 encoder 표현 사이의 소유권 이전을 담당한다. |
| 4. 최종 admitted source 연결 | `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1357`, `:1530`에서 기존 등록·시작 코드를 공유하고 최종 admitted source를 그대로 binding adapter에 전달한다. Request 경로의 raw bool task → host submit_result projection → mesh admission observer를 제거했다. Target 거부 분류는 `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1346`으로 이동하고 legacy host도 이를 사용한다(`framework/src/runtime/stateful/public_host_runtime.cpp:3405`). 오류 변환은 `framework/src/runtime/messaging/submit_result_mapper.hpp:67`을 공유한다. |
| 5. Host forwarding 제거 | `framework/src/runtime/mesh/mesh_node_runtime.cpp:4450`에서 기존 raw owner에 직접 제출한다. `framework/src/runtime/host/app.cpp:2632`와 세 테스트 adapter는 `message_parts_t` 소유 표현을 그대로 넘긴다. 다른 경로가 쓰는 host/raw wrapper는 유지했다. 특히 host의 기존 vector signature와 `message_parts_t`의 explicit vector 생성자를 유지했다. |

공통 정적 JSON fragment 작성은 `framework/src/runtime/messaging/envelope_codec.cpp:351`의 작성 함수가 소유한다. 기존 TLS 경로와 bind/type plan이 같은 함수를 사용하므로 canonical field 순서·escaping 규칙을 복제하지 않는다.

## 제품 검토와 삭제한 메커니즘

- 두 종류의 node transport index, RouteChannel의 두 번째 map 조회, 이를 위한 lane task 왕복을 제거했다.
- 해당 request 경로에서 TLS header-plan matching과 정적 fragment 재선택을 제거했다. 다른 경로가 쓰는 기존 TLS encoder는 유지했다.
- JSON header의 별도 native message와 service frame으로의 header copy, hot path의 중간 part vector를 제거했다.
- 해당 request 경로에서 bool admission task, submit_result projection, mesh admission observer와 public-host forwarding 호출을 제거했다. 다른 사용자가 있는 legacy wrapper는 유지했다.
- 삭제된 observer 지점을 보존하려던 prototype의 test 전용 `submission_error`·catch/rethrow와 두 fault hook을 제거했다. `framework/include/zlink/framework/detail/test_creation_probe.hpp`의 쓰이지 않는 extern도 삭제했다.
- Native-count 테스트의 `J1672_BEFORE` 비교용 조건 네 곳을 제거했다. 현재 dispatch API와 현재 기대값만 남겼다.
- Prototype이 추가했던 host wrapper signature 변경과 implicit vector 변환을 취소했다.
- 공통 wire loop는 기존 range-for를 유지했다. 두 body 표현이 같은 크기·한도·flow 판정을 사용한다.

추가 검토에서 prototype의 owned encoder가 `application_payload_t`의 top-level flow pair를 무시하는 문제를 찾았다. `runtime/protocol/application_envelope_encoder.hpp`에 공통 callable 타입을 두고 owner의 flow pair를 기존 공통 wire writer에 전달했다. Presence/UUIDv7 판정은 기존 writer 한 곳이 계속 소유한다. 별도 validation 분기나 cache를 추가하지 않았다.

Malformed static UTF-8의 준비 오류를 보존하는 `exception_ptr`는 유지했다. 이는 bind/type 준비에서 발생한 오류를 기존과 같이 E1에 알리기 위한 값이다. Observer fault injection을 보존하는 예외 캡처와는 용도가 다르다. 새 테스트에서 channel/name/content type 세 필드의 E1 거부를 검사한다.

새 production 제어 flag·counter·queue·cache·lock·poller는 0개다. Plan은 기존 serializer state와 published index 안에 둔다. Content type은 기존 state에서 plan으로 이동했고, owned encoder가 header native frame과 중간 part vector를 대체한다. 새 callable 타입은 두 소유 표현이 공유하는 타입 정의이며 별도 실행 단계가 아니다.

## 결정 소유와 규칙 수

숫자는 같은 결정을 내리는 위치의 수다.

| 결정 | 수정 전 | 수정 후 | 소유자 |
|---|---:|---:|---|
| C1 terminal 경쟁 | 1 | 1 | 기존 operation registry의 atomic take |
| Deadline 만료 | 1 | 1 | 기존 operation entry와 C2 deadline index |
| Physical admission·WRITABLE 재제출 | 1 | 1 | 기존 binding/Core |
| 최종 typed delivery | 1 | 1 | 제출 전에 준비한 최종 typed receiver와 기존 dispatcher |
| Admission 성공을 최종 admitted에 반영 | 2 | 1 | Binding adapter만 반영; mesh observer 삭제 |
| Raw submit 거부의 NotFound/Unavailable 분류 | 1 | 1 | Host의 판정을 raw owner로 옮기고 공유 |
| 해당 request의 요청별 TLS plan matching | 1 | 0 | Bind/type 준비에서 이미 선택한 plan 사용 |

소유 계층: Framework codec은 serialization, registry는 C1/C2, dispatcher는 typed delivery, binding/Core는 physical admission·retry를 소유한다.

Spec 조항: `01-execution/01-submit-and-completion.ko.md` §9–11(최종 수신자, C1/C2, dispatcher), `01-execution/08-messaging-hot-path.ko.md` §3(E1–E5), `01-execution/05-payload-ownership-and-codec.ko.md`(한 번의 wire 작성과 codec 선택), `bindings/doc/spec/async-execution-model.ko.md` §5(admitted/reply 합류)를 따른다. 등록·만료·physical cleanup 알고리즘은 변경하지 않았다.

교차언어 대조: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5838`의 operation 제거와 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13`의 독립 turn, Java/Kotlin 공용 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:407`의 take와 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51`의 게시, Node `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:240`, `:434`의 deadline·take를 정적으로 확인했다. C++도 같은 소유 경계를 유지한다. 네 언어 실행 검증을 수행한 결과는 아니다.

변경 분류: **A — 기존 계약 적응**. 새 public API나 spec 동작을 추가하지 않았다.

## 영구 wire fixture와 테스트

Fixture는 `tests/fixtures/request-wire-main-213e579f52.json`이다. 제품 patch 적용 전에 현재 main의 library를 직접 빌드했고, 변경 전 `client_call_codec_t::encode_payload_parts → application_payload_t::from_parts → encode_application_payload_message`를 호출해 생성했다. 해당 시점에는 tracked 제품 소스 변경이 없었다.

- 생성 소스: `cpr2/capture_main_wire.cpp`
- 생성·링크 명령: `cpr2/build_capture.py`
- 원본 library hash: `cpr2/main-library.sha256`
- 변경 전 소스 hash: `cpr2/main-source-sha256.json`
- 생성 결과: `cpr2/capture-main.log`
- 기존 cproto main bytes와의 대조: `cpr2/fixture-provenance.log`, **12/12 동일**
- Fixture SHA-256: `a802d8d51bf1295acb97771a456e662f03fcc37b2e4c3255f49c57c6069d964e`

Body는 BenchPayload의 `bytes body = 1`을 표현하는 protobuf tag `0a`, length `8008`, `0xab` 1024개다. Bench/protobuf build 의존성 없이 같은 1027 byte serialized body를 사용한다. Correlation은 JSON `123abc`/service header `0x123abc`, deadline은 `2026-11-07T12:34:56Z`다.

새 `test_cpp_framework_request_wire_identity.cpp`에는 다음 검사가 있다. `CMakeLists.txt:3101`에서 unit tier로 등록했다.

| 검사 | 위치 | 목적 |
|---|---|---|
| Main encoder bytes 대조, 6개 parameter | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:85` | 기본, escaped channel, escaping·UTF-8 metadata, flow, topic/source, deadline 없음의 service header와 payload를 fixture와 대조한다. RouteChannel과 MeshNode plan을 모두 검사한다. |
| 한 byte 변조 | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:107` | 정상 frame은 fixture와 같고 body 한 byte를 바꾸면 달라짐을 확인한다. |
| 잘못된 metadata | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:119` | NUL key를 service packing 전에 protocol_error로 거부한다. |
| 잘못된 static UTF-8 | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:135` | Channel/name/content type 오류가 두 plan 경로 모두 E1에 전달됨을 검사한다. |
| Ambient flow snapshot | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:151` | Submit scope가 끝난 뒤에도 E1의 flow bytes가 유지된다. |
| Copy/materialization/release | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:165` | 복사한 소유 표현의 bytes와 parts가 같고, 폐기할 때 encoder를 실행하지 않는다. |
| Outer flow pair | `tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:191` | 두 표현 모두 기존 wire writer로 정상 pair를 쓰고, 누락 pair와 잘못된 UUID를 거부한다. |

추가 negative 실행은 fixture 파일의 body 한 byte를 고의로 바꾸고 실제 fixture 대조 테스트가 exit 1로 실패하는지 확인한다. 실행 뒤 원본 파일은 `finally`에서 bytes 그대로 복구한다. 결과는 검증 절에 기록한다.

## 기존 테스트의 변경 이유

`test_cpp_framework_request_native_counts.cpp`의 8건 실제 round trip에서 기대값은 요청당 init 합계 **8→7**, close **10→9**, copy **2 유지**다. JSON header native message 한 개의 생성·해제가 없어졌고, binding이 유지하는 두 send view의 copy는 그대로이기 때문이다. 요청별 cleanup을 기다리는 `closes_per_request`도 9로 바꿨다. Timeout·오류·completion 조건은 완화하지 않았다.

`tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport_projection.cpp:291` 이후 네 테스트는 삭제된 observer의 인위적 fault 대신 실제 native 거부·deadline·shutdown·닫힌 registry에서 admitted/public/physical 실패와 한 번의 최종 변환·전달을 검사한다. Native 거부의 구체적인 오류 종류는 binding이 분류한 값을 사용하고 admitted/public/physical 결과가 모두 실패하며 같은 원인을 유지하는지 검사한다. Deadline과 shutdown은 고정된 오류 종류를 검사한다. Registry close 테스트는 raw 등록 경로에 실제로 도달하도록 expected peer를 설정하고 shutting_down을 검사한다.

`test_cpp_framework_request_transport.cpp`, native-count·projection의 세 adapter는 `take_items()`로 별도 표현을 만들지 않고 `message_parts_t`를 넘긴다. Projection의 vector fixture는 explicit 생성자로 변환한다.

`tests/Zlink.Framework.UnitTests/test_cpp_framework_route_client_channel_routing.cpp:934` 이후 publication 검사는 새 `nodes` 표현을 사용하며, 게시된 route 객체가 실제 route runtime과 같은지와 이전 snapshot이 변하지 않는지도 검사한다.

## 리팩토링 점검

추가 발견 5건을 모두 수정했고 넘긴 항목은 0건이다. 승인된 1–5단계의 성능 변경은 위 단계 표에 별도로 정리했다.

| 분류 | 발견 | 처리 |
|---|---|---|
| 성능 | 실패할 수 없어진 lookup의 불필요한 `result_t` 반환 | 남은 send lookup에서도 제거 |
| POSDDD | Owned encoder가 공통 outer-flow validation을 건너뜀 | Owner flow pair를 공통 codec writer로 전달 |
| 불필요한 코드 | 삭제한 observer를 보존하는 fault hook과 예외 캡처 | Hook·extern·catch/rethrow 삭제, 실제 실패 테스트로 전환 |
| 불필요한 코드 | 사용 중인 host wrapper의 불필요한 signature 변경과 implicit vector 변환 | 기존 signature와 explicit 생성자 유지 |
| 불필요한 코드 | Build 설정에서 정의하지 않는 `J1672_BEFORE` 비교 분기 | 네 조건 삭제, 현재 API와 assertion 유지 |

건드린 호출 경로에서 gate를 중복하는 lock·queue·권한 flag나 handler turn의 infrastructure 대기를 추가하지 않았다.

## 변경 파일

- [framework/languages/cpp/CMakeLists.txt:3098](/root/worktree/zlink-3867e/framework/languages/cpp/CMakeLists.txt:3098)
- [framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:912](/root/worktree/zlink-3867e/framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:912)
- [framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:8](/root/worktree/zlink-3867e/framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:8)
- [framework/languages/cpp/framework/include/zlink/framework/detail/test_creation_probe.hpp:17](/root/worktree/zlink-3867e/framework/languages/cpp/framework/include/zlink/framework/detail/test_creation_probe.hpp:17)
- [framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:113](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:113)
- [framework/languages/cpp/framework/src/runtime/channels/channel_runtime.hpp:1](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.hpp:1)
- [framework/languages/cpp/framework/src/runtime/channels/channel_runtime_manager.cpp:183](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/channels/channel_runtime_manager.cpp:183)
- [framework/languages/cpp/framework/src/runtime/host/app.cpp:2629](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/host/app.cpp:2629)
- [framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:43](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:43)
- [framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.hpp:267](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.hpp:267)
- [framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1343](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1343)
- [framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.hpp:237](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.hpp:237)
- [framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:70](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:70)
- [framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.hpp:40](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.hpp:40)
- [framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:3](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:3)
- [framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp:2](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp:2)
- [framework/languages/cpp/framework/src/runtime/messaging/submit_result_mapper.hpp:64](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/messaging/submit_result_mapper.hpp:64)
- [framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:91](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:91)
- [framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.hpp:3](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.hpp:3)
- [framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3401](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3401)
- [framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_native_counts.cpp:100](/root/worktree/zlink-3867e/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_native_counts.cpp:100)
- [framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport.cpp:230](/root/worktree/zlink-3867e/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport.cpp:230)
- [framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport_projection.cpp:13](/root/worktree/zlink-3867e/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_transport_projection.cpp:13)
- [framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_route_client_channel_routing.cpp:949](/root/worktree/zlink-3867e/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_route_client_channel_routing.cpp:949)
- [framework/languages/cpp/framework/src/runtime/protocol/application_envelope_encoder.hpp:1](/root/worktree/zlink-3867e/framework/languages/cpp/framework/src/runtime/protocol/application_envelope_encoder.hpp:1)
- [framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:1](/root/worktree/zlink-3867e/framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp:1)
- [framework/languages/cpp/tests/fixtures/request-wire-main-213e579f52.json:1](/root/worktree/zlink-3867e/framework/languages/cpp/tests/fixtures/request-wire-main-213e579f52.json:1)

## 빌드·검사 결과

- 원본 main library 빌드: 통과. 관찰한 MemAvailable 최소 17.331 GiB (`build-main.log`).
- 최종 전체 빌드: **620/620 단계 통과, exit 0** (`build-final.log`). Samples 7개와 C++ cross-language host를 포함했다.
- 최종 빌드 MemAvailable 관찰 최소: **12.983 GiB**. Compile pool 2와 `--parallel 3`를 사용했고 memory guard에 의한 중단은 없었다.
- 최종 전체 C++ format: `bash scripts/format/format.sh --check cpp`, **exit 0** (`format-check.log`).
- `git diff --check`와 새 파일을 포함한 `final.diff` 역적용 검사: 통과.
- Wire 테스트: **12/12 case 통과** (`ctest-wire.log`).
- Fixture 한 byte 변조: 실제 이름이 일치하는 비교 테스트 1개가 실행되어 **exit 1로 거부**, helper는 exit 0. 원본 fixture hash도 복구 후 같았다 (`fixture-corruption.log`, `fixture-corruption-check.log`).
- 관련 테스트 첫 실행: 4개 타깃 중 3개 통과, request-call-allocation 타깃의 새 3개 fixture 실패 (`ctest-focused.log`). Location Store completion 직후 C1 등록이 아직 실행되지 않은 상태에서 만료·종료를 강제한 테스트 순서가 원인이다.
- 수정: Native 거부는 실제 public/physical 실패를 기다리고, 만료·종료는 기존 `pending_operation_count()`로 C1 등록을 확인한 뒤 실행한다. 기존 runtime pump를 사용하며 production 코드나 request timeout을 바꾸지 않았다. 해당 테스트 소스와 실행 파일의 증분 빌드 2/2 단계가 통과했다 (`build-test-coordination.log`).
- Native 호출 수: 8건에서 init=32, init_size=24, init_data=0, copy=16, close=72. 합계 **요청당 init 7·copy 2·close 9**가 통과했다 (`ctest-focused.log`).
- 수정한 실패 경로 **4/4 case 통과** (`coordination-cases.log`), request-call-allocation 타깃 전체도 통과했다 (`ctest-call-fixed.log`).
- 최종 unit/contract: **100% tests passed, 0 tests failed out of 298**, 175.64초, exit 0 (`ctest-final.log`). 관련 다섯 타깃도 모두 통과한 상태다.
- 최종 검증 구간의 MemAvailable 관찰 최소는 25.394 GiB다 (`verification.memory.tsv`).
- 남은 실패와 건너뛴 테스트는 없다.

전체 빌드 명령은 다음과 같다.

```sh
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0
```

최종 전체 검사 명령은 다음과 같다. 검증 단계는 측정 잠금을 공유하며 순서대로 실행했다.

```sh
/root/jobs/fx/cx/measure-shared.sh ctest --preset linux-ninja-release -L '^(unit|contract)$' --output-on-failure
bash scripts/format/format.sh --check cpp
```

## 남은 위험과 작업 범위

남은 테스트 실패와 알려진 제품 blocker는 없다. 성능 판정은 감독자의 후속 측정에서 확인한다. Throughput 측정과 네 언어 실행 검증은 수행하지 않았다. C++ cross-language host와 samples는 빌드로 확인했다.

착수 때 저장소 지침의 `scripts/dev/internal-docs.sh`를 실행하면서 이번 작업의 Git 제한을 놓쳤다. 이 스크립트는 read-only root 조회 외에 외부 `/root/zlink-docs` 사본의 `git pull`과 `~/.claude/agents` 복사를 수행한다. 초기 branch 확인에도 `git branch --show-current`를 사용했다. 이후 직접 실행한 Git 명령은 diff/status/apply로 제한했고 CMake의 Git 탐색도 비활성화했다. 제품 저장소에서 commit·push·branch 변경이나 GitHub Issue/PR 작업은 하지 않았다. 다른 모델·AI 도구·sub-agent는 사용하지 않았다.
