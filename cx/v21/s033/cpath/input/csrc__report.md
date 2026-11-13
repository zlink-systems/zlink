# #3868 C++ Framework source send 경로

Source user 중앙값은 5.248에서 3.958 µs/message로 24.57% 감소했다. Source kernel은 0.975, target kernel은 1.192 µs/message 감소했다. 그러나 source user 예산 2.500을 1.458 초과한다. Saturation F÷B는 0.2832에서 0.2965로 올랐지만 목표 0.6에 미달하며, binding ÷ 0.6 경과시간 예산을 2.703 µs/message 초과한다.

전체 unit/contract는 306개 중 304 통과, 2 실패, skip 0이다. Oversized multipart는 public binding만으로도 실패했다. 추가 location 실패 중 원래 case 하나도 변경 전 main에서 재현됐다. 나머지 case의 근본 원인은 확정하지 않았으며 실패는 남아 있다.

기준 branch는 `perf/3868-cpp-source-path`이며 사용자가 제시한 main 기준은 `ce1b779323`이다. 시작할 때 `git status --short --branch`에서 기존 변경이 없었다. Core, binding, spec과 다른 언어 구현은 수정하지 않았다. Commit과 GitHub 작업도 하지 않았다.

## 1. Source user CPU 귀속

`csprof2/cpp-framework-136889-fp-r{1,2,3}/capture/source.perf.data`를 사용한다. 파일명은 fp지만 source의 실제 record 옵션은 `dwarf,8192`다. 당시 바이너리는 `csendb/build-main`이며 이번 main과 revision이 다르다. csprof2의 source user 실측은 5.260, 5.280, 5.273 µs/message다. 아래 귀속은 user 표본의 period 비율을 ckern2의 4.912 µs/message에 정규화한 추정치다. 서로 다른 실행의 CPU를 같은 실측값으로 표시하지 않는다. 원본 perf 입력과 /proc snapshot의 사본, 함수별 전체 귀속은 `raw/profile-history-r*/`와 `raw/attribution.json`에 보존한다.

C++ Framework 경로를 가장 가까운 함수에 배타적으로 귀속했다. Allocator·mutex·shared_ptr helper는 자신의 leaf 이름으로 별도 비용을 중복 합산하지 않고 호출자 함수에 귀속한다. 위치는 당시 matching source의 대표 정의·호출 지점이다. `extensions/`와 `src/`·`include/`는 `framework/languages/cpp/`와 그 `framework/` 아래를 뜻한다.

| 함수 | file:line | 정규화 user µs/message | Binding 경로도 수행 | 존재 이유와 중복 작업 |
| --- | --- | ---: | --- | --- |
| `Core transport` | `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:70; native library` | 1.362 | 예 | Binding에도 존재한다. Physical admission·I/O는 Core/binding 소유다. |
| `Binding operation builder/completion` | `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:132` | 0.259 | 예 | Binding에도 존재한다. §E4 multipart 제출과 backpressure snapshot을 제공한다. |
| `Bench driver·metrics·pacing` | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:889` | 0.455 | 예 | 벤치 측정 작업이다. Framework 요구 조항은 없다. 제품 변경 대상이 아니다. |
| `Protobuf serializer와 typed serializer closure` | `extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:27` | 0.222 | Protobuf만 예; erasure는 추가 | 05-payload-ownership-and-codec §5·6의 typed codec이다. Binding도 protobuf를 쓰지만 Framework에는 serializer erasure가 추가된다. |
| `route_client_t::submit_send_erased` | `src/runtime/channels/channel_runtime.cpp:1441` | 0.209 | 아니오 | 08-messaging-hot-path E2의 transport 선택과 오류 매핑이다. Callable 보관과 result projection의 중간 표현은 spec 요구가 아니다. |
| `app mesh transport closure와 send_mesh_node_t::run` | `src/runtime/host/app.cpp:2600` | 0.214 | 아니오 | 02-channel-messaging §8의 local/remote 구분이다. 전달 전용 coroutine frame·shared result는 spec 요구가 아니다. |
| `mesh_node_runtime_t::routing_id_async와 await_resume` | `src/runtime/mesh/mesh_node_runtime.cpp:4732` | 0.138 | 아니오 | Local RID 조회다. 이미 있는 immutable identity를 task로 포장할 요구 조항은 없다. |
| `raw_route_port_t::submit_send_impl와 submit_send` | `src/runtime/backend/raw_route_port.cpp:152` | 0.134 | 아니오 | 08-messaging-hot-path E4다. Binding의 builder를 시작한다. Native part의 identity materializer·반복 move·builder 자기 move 대입은 요구 조항이 없다. |
| `header_writer_t::append, write_header, encode_header` | `src/runtime/messaging/envelope_codec.cpp:245,424,498` | 0.276 | 아니오 | 06-wire-protocol의 canonical JSON header다. Header 크기 계산과 실제 쓰기에서 두 번 방문한다. 완성된 같은 bytes를 다시 만드는 요구 조항은 없다. |
| `native_wire_writer_t와 append_u32·append_text8` | `src/runtime/protocol/service_wire_codec.cpp:62,125,182` | 0.352 | 아니오 | Wire 길이·경계·big-endian 표현에 필요하다. Byte마다 native message size를 호출할 요구 조항은 없다. Native buffer 할당과 최종 frame 복사가 포함된다. |
| `encode_application_payload_message` | `src/runtime/protocol/service_wire_codec.cpp:3224` | 0.106 | 아니오 | Service wire의 단일 application frame을 만든다. Header/body bytes를 최종 frame에 복사한다. Binding 단독 벤치는 이 envelope를 만들지 않는다. |
| `application_payload_t 소멸과 variant 정리` | `src/runtime/protocol/service_wire_codec.hpp:73` | 0.118 | 아니오 | 05-payload-ownership-and-codec §4의 정확히 한 번 해제다. Vector·중간 소유 객체 수는 계약이 정하지 않는다. |
| `service_topology_registry_t::peer` | `src/runtime/mesh/service_topology_registry.cpp:515` | 0.084 | 아니오 | 분류와 현재 ready admission에 사용한다. 같은 peer를 두 번 조회하지만 판정 목적이 달라 하나를 삭제하지 않는다. Snapshot shared_ptr 획득 비용이 포함된다. |
| `serializer_registry_t::cached_serializer와 get` | `src/runtime/codecs/serializer.cpp:159; include/zlink/framework/contracts/codecs/serializer.hpp:322` | 0.081 | 아니오 | Typed serializer 선택이다. Binding 단독 호출에는 registry 조회와 shared_ptr 획득이 없다. |
| `create_envelope와 envelope_header_t 소멸` | `src/runtime/messaging/client_call_codec.cpp:93; messaging/envelope_codec.hpp:145` | 0.096 | 아니오 | Canonical header 필드·metadata·flow를 소유한다. 임시 header 객체 포장은 spec 요구가 아니다. |
| `message_parts_t 생성과 encode_payload_parts` | `src/runtime/messaging/envelope_codec.cpp:24; client_call_codec.cpp:65` | 0.071 | 아니오 | Header/body 소유권이다. 두 parts의 vector 할당은 Binding 벤치에 없다. Byte 전체 복사와 vector descriptor 할당을 구별한다. |
| `raw mesh send 시작·결과 포장` | `src/runtime/mesh/raw_mesh_node_owner.cpp:1277,1652,1755,1787` | 0.121 | 아니오 | E3/E4, submit/completion §10·11이다. Pending일 때 registry가 필요하다. 즉시 결과의 계층별 result_t 포장은 요구 조항이 없다. |
| `state_lane_t::schedule_drain와 drain_loop` | `src/runtime/execution/state_lane.cpp:134,169` | 0.053 | 아니오 | 06-state-ownership-and-lanes의 owner turn이다. Socket close와 제출 직렬화는 유지한다. |
| `task_shared_state_t<result_t<void>>::complete` | `include/zlink/framework/contracts/dispatch/task.hpp:664` | 0.043 | 아니오 | 완료 전달이다. 즉시 send 전달용 coroutine이 공유 state를 만드는 것은 계약 요구가 아니다. |
| `public_host_runtime_t::send_to_node` | `src/runtime/stateful/public_host_runtime.cpp:3365` | 0.032 | 아니오 | Wire 제출 경계다. Typed RID를 byte vector로 만든 뒤 다시 typed RID로 되돌리는 복사는 계약 요구가 없다. |
| `route_send_call_t::async와 bound_submission_t` | `src/runtime/channels/channel_runtime.cpp:1410; include/zlink/framework/contracts/channels/channel.hpp:687` | 0.040 | 아니오 | 호출 한 번 제출 계약이다. 공유한 submit_once 판단을 유지한다. Binding 단독 bench에는 fluent call 객체가 없다. |
| `application_payload_t::from_parts와 packet_name` | `src/runtime/protocol/service_wire_codec.cpp:2958,3002` | 0.027 | 아니오 | Application payload 표현과 profile 이름이다. Rvalue parts는 move하며 body deep copy로 계산하지 않는다. |
| `Header 검증과 metadata 쓰기` | `src/runtime/messaging/envelope_codec.cpp:168,326,464` | 0.029 | 아니오 | Wire 오류·flow pair·metadata 제한에 필요하다. Metadata의 크기/쓰기 방문은 두 번이다. 검증 규칙을 완화하지 않는다. |
| `그 밖의 user 함수` | `raw/attribution.json 및 raw/attribution-locations.json` | 0.390 | 혼합·미분류 | 작은 coroutine·ambient context·value 소멸과 libc helper다. 전체 함수별 수치는 raw에 있다. 미귀속을 제거 가능한 비용으로 가정하지 않는다. |
| 합계 |  | 4.912 |  | 세 실행의 user period 비율 평균이다. |

Binding 열의 예는 같은 기능을 수행한다는 뜻이며 CPU 비용이 같다는 뜻은 아니다. Framework에서 늘어난 source 제출 간격에 따라 Core 비용도 달라질 수 있다.

Core/binding 묶음의 주요 함수도 별도로 확인했다. 다음 비용은 위 표에 이미 들어 있으므로 합계에 다시 더하지 않는다. 나머지 native 함수의 배타 비용은 `raw/attribution-native-functions.json`에 있다. 아래 Core 위치는 matching source의 정의 또는 호출 지점이며 Core C header는 읽지 않았다.

| 함수 | file:line | 정규화 user µs/message | 이유 |
|---|---|---:|---|
| `encoder_base_t<zmp_encoder_t>::encode` | `core/src/runtime/protocol/encoder.hpp:47` | 0.220 | Core wire encoding과 transport buffer 조립이다. Binding도 수행한다. |
| `submit_raw_send_state` | `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:29` | 0.112 | Binding의 native admission이다. Framework가 대체하지 않는다. |
| `socket_base_t::try_admit_send_parts_scoped` | `core/src/runtime/sockets/common/socket_send_submit.cpp:288` | 0.071 | Core의 multipart admission이다. |
| `pipe_t::write_message_unlocked` | `core/src/runtime/core/pipe_write.cpp:1175` | 0.059 | Core pipe에 message를 게시한다. |
| `router_socket_t::send` | `bindings/cpp/src/Runtime/Sockets/router.cpp:22` | 0.057 | Binding builder를 만든다. |
| `send_submit_operation_t::async` | `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:171` | 0.055 | Binding의 비동기 submit 시작이다. |

### 현재 바이너리의 보조 profile

추가로 현재 main·변경판 Release 바이너리를 136,889 msg/s에서 각각 한 번 profile했다. Source user 표본은 1,634, 1,240개다. `perf record -F 199 --call-graph dwarf,16384 -N`을 사용했다. `-N`은 build-id cache 쓰기를 끈다. Source user ticks로 정규화한 profile 실행의 비용은 5.312, 4.086 µs/message다. 이는 perf 없는 3회 CPU 중앙값과 구별한다. 아래 그룹 변화는 전체 변경의 동시 적용 결과이며 수정 항목별 독립 절감으로 합산하지 않는다.

| 그룹 | 대표 file:line | Main profile µs/message | 변경판 profile µs/message |
|---|---|---:|---:|
| Core transport | `기존 Core transport; 위 native 함수 위치` | 1.437 | 1.167 |
| Bench | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:889` | 0.436 | 0.455 |
| Envelope representation | `messaging/envelope_codec.cpp:24; messaging/client_call_codec.cpp:94` | 0.410 | 0.432 |
| Service frame encoder | `protocol/service_wire_codec.cpp:59,3224` | 0.549 | 0.372 |
| Route public submit | `channels/channel_runtime.cpp:1441` | 0.507 | 0.250 |
| Binding builder/completion | `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:132` | 0.328 | 0.217 |
| Application payload representation | `protocol/service_wire_codec.cpp:2995,3005` | 0.247 | 0.211 |
| Envelope header | `messaging/envelope_codec.cpp:519` | 0.319 | 0.089 |
| Identity task | `mesh/mesh_node_runtime.cpp:4724,4732` | 0.140 | 0.049 |

함수별 전체 귀속과 period는 `raw/fresh-source-attribution.json`에 있다. 원본은 `raw/profiles/{main,change}-2/capture/`다. 두 source/target perf 입력, stderr, 명령, maps와 script 출력을 보존했다. 첫 추가 profile은 이 서버 perf가 `--buildid-dir=...` global 옵션을 지원하지 않아 실패했다. `raw/profiles/main-1/`에 실패를 남겼고 지원되는 `-N`으로 cache 쓰기를 끈 재시도가 완료됐다. 이 실패를 24개 정상 비교 cell에 포함하지 않았다.

남은 비용은 envelope/header 객체와 parts 표현, public submit closure/result, application payload 표현에도 분산돼 있다. 위 표에서 유지되는 Core·binding·bench·protobuf와 service/header encoding의 변경판 profile 비용을 합하면 약 2.396 µs/message다. 이 합은 불변 하한이 아니다. 현재 cadence와 profile 조건의 관측치다. 2.500 예산을 맞추려면 다른 모든 표현·dispatch 비용을 약 0.104 안에 두거나 이 묶음도 더 줄여야 한다. 작은 helper 하나를 제거하면 예산을 맞춘다고 주장할 근거는 없다.

## 2. 예산과 제거 순서

Source user 예산은 2.50 µs/message다. ckern2 binding 1.520보다 최대 0.980을 추가할 수 있다. 기존 Framework 4.912에서 최소 2.412를 제거해야 한다.

Stage 단위 초기 목표 배분은 encode 1.10, Framework part-container/submit 포장 0.45, dispatch/coroutine 0.35와 남은 route/serializer/하위 계층 비용 0.512 µs/message였다. 이 2.412는 목표에서 역산한 배분이다. 구현만으로 그만큼 없어졌다는 예측이나 판정으로 쓰지 않는다. 특히 기존 stage의 inclusive 비용을 함수별 배타 비용과 더하지 않는다.

함수 귀속에 근거한 실제 제거 후보와 예상 절감은 아래와 같다. 마지막 행은 source cadence가 Core 작업을 줄일 때만 가능한 간접 효과이며 보장하지 않는다. 이 함수별 예상 합계만으로 예산이 닫히지 않으므로 최종 판정은 새 측정에 따른다.

| 제거 대상 | 예상 절감 µs/message | 근거와 제거 방식 |
|---|---:|---|
| 같은 header의 반복 인코딩 | 0.25–0.30 | `header_writer::append`, `write_header`, `encode_header`의 배타 비용 약 0.276 중 반복 쓰기를 제거한다. 기존 type fragments를 유지하며 완성된 header bytes도 재사용한다. |
| Writer의 byte별 native size 조회 | 0.15–0.20 | `append_u32`·`push_back`·`append_text8` 안의 반복 native 호출을 줄인다. Native 할당·UTF-8 검사·body 복사 자체는 유지한다. |
| 최종 part vector·RID vector·native wrapper 이동 | 0.12–0.18 | `send_with_header_result`, port submit, public-host RID 변환과 중간 이전 비용에서 제거한다. Binding builder에 stack parts를 넘긴다. |
| app 전달 coroutine·identity task·공유 결과 | 0.30–0.40 | app transport closure, `send_mesh_node_t::run`, `routing_id_async`, continuation과 공유 task state의 비용에서 제거한다. 기존 sync identity와 task projection을 사용한다. |
| Source cadence에 따른 추가 Core/binding user 작업 | 0–0.70 | Framework user 작업을 줄인 뒤 batching이 개선될 때만 가능하다. 새 queue·batching 규칙을 만들지 않는다. Kernel 절감은 이 user CPU 합계에 넣지 않는다. |
| 합계 | 0.82–1.78 | 필요한 2.412와 비교하면 예상만으로는 0.632–1.592가 부족하다. Source user 2.50 달성 여부와 이 부족분은 실측으로 판정한다. |

대안은 최종 service payload에 caller header/body를 직접 쓰는 deferred encoder다. csasm은 이 방식에서 source 총 CPU가 9.071에서 9.863으로 증가했고 csendb에서도 더 느렸다. 이 실험을 반복하지 않는다. Target 다중 drain도 cdrain과 cdrain2에서 cheap send가 각각 11.44%, 12.51% 느려져 제외한다. 선택한 방식은 현재 wire 조립을 유지하며 source의 표현 이동과 반복 계산을 제거한다.

## 3. 결정 소유와 변경 범위

구현 전 결정 소유 표다. 위치는 `framework/languages/` 기준이다. .NET과 Java는 이미 완성된 단순 header bytes를 재사용한다. Kotlin은 Java runtime을 사용한다. Node.js는 service NodeSend header를 상수로 보관하고 application frame을 binding에 보낸다. 언어별 buffer 보관 방식은 다르지만 wire, 오류, admission과 payload 수명 계약은 같다. 이번 변경은 C++의 표현·계산 제거이며 사용자 동작을 바꾸지 않는다.

| 결정 | Spec 소유 조항 | C++ 판정 위치 | 다른 언어의 위치 | 변경 후 소유자 |
|---|---|---|---|---|
| Wire encoding과 flow snapshot | 08-messaging-hot-path §3 E1, 06-wire-protocol 및 schema | `cpp/framework/src/runtime/messaging/envelope_codec.cpp:498`, `protocol/service_wire_codec.cpp:3224` | `.NET Runtime/Messaging/ZLinkEnvelopeCodec.cs:188`, Java `runtime/messaging/ZLinkChannelEnvelope.java:506`; Kotlin은 Java 공유; Node `runtime/foundation/raw-service-mesh-runtime.ts:641` | 기존 codec |
| Local/remote 대상 선택 | 02-channel-messaging §8 | `cpp/framework/src/runtime/host/app.cpp:2600` | .NET `Runtime/Service/ZLinkManagedMeshNode.cs:3413`; Java `runtime/mesh/ZLinkMeshNodeRuntime.java`; Node 위 `sendToNode` | 기존 mesh transport |
| Object Client 분류와 live peer admission | 02-channel-messaging §8, 08-messaging-hot-path §3 E2/E4 | `mesh_node_runtime.cpp:4194`, `raw_mesh_node_owner.cpp:1764,4369` | 언어별 managed mesh owner | 기존 topology와 mesh owner. 분류와 현재 admission은 다른 판정이므로 합치지 않는다. |
| Physical retry와 payload 보관 | 01-submit-and-completion §4·5, 08-messaging-hot-path E4 | `backend/raw_route_port.cpp:181`은 binding operation을 시작한다. retry 자체는 binding 소유다. | 다른 언어도 binding submit 경계 사용 | binding builder와 async 결과 |
| Framework pending 등록과 close | 01-submit-and-completion §10·11 | `raw_mesh_node_owner.cpp:1665` | 언어별 operation owner | 기존 owner lane와 registry |
| Wire bounds와 frame ownership | 05-payload-ownership-and-codec §1·2·4 | `service_wire_codec.cpp:59`, `raw_route_port.cpp:181` | 언어별 codec/binding storage | codec의 남은 span, binding의 message 소유권 |

분류는 B다. 공개 계약 변경 없이 C++의 중복 표현과 반복 작업을 제거한다. 물리 연결 선택·재제출·completion drain을 Framework에 추가하지 않는다.


### 변경 파일과 소유권

구현 위치는 현재 worktree의 `framework/languages/cpp/framework/` 기준이다. 테스트 위치는 `framework/languages/cpp/` 기준이다. Wire·error·admission·ownership 계약은 바꾸지 않는다.

| 파일과 위치 | 변경 |
|---|---|
| `src/runtime/messaging/envelope_codec.cpp:375,469,519` | 기존 thread-local type plan 하나가 type fragments와 그 type의 처음 완성된 header bytes를 함께 소유한다. 같은 값은 bytes를 복사해 새 native message로 반환한다. Correlation·metadata·flow만 바뀌면 기존 fragments로 인코딩하며 cache key를 다시 복사하지 않는다. Generic/prepared header의 크기 계산·native buffer 쓰기는 `encode_header_bytes` 한 곳에 모았다. |
| `src/runtime/protocol/service_wire_codec.cpp:59` | Writer의 `_data`·`_size`를 남은 span으로 대체했다. Byte마다 native `message_t::size()`를 호출하지 않는다. 범위 검사와 마지막 길이 일치 검사는 유지한다. Allocation 성공을 확인한 뒤 span을 만든다. |
| `src/runtime/mesh/raw_mesh_node_owner.cpp:1652,1666,1756,1792` 및 `.hpp` | 최종 service frame들을 stack array에서 제출한다. Lane와 port에는 span을 빌려 준다. `run_sync`가 반환할 때는 binding이 bytes와 target을 소비하거나 pending operation 안에 보관했다. Metadata의 materialization은 기존처럼 payload 인코딩보다 먼저 한다. |
| `src/runtime/backend/raw_route_port.cpp:137,153,161` 및 `.hpp` | Native parts span을 binding multipart builder에 직접 이전한다. Native part identity materializer와 builder 자기 move 대입을 없앴다. Socket mutex, immediate 결과 판정, pending observer와 오류 분류는 유지한다. |
| `src/runtime/stateful/public_host_runtime.cpp:3365,3380` | Typed RID에서 byte vector를 만들지 않고 pointer·size view를 전달한다. Port가 다시 typed RID를 만들 때 binding의 기존 `from(data,size)`를 쓴다. |
| `src/runtime/host/app.cpp:2601` | 기존 동기 identity 조회와 task projection으로 Node send 결과를 전달한다. 즉시 admission에서 전달 전용 coroutine frame과 그 공유 결과 storage를 만들지 않는다. Pending에서는 기존 projection이 mesh/service의 수명을 유지한다. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_envelope_allocations.cpp:215` | 반환 bytes의 독립성, 모든 header 값 변경, 잘못된 correlation·metadata·flow 거부와 correlation 변경 시 기존 allocation 상한을 검증한다. |
| `tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp:1022` | Stack parts와 RID view를 넘긴 뒤 원래 RID를 덮어쓰고 wrapper를 파괴한다. HWM pending을 실제로 만든 뒤 admission과 두 frame의 원래 bytes 도착을 확인한다. |

Cache key는 `messaging/envelope_codec.hpp:47`의 text 복사와 `:132`의 storage 복사를 사용한다. View 문자열은 소유 문자열이 되고 native retained-header message는 복사하지 않는다. 입력 wire frame을 cache 수명만큼 보관하거나 clone하지 않는다. 기존 `CsitesHeaderViewsRetainWireAcrossCopiesAndSuspension`과 새 독립 writable bytes 검사가 이 소유권 경계를 확인한다.

### 판정 위치와 mutex

Borrowed span의 수명 근거는 `src/runtime/execution/state_lane.hpp:289`의 기존 `run_sync`다. Caller가 자신의 synchronous turn을 실행하고 result/error를 관찰한 뒤 반환한다. 다른 executor에 span을 넘겨 두고 먼저 반환하는 경로가 아니다. Binding submit 이후의 pending은 builder가 이전한 native messages와 target을 소유한다. 정상 backpressure 대조가 caller storage 파괴 뒤의 bytes를 확인한다.


Header의 크기 측정·native 쓰기 구현은 2곳에서 1곳으로 줄었다. 일반 overload와 prepared overload가 같은 writer를 쓴다. Native writer의 byte 경계 검사 위치는 push·insert·generate·take의 4곳으로 전후가 같고, 별도 native size 조회는 남은 span의 길이 조회로 바뀌었다.

최종 service frames가 만들어진 뒤 binding builder에 도달할 때까지의 **소유 표현 위치는 8곳에서 2곳**으로 줄었다. 이전에는 `send_with_header_result`의 vector, `submit_owned_send_result`의 값 parameter, `start_send`의 값 parameter, lane lambda capture, `start_send_on_lane`의 값 parameter, port의 값 parameter, `submit_send_impl`의 값 parameter와 binding builder가 순서대로 소유했다. 이후에는 `send_with_header_result`의 stack array와 binding builder가 소유하며 중간 호출은 span을 빌린다. 이는 payload byte 복사 8회가 있었다는 뜻이 아니다. 기존 vector move도 대부분 같은 저장소를 이전했다.

Known target의 정상 즉시 제출에서 local/remote 선택, Object Client 분류, live-peer ready, port 존재, socket 존재, binding result와 registry 등록 여부의 소유자는 바뀌지 않는다. 명시적 Framework mutex 소유자는 owner lane의 mailbox mutex와 port의 socket mutex **2개에서 2개**다. 이는 lock 획득 횟수나 전체 Core/binding lock 수가 아니다. Pending에만 필요한 lifecycle·registry lock도 유지한다. 새 queue·lock·flag·counter는 없다. Writer의 size cursor는 제거했고 기존 header plan의 key와 backing storage를 대체했다.

수정 전/후 규칙 수: header 크기 측정·쓰기 판정 위치 **2→1**, 최종 frame 소유 표현 **8→2**, 정상 제출의 명시적 Framework mutex 소유자 **2→2**다. Physical retry·completion drain·errno 분류에는 새 판정 위치가 없다.

Cache의 실행 분기도 별도로 센다. Type 일치만 확인하던 경로에 완성된 header 값 일치를 추가했으므로 cache hit 판정은 **1→2**다. 이는 같은 소유자 안의 계산 재사용 조건이며 새 admission·retry 규칙은 아니다. Native header allocation 실패 검사 위치는 기존 generic/prepared 각각 한 곳의 **2곳에서**, 공통 writer와 cached bytes 복사의 **2곳**으로 유지된다. 이 cache 조건 증가를 header writer 통합이나 frame 소유 표현 감소에 숨기지 않는다.

| 센 범위 | 수정 전 | 수정 후 |
|---|---:|---:|
| Header measure/write 구현 위치 | 2 | 1 |
| Header cache hit 판정 | 1 | 2 |
| Header native allocation 실패 검사 위치 | 2 | 2 |
| Native service writer 경계 검사 위치 | 4 | 4 |
| 위 네 범위의 판정·구현 위치 합계 | 9 | 9 |
| 최종 frame의 순차 소유 표현 | 8 | 2 |
| 정상 제출의 명시적 Framework mutex 소유자 | 2 | 2 |

### 리팩토링 점검

| 중복 검사·이전·복사 | 위치 | 결과 |
|---|---|---|
| 같은 header의 크기 계산과 JSON 쓰기 | `messaging/envelope_codec.cpp:519` | 동일 header는 제거했다. 값이 달라지면 canonical encode와 검증을 유지한다. |
| Generic/prepared header의 같은 측정·쓰기 코드 | `messaging/envelope_codec.cpp:469,549` | Writer 한 곳으로 모았다. |
| Byte마다 native capacity 조회 | `protocol/service_wire_codec.cpp:59` | 제거했다. 남은 span 하나가 경계를 소유한다. |
| 최종 service parts vector 할당과 계층별 move | `mesh/raw_mesh_node_owner.cpp:1756` | Stack array와 borrowed span으로 대체했다. |
| Native part identity materialization과 builder 자기 move 대입 | `backend/raw_route_port.cpp:200` | Native send 경로에서 제거했다. Raw byte parts를 native message로 만드는 경로는 유지한다. |
| Typed RID → vector → typed RID | `stateful/public_host_runtime.cpp:3365`, `backend/raw_route_port.cpp:208` | Vector를 제거했다. Binding이 operation 시작 시 target을 보관한다. |
| Identity task와 전달 전용 coroutine/shared result | `host/app.cpp:2601` | 정상 즉시 제출에서 제거했다. |
| 같은 peer의 분류/ready 조회 | `mesh/mesh_node_runtime.cpp:4194`, `mesh/raw_mesh_node_owner.cpp:1765` | 유지했다. Object Client 분류와 현재 admission은 다른 결정이다. |
| Header/body의 단일 service frame 조립 | `protocol/service_wire_codec.cpp:3224` | 유지했다. Wire format을 바꾸지 않으며 csasm의 단일 조립 실험을 반복하지 않는다. |
| Public result의 계층별 표현, serializer callback과 pending registry | `channels/channel_runtime.cpp:1441`, `codecs/serializer.cpp:159`, `mesh/raw_mesh_node_owner.cpp:1666` | 유지했다. 이번 변경의 절감으로 주장하지 않는다. |

## 4. 검증

빌드는 `linux-ninja-release` preset, `-O3 -DNDEBUG`이며 `-g`를 추가하지 않았다. Main과 change는 test probes를 끄고 같은 native Core 1.23.0과 C++ binding 1.23.0을 링크한다. 테스트 빌드는 같은 Release 설정에 creation probes를 켠 별도 directory다. 설정·실제 compile commands·package SHA-256은 `raw/configure-*.log`, `raw/compile-commands-*.json`, `raw/build-provenance.json`에 있다.

처음 scratch 구성에서 snapshot의 relative generated protocol include가 없어서 빌드가 실패했다. 필요한 protocol 입력을 snapshot에 복사했다. 첫 change/focused 빌드에서는 binding package에 span RID overload가 없어 컴파일이 실패했고, 기존 pointer·size overload로 고쳤다. 모든 실패 로그를 보존했다.

동시 빌드 중 MemAvailable이 12 GiB 아래로 내려간 구간이 있었고 직접 시작한 빌드의 자식들에 SIGTERM을 보내 중단했다. 최초 SIGINT만으로는 모든 compiler가 종료되지 않았다. 중단 상태는 `raw/memory-build-stop.json`, `raw/memory-build-terminated.json`에 있다. 이후 병렬 1로 재개하고 13 GiB 경계에서 중단하는 감시를 적용했다. 이 구간을 메모리 조건을 충족한 실행으로 표시하지 않는다. 성능 측정은 별도 exclusive lock과 12 GiB 검사를 적용한다. 측정 batch와 추가 profile이 끝난 뒤 최종 전체 테스트 빌드는 메모리 23 GiB 이상을 확인하고 병렬 2로 진행했다. 시작 조건 20 GiB와 중단 경계 13 GiB를 적용했고 완료했다. 최종 빌드 직후 전체 csrc 크기는 7.8 GiB였다. 중단·재개와 병렬도 변경은 `raw/build-*-status.json`, `raw/build-parallel-adjustment.json`에 있다.

최종 테스트 결과와 측정 결과는 아래에 기록한다.

현재 main의 벤치 Node send는 `channel_runtime.cpp:1482`의 `encode_node_send_parts`에서 `client_call_codec.hpp:91`의 일반 `encode_payload_parts`를 선택한다. 그 결과는 native parts vector이며 `app.cpp:2617`의 `take_items()`를 거쳐 `application_payload_t::from_parts`로 이동한다. `encode_application_payload_message`의 deferred `envelope_encoder` branch는 코드에 남아 있지만 이 Node send 호출이 선택하는 표현은 아니다. 이 정적 호출 경로는 csprof2에서 관찰된 `encode_header` 스택과도 맞는다. Deferred request/activation encoder를 send에 새로 연결하지 않았다.

### Focused 검증과 하위 계층 재현

처음 focused 실행은 새 stack-parts 검사의 `received == submitted`에서 실패했다. Timeout이나 HWM, assertion을 바꿔 통과시키지 않았다. `raw/test-focused.log`와 `raw/test-stack-trace.log`에 실패를 보존했다. 기존 raw send-stage trace는 submit `begin` 뒤 admission이 끝나지 않았음을 보였다. Source monitor의 paused count는 이미 0이었다.

Framework를 링크하지 않는 public C++ binding 재현을 만들었다. 소스는 `scripts/ownership-repro.cpp`, 처음 소스 사본은 `raw/native-ownership-repro-v1.cpp`다. 같은 16-byte send/receive HWM에서 target을 pause하고 제출한 뒤 resume한다. 두 frame (`header`, 1 KiB body)은 RID 자동/명시 설정 모두 submitted=1, received=0, admission ready=false로 멈췄다. 같은 body의 단일 frame은 두 설정 모두 received=1, admission terminal=ok였다. `raw/native-ownership-cases.json`, `raw/native-{default,explicit}-{multi,single}.log`에 원시 결과가 있다. 이 재현은 Framework span이나 cache를 사용하지 않는다.

Public binding의 HWM 2048 multipart 대조는 submitted=1, received=1, terminal=ok였다(`raw/test-native-normal.log`). Framework에도 이 정상 크기 대조를 추가했고, caller RID를 덮어쓰고 stack wrappers를 파괴한 뒤 원래 두 frame이 도착했다(`raw/test-stack-controls.log`). 원래 HWM 16 oversized 실패도 test 끝에 그대로 남겼다. 성공 대조를 추가한 것이며 기존 실패 조건을 제거하거나 skip하지 않았다.

이는 Core/binding 경계의 oversized multipart pending 문제다. Core Auto HWM §「Multipart와 큰 message」는 complete message 한 건의 빈 queue oversize admission을 정한다(`core/doc/spec/core/systems/06-auto-hwm.ko.md:462`). Public binding 재현만으로 Core와 binding 중 어느 구현이 잘못됐는지까지 확정하지 않는다. 이번 작업은 두 계층의 수정과 GitHub 작업을 금지하므로 변경하거나 이슈를 등록하지 않았다. Framework의 retry·HWM 보상도 추가하지 않았다.

다른 focused 7개는 stress-ng 부하에서 모두 통과했다. 대상은 service wire codec, generated codec, route-client channel routing, envelope allocations, request wire identity, send allocations와 route payload ownership이다. `raw/test-focused-other.log`, `.xml`, `raw/test-focused-other-status.json`에 있다. Full unit/contract는 이 알려진 실패를 포함해 한 번 실행했다.

### Full unit/contract 결과

`ctest --test-dir build-tests -L 'unit|contract' --output-on-failure`를 한 번 실행했다. CTest 항목 306개 중 **304 통과, 2 실패, skip 0**다. 실행 시간은 277.2초, 종료 코드는 8다.

전체 실행은 `measure-shared.sh` 안에서 stress-ng CPU worker 8개·80% 부하와 함께 수행했다. MemAvailable 최저는 15.805 GiB다. Stress process도 종료 코드 0으로 정리했다. 앞의 focused 실행은 CPU worker 2개·80%였으며 그 조건을 전체 실행 조건과 혼동하지 않는다.

| 실패한 CTest 항목 | 결과 | 로그 |
|---|---|---|
| `test_cpp_framework_raw_route_port_contract` | 정상 HWM의 stack/RID 수명 대조는 통과했다. HWM 16 oversized multipart에서 admission이 완료되지 않아 마지막 assertion이 실패했다. Public binding 재현과 같은 경계다. | `raw/test-all.log`, `raw/test-stack-controls.log`, `raw/native-ownership-cases.json` |
| `test_cpp_framework_location_runtime` | 243개 GTest 중 lease-retention 검사 3개가 실패했다. 아래 상태 값이 예상과 달랐다. | `raw/test-all.log`, `raw/full-test-summary.json` |

Location 실패는 `BothRepositories/StoreOutageRetention.SameTokenMemberWaitsForSpotPerActor/0`의 late_calls와 fresh_calls가 1 대신 0인 경우(`test_cpp_framework_location_runtime.cpp:5192,5194`), `ReviewHeartbeatDoesNotWaitOrRestartExactRead/0`의 outage가 false 대신 true인 경우(`:5044`), `ReviewCutoffTerminatesReadBeforeEpisodeRelease/0`의 cutoff_released가 true 대신 false인 경우(`:5036`)다. 코드 위치는 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/` 기준이다. 이 경로는 수동 scheduler와 lease 상태를 검사한다. 원래 실패를 재실행 통과로 지우지 않는다. 변경 전 main 비교는 아래에 기록했다.

전체 stdout·JUnit·stress log·메모리 표본은 `raw/test-all.log`, `raw/test-all.xml`, `raw/test-all-stress.log`, `raw/test-all-memory.json`에 있다. CTest 내부 로그도 `raw/test-all-ctest-artifacts/`에 복사했다. Compiler를 직접 실행하는 typed JSON negative 검사 12개는 generated CTest의 compiler 인자만 cpp-build-slot wrapper로 바꿨다. 원래 assertion·timeout·label·compiler 인자는 유지했고 진단 log 경로를 `raw/typed-json-compile-fail-*.log`로 옮겼다. 검사는 모두 통과했다. 변경한 generated command는 `raw/test-compiler-wrapper.json`에 있다.

Format 검사는 변경 파일 10개에 `clang-format-18 --dry-run --Werror`를 적용해 통과했다. `git diff --check`도 통과했다. 로그는 `raw/clang-format-check.log`, `raw/clang-format-check-status.json`에 있다.

### Location 실패의 변경 전 대조

초기 main source snapshot을 별도 `build-baseline-tests`에서 같은 linux-ninja-release·test-probe 설정으로 빌드했다. 비교 대상 `test_cpp_framework_location_runtime.cpp`는 현재 worktree와 byte 단위로 같다. Hash와 명령은 `raw/baseline-location*/provenance.json`, `raw/compile-commands-baseline-tests.json`, `raw/configure-baseline-tests.log`에 있다. 테스트 코드는 수정하지 않았다.

| 변경 전 main 대조 | 결과 | 원시 로그 |
|---|---|---|
| 전체 location 실행 파일, stress-ng 8 CPU·80% | 20회 통과했다. 이 통과로 원래 실패를 지우지 않았다. | `raw/baseline-location/iteration-*.log`, `raw/test-baseline-location-status.json` |
| 전체 location 실행 파일, stress-ng 16 CPU·80% | 15번째에서 실패해 중단했다. `ReviewOnlyReadErrorsRetryOnNextRenew/1`이 원래 실패와 같은 공통 assertion `:5044`에서 outage=true였다. Case 이름은 원래 실패와 다르다. | `raw/baseline-location-heavy/iteration-15.log`, `raw/test-baseline-location-heavy-status.json` |
| 원래 실패한 세 case만 선택, stress-ng 16 CPU·80% | 20번째에서 원래 case `SameTokenMemberWaitsForSpotPerActor/0`가 같은 `:5192,5194`에서 late_calls=0, fresh_calls=0으로 실패해 중단했다. | `raw/baseline-location-filtered/iteration-20.log`, `raw/test-baseline-location-filtered-status.json` |

외부 부하가 달라 worker 수가 다른 대조를 따로 기록했다. 전체 unit/contract는 worker 8개에서 실행했고 당시 global load가 14대까지 올라갔다. 변경 전 main 대조의 worker 16개를 동일 환경이라고 표현하지 않는다. 각 대조의 통과·실패와 부하·메모리 기록을 모두 보존했다. 8-worker 대조가 끝난 뒤 16-worker 대조를 시작했으며 두 stress 실행을 겹치지 않았다.

이로써 원래 location case 하나와 같은 공통 outage assertion의 실패가 수정 전에도 존재함을 확인했다. 전체 실행에서 실패한 세 case 각각의 근본 원인까지 확정한 것은 아니다. 이 테스트는 수동 scheduler의 lease·outage 상태를 검사하며 해당 location source와 테스트는 수정하지 않았다. Timeout·기대값·fixture를 바꾸지 않았고 통과한 재실행을 완료 근거로 사용하지 않았다. Location 실패는 남은 실패로 기록한다.

## 5. 측정

조건 B다. Source는 CPU 0–7, target은 8–15다. 1 KiB send-saturation을 main·변경판·binding 순서와 그 역순으로 번갈아 실행했다. 같은 세션에서 saturation 각 5회와 136,889 msg/s 각 3회, 총 24 cell을 수집했다. Warmup은 30초·10 segments다. Saturation active는 5초, paced active는 20초다. CPU capture는 각각 2초와 12초다. Paced는 기존 `CSEND_PACE`의 64건 단위 `sleep_until`을 세 구현에 동일하게 적용했다. Unpaced에서는 pace와 floor 환경 변수를 해제했다. CPU 비교에는 perf를 켜지 않았다. 별도 추가 profile은 이 표에 포함하지 않는다.

각 cell은 exclusive `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행했다. 시작 전 load 1분 값 4 이하와 MemAvailable 12 GiB 이상을 확인했다. 측정 중 자신의 load 때문에 load 값이 올라가는 것은 배제 조건으로 삼지 않았다. 대기·시작 조건은 `raw/measure-wait.jsonl`, `raw/measure-session.json`과 각 cell의 `commands.json`에 기록했다. 측정 도중 메모리는 매초와 capture snapshot마다 확인했다. 모든 thread의 source/target affinity 범위도 snapshot에서 검증했다.

### 같은 rate의 CPU 비용

`/proc/<pid>/stat`의 전체 process utime·stime 차이를 사용한다. CLK_TCK는 100이다. Source의 분모는 capture 전후 completed 차이, target은 received 차이다. 식은 `Δticks / CLK_TCK × 1,000,000 / Δmessages`다. 아래 중앙값은 각 비용 열의 3회 중앙값이다. 단위는 µs/message다.

| 구현 | 회차 | msg/s | Source user | Source kernel | Target user | Target kernel |
|---|---:|---:|---:|---:|---:|---:|
| main | 1 | 136,888.6 | 5.297 | 3.821 | 9.410 | 3.822 |
| main | 2 | 136,888.6 | 5.248 | 3.791 | 9.613 | 3.821 |
| main | 3 | 136,888.6 | 5.009 | 3.655 | 9.535 | 3.384 |
| main | 중앙값 | 136,888.6 | **5.248** | **3.791** | **9.535** | **3.821** |
| 변경판 | 1 | 136,888.6 | 4.190 | 2.907 | 8.620 | 2.769 |
| 변경판 | 2 | 136,888.6 | 3.958 | 2.816 | 8.284 | 2.629 |
| 변경판 | 3 | 136,888.6 | 3.914 | 2.748 | 8.519 | 2.585 |
| 변경판 | 중앙값 | 136,888.6 | **3.958** | **2.816** | **8.519** | **2.629** |
| binding | 1 | 136,888.6 | 1.528 | 1.071 | 1.329 | 1.023 |
| binding | 2 | 136,888.6 | 1.534 | 1.057 | 1.311 | 1.009 |
| binding | 3 | 136,888.6 | 1.545 | 1.020 | 1.376 | 0.990 |
| binding | 중앙값 | 136,888.6 | **1.534** | **1.057** | **1.329** | **1.009** |

Source user는 5.248에서 3.958으로 1.289 µs/message, 24.57% 감소했다. Source kernel은 0.975, target kernel은 1.192 감소했다. Target user도 1.015 감소했다. Source user와 양쪽 kernel이 함께 줄어든 결과지만 각 수정의 인과 효과를 개별 실험한 수치로 해석하지 않는다.

이번 main 중앙값 기준으로 필요한 source user 절감은 2.748 µs/message다. 실제 절감은 1.289다. Source user 고정 예산 2.500에 대해 변경판은 **over budget by 1.458 µs/message**다. 이 세션의 binding source user + 1.000 예산은 2.534이며 초과는 1.424다. Source user만을 binding ÷ 0.6으로 비교하면 예산은 2.557, 초과는 1.401다. 예산을 달성하지 못했다.

### Unpaced saturation, RUNS=5

F÷B는 throughput의 Framework/binding 비다. 중앙값 행은 각각 5회 throughput 중앙값의 비다. 여기서 µs/message는 CPU 합이 아니라 throughput의 역수인 경과시간이다. 예산은 `binding 경과 µs/message ÷ 0.6`이다. 각 회차의 초과는 같은 번호 binding과 비교한다.

| 회차 | Binding msg/s | Main msg/s | 변경판 msg/s | Main F÷B | 변경판 F÷B | 변경판 over-budget µs/message |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 595,094.5 | 181,026.5 | 185,361.4 | 0.3042 | 0.3115 | 2.594 |
| 2 | 634,597.1 | 179,529.4 | 186,743.1 | 0.2829 | 0.2943 | 2.729 |
| 3 | 637,209.5 | 171,733.2 | 190,653.4 | 0.2695 | 0.2992 | 2.630 |
| 4 | 631,112.4 | 178,762.5 | 187,147.1 | 0.2832 | 0.2965 | 2.703 |
| 5 | 586,710.3 | 177,248.4 | 189,497.6 | 0.3021 | 0.3230 | 2.436 |
| 중앙값 | 631,112.4 | 178,762.5 | 187,147.1 | **0.2832** | **0.2965** | **2.703** |

| 구현 | 경과 µs/message | Binding ÷ 0.6 예산 | Over budget by µs/message |
|---|---:|---:|---:|
| main | 5.594 | 2.641 | 2.953 |
| 변경판 | 5.343 | 2.641 | 2.703 |

변경판 throughput은 main보다 4.69% 증가했다. F÷B 0.2965는 목표 0.6에 미달한다. 경과시간 예산 2.641에 대해 **over budget by 2.703 µs/message**다. Source 최적화로 줄지 않은 target user 비용도 남아 있다. 이번 source 수정만으로 throughput 목표를 충족했다고 판단하지 않는다.

### 원시 결과와 집계 검사

원시는 `raw/runs5/{main,change,binding}-{1..5}/`와 `raw/paced/{main,change,binding}-{1..3}/`에 있다. 각 directory에 bench JSON, target stats, stdout·stderr, 실행 명령, capture 전후 `/proc` snapshot과 thread timeline이 있다. 바이너리·package SHA-256은 `raw/measure-session.json`과 `raw/build-provenance.json`에 있다. 전체 수치와 재집계는 `raw/summary.json`, `scripts/summarize.py`를 사용한다.

24개 모두 제출·완료·수신 수가 같고 source/target 오류·abandoned·rejection·header validation failure가 0이다. Drain bound와 contamination도 없다. Paced 9개는 요청 rate의 1% 안에 있다. 집계 검사는 정상 24개를 받아들였고 오류·abandoned·header failure·contamination·drain bound·제출 수·target 수신 수·target error·target rejection·0 throughput을 각각 넣은 잘못된 입력 10개를 모두 거부했다. 결과는 `raw/summary.json`의 `negative`에 보존한다.

소유 계층은 기존과 같다. Codec은 wire bytes와 검증을, mesh owner는 대상 분류와 public completion을, binding은 native payload 보관·physical admission·DONTWAIT 재제출을 소유한다.

Spec 근거는 08-messaging-hot-path §3 E1–E4, 05-payload-ownership-and-codec §1·2·4, 01-submit-and-completion §4·5·10·11과 02-channel-messaging §8이다.

교차언어 대조는 .NET·JVM(Java/Kotlin)·Node.js의 기존 encoding/submission 소유를 읽어 확인했다. C++를 포함한 네 구현의 소유를 대조했다. Wire·error·payload 수명 계약을 바꾸지 않았다. 다른 언어 구현은 수정하지 않았으며 네 언어 contract 실행은 하지 않았다. C++의 기존 wire golden·contract 검사는 실행했다.

변경 분류는 B다. 공개 계약 변경 없는 C++의 중복 표현·계산 제거다. 최종 frame의 순차 소유 표현은 8→2, header measure/write 구현은 2→1, cache hit 조건은 1→2, 센 header/writer 판정·구현 위치 합계는 9→9, 정상 즉시 제출의 명시적 Framework mutex 소유자는 2→2다. Cache 조건 증가도 숨기지 않았다.

리팩토링 점검은 10항목 중 7항목을 제거·통합했고 3항목은 기존 계약 소유를 유지했다. 새 Runtime queue·lock·flag·counter는 없다. 유지·제거 이유와 위치는 3절 표에 있다. 남은 실패는 oversized multipart의 Core/binding 경계와 location lease 검사다. 사용자 지시에 따라 Core·binding·spec과 GitHub는 변경하지 않았다.

Source user와 F÷B 예산은 달성하지 못했다. 추가 helper 하나의 제거로 남은 예산을 닫을 근거가 없어 더 작은 hotspot 수정을 쌓지 않았다. 최종 Framework 변경은 `change.diff`에 저장했다.

최종 바이너리 6개와 native package 2개의 SHA-256은 측정 당시 manifest와 모두 일치했다. 네 지정 빌드의 compile commands에는 `-g`가 없고 CMAKE_BUILD_TYPE은 Release다. 최종 csrc 크기는 9.3 GiB이며 원시 출력은 삭제하지 않았다. 최종 확인은 `raw/final-artifacts.json`, `raw/final-git-status.txt`, `raw/final-diff-check.log`에 있다. Engine connector 테스트가 worktree에 만든 `.run` 출력 5개도 `raw/full-suite-engine-server-run/`으로 옮겨 보존했다. Commit은 하지 않았다.
