# #3868 C++ Framework target 수신 user CPU 귀속

## 범위와 정규화

기존 `csprof2/cpp-framework-136889-fp-r{1,2,3}/capture/target.perf.data`를 `csendb/build-main/bench_cpp_framework_server` 심볼로 읽었다. `perf report --no-children`와 `perf script`의 사용자 모드(`U`) 표본만 사용했다. 커널 모드 표본은 제외했다. 함수 값은 exclusive leaf 표본의 비율이다.

각 스크립트의 `U` 표본 수는 r1 143,529, r2 142,179, r3 143,619이며 총 429,327개다. 모든 표본 period가 200,040으로 같음을 확인했다. 함수 또는 stage 비용은 `9.535 µs/msg × 해당 표본 수 / 429,327`로 환산했다. 따라서 stage 합은 측정된 main target user 9.535 µs/msg와 일치한다. 이 방법은 표본 비율을 실측 CPU에 정규화하므로 profile 회차의 실측 CPU 평균 9.550 µs/msg와 약간 다르다. 최근 source 변경 회차의 target 8.519 µs/msg에는 대응하는 target profile이 없어서 그 값으로 함수별 재정규화하지 않았다. `csrc/report.md`와 `csprof2/report.md`가 지시한 본체와 `csendb/main-source`의 C++ receive 경로는 동일한 것으로 확인했으며 target-side receive 변경은 찾지 못했다.

표의 `spec`는 `framework/doc/framework/common/spec/server` 아래 공통 규격의 조항이다. 바인딩 여부는 같은 벤치의 C++ binding target 경로와 비교했다. Framework 코드는 `csendb/main-source/framework` 기준 상대 경로로 썼다. `libzlink` 내부 구현 원본은 허용된 읽기 범위 밖이라 파일·행을 확인하지 않았다.

## 1. 함수별 exclusive 귀속

`µs/msg`는 세 target profile에서 나온 user leaf 표본을 합산해 9.535에 정규화했다. 함수 이름이 같은 서로 다른 clone/overload는 별도 행으로 남겼다. `기타 leaf`는 나머지 모든 함수 심볼을 포함하며, `[unknown]`은 함수 이름을 해석하지 못한 표본이다.

| 함수 또는 exclusive 심볼 | 파일:행 | µs/msg | 존재 이유 / spec | binding receive 경로 |
|---|---|---:|---|---|
| `[unknown]` | 심볼 미확인 | 0.279 | 호출자를 확정할 수 없음 / 없음 | 불명 |
| `malloc` | glibc | 0.250 | user allocator / 없음 | 예, receive/runtime 할당 일부 |
| `_int_malloc` | glibc | 0.209 | allocator 내부 할당 / 없음 | 예, 일부 |
| `cfree` | glibc | 0.189 | user free / 없음 | 예, 일부 |
| `route_handler_invoker_t::invoke` coroutine actor clone | `runtime/channels/route_handler_invoker.cpp:10` | 0.182 | 필터·scope·handler·completion 순서 실행 / hot path §4.3 W1–W4 | 아니오 |
| `__memcmp_avx2_movbe` | glibc | 0.181 | byte range 비교. 구체 key 용도는 leaf만으로 판별 불가 / 없음 | 예, 비교 일부 |
| `raw_mesh_node_owner_t::receive_next` | `runtime/mesh/raw_mesh_node_owner.cpp:3602` | 0.181 | 수신 record 분류와 application mailbox 투입 / hot path §4.1 I1–I4 | 아니오 |
| `route_packet_dispatcher_t::dispatch_send` | `runtime/channels/route_packet_dispatcher.cpp:203` | 0.174 | send 종류 분기와 handler 실행 연결 / hot path §4.3 W1–W3 | 아니오 |
| `service_mailbox_record_t` move constructor | `runtime/mesh/service_mailbox.hpp:146` (암시적) | 0.164 | record·소유권을 mailbox로 이동 / hot path §4.1 I4 | 아니오 |
| `public_host_runtime_t::dispatch_application_claim` | `runtime/stateful/public_host_runtime.cpp:8108` | 0.142 | claim record를 owner 실행으로 전달 / hot path §4.1, §4.3 | 아니오 |
| `shared_ptr::_M_release` | libstdc++ | 0.136 | 공유 claim·runtime·terminal 수명 정리 / 해당 수명 규칙, 구현 세부는 없음 | 아니오 |
| `zlink::message_t` move constructor | `libzlink` 바이너리 | 0.126 | 수신 frame 소유권 이전 / payload ownership §4 | 예, raw received frame 이동 |
| `zlink_router_recv` | `libzlink` 바이너리 | 0.123 | ROUTER record 수신 / Core·binding 경계, hot path §1 | 예 |
| `pthread_mutex_lock` | libc | 0.123 | mailbox·runtime 상태 보호 / 동기화 계약, 구체 lock은 없음 | 예, Core 쪽 일부만 |
| `envelope_header_t` destructor | `runtime/messaging/envelope_codec.hpp:145` | 0.118 | parsed header 필드 정리 / payload ownership and codec §6 | 아니오 |
| `_int_free` | glibc | 0.111 | allocator 내부 해제 / 없음 | 예, 일부 |
| `__memmove_avx_unaligned_erms` | libc | 0.111 | 메모리 이동; 이 leaf만으로 복사 대상은 판별 불가 / 없음 | 예, 일부 |
| `mesh_record_dispatcher_t::dispatch` | `runtime/mesh/mesh_record_dispatcher.cpp:32` | 0.108 | 이미 parse된 header·parts를 route dispatcher로 전달 / hot path §4.3 | 아니오 |
| `envelope_header_t` move constructor | `runtime/messaging/envelope_codec.hpp:145` | 0.106 | parsed header를 다음 단계에 전달 / payload ownership §4 | 아니오 |
| `zlink::msg_t::close` | `libzlink` 바이너리 | 0.105 | frame buffer 수명 종료 / payload ownership §4 | 예, receive 이후 정리 |
| `std::string` move constructor | libstdc++ | 0.105 | header·routing metadata 이동 / 없음 | 예, 일부 metadata |
| `route_handler_invoker_t::invoke` 일반 clone | `runtime/channels/route_handler_invoker.cpp:10` | 0.103 | async handler 호출과 결과 정리 / hot path §4.3 | 아니오 |
| `application_dispatch_terminal_owner_t` move constructor | `runtime/mesh/mesh_node_host_service.cpp:259` | 0.098 | 완료·mailbox 정리 권한의 단일 소유 이전 / Submit과 완료 §10, hot path §4.3 | 아니오 |
| `recv_whole_envelope` | `libzlink` 바이너리 | 0.093 | binding이 수신 record와 frame을 완성 / Core·binding 경계, hot path §1 | 예 |
| `canonical_header_reader_t::string` | `runtime/messaging/envelope_codec.cpp:640` | 0.083 | envelope header 문자열 필드 parsing / payload ownership and codec §6 | 아니오 |
| `header_sax_t::finish` | `runtime/messaging/envelope_codec.cpp:938` | 0.081 | header 값에서 typed header 구성 / payload ownership §6 | 아니오 |
| `std::shared_count` copy constructor | libstdc++ | 0.080 | 공유 owner 참조 수명 / 없음 | 아니오 |
| `mesh_node_host_service_t::dispatch_application` | `runtime/mesh/mesh_node_host_service.cpp:3742` | 0.079 | host claim 이후 수신 handler 처리 경로 / hot path §4.3 | 아니오 |
| `decode_header_bytes<true>` | `runtime/messaging/envelope_codec.cpp:1196` | 0.078 | application envelope header decode / payload ownership §6 | 아니오 |
| `zlink_msg_size` | `libzlink` 바이너리 | 0.074 | frame 크기 확인 / Core·binding 경계 | 예 |
| `pthread_mutex_unlock` | libc | 0.071 | 잠금 해제 / 동기화 계약, 구체 lock은 없음 | 예, Core 쪽 일부만 |
| `application_job_queue_t::permit_t` destructor | `runtime/dispatch/application_job_queue.hpp:157` | 0.068 | handler 진입·terminal에서 permit 반환 / Application job queue 규격 | 아니오 |
| `raw_mesh_node_owner_t::enqueue_received` | `runtime/mesh/raw_mesh_node_owner.cpp:3286` | 0.067 | 수신 record를 mailbox에 넣고 admission 통지 / hot path §4.1 I4 | 아니오 |
| `service_provider_t::resolve` | `runtime/configuration/services.cpp:290` | 0.063 | handler scoped service resolve / hot path §4.3 W3, scope 규칙 | 아니오 |
| `_Hash_bytes` | libstdc++ | 0.062 | hash key 조회 / 없음 | 예, 일부 |
| `unlink_chunk` | glibc | 0.062 | allocator 해제 / 없음 | 예, 일부 |
| `make_route_message_context` | `runtime/channels/route_packet_dispatcher.cpp:25` | 0.059 | handler에 전달할 source·packet·metadata context 구성 / hot path §4.3 W3 | 아니오 |
| `_int_free_merge_chunk` | glibc | 0.057 | allocator 해제 / 없음 | 예, 일부 |
| `__libc_recv` | libc | 0.056 | native socket read / Core·binding 경계 | 예 |
| `decode_mesh_record_header_impl<true>` | `runtime/mesh/mesh_node_host_service.cpp:118` | 0.055 | owner dispatch 전에 application header decode / payload ownership §6 | 아니오 |
| `기타 leaf 함수` | 여러 곳 | 4.926 | 0.05 µs/msg 미만 함수 및 나머지 stack leaf의 합 / 함수별 spec은 원시 symbol 참조 | 혼합 |

이 표의 함수 값은 stack leaf만 세었다. 즉 함수 안쪽 자식 시간을 다시 부모에 더하지 않았다. 예를 들어 `malloc` 계열 샘플은 stage 값 안에 이미 포함되며 별도 비용으로 stage 합에 더하지 않는다. `기타 leaf`는 그 함수들까지 포함해 총 9.535가 되도록 계산했다. 원시 symbol 목록과 표본 수는 `raw/r{1,2,3}-target-user-attribution.json`, self report는 `raw/r{1,2,3}-target-user-exclusive.txt`에 있다.

## 2. stage별 user CPU

framework stage mapping은 `csprof2/scripts/rules.py`의 기존 leaf-to-caller 분류 규칙을 사용자 표본에 적용했다. Framework receive dispatch는 profile의 `dispatch/coroutine/pump`와 `mailbox enqueue/claim`을 합쳤다. Core/binding receive는 native receive·decoder leaf와 수신 전용 Core boundary다.

| 요청 stage | µs/msg | 근거와 해석 |
|---|---:|---|
| Core/binding receive | 2.047 | native ROUTER receive, Core receive 및 Framework port 경계. binding 실측 1.329보다 0.718 높다. |
| Framework receive dispatch: poller·lane handoff·mailbox | 3.918 | pump/dispatch 2.912 + mailbox enqueue/claim 1.007. host dispatch와 application executor로 보내는 관리 비용이다. |
| Envelope/header decode | 1.041 | 기존 `wire decode` user 표본. 대부분 application envelope/header parser와 service-wire header reader다. |
| Payload decode/serializer | 0.000* | typed protobuf decode 호출은 존재하지만 별도 exclusive leaf가 식별되지 않았다. profile의 inline·unknown 비용과 섞여 있어 실제 decode가 0이라는 뜻이 아니다. 그 CPU를 임의로 header stage에 더하지 않았다. |
| Routing to handler | 1.357 | topology·record/route kind·handler registry lookup·context 구성. |
| Handler and bench | 0.098 | handler 0.049 + target metrics/bench 0.049. `command_handler_t::handle`은 payload body를 metrics에 기록한다. |
| Completion/metrics | 0.397 | terminal, permit 및 mailbox settlement 소유자의 user CPU. 위 target benchmark polling은 별도 handler/bench 행에 넣었다. |
| Other | 0.678 | unattributed 0.666 + send submit 0.004 + encode 0.008 + 반올림 전 native send 약 0.000. |
| **합계** | **9.535** | 동일한 user profile leaf 표본을 측정치에 정규화한 합계. |

Payload는 Framework가 `.deserialize(...)`로 typed `BenchPayload`를 만들고 나서 handler에 전달한다(`runtime/channels/route_handler_registry.hpp:104-115`). 이 동작은 binding 경로도 `BenchPayload::ParseFromArray`로 한 번 수행한다(`framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:95-99`). 그래서 그 decode를 Framework 전용 중복 작업으로 세지 않았다. 별도 parser symbol이 보이지 않아 CPU split은 profile만으로 확정할 수 없다.

## 3. Binding 비교와 중복 검사·handoff

Binding receive 경로가 하는 일은 `router.recv`/`zlink_router_recv`, native envelope/frame 수신, payload protobuf decode 한 번, metrics 기록이다(`bench_zlink_cpp_server.cpp:80-100`). Binding은 application envelope/header parsing, topology와 handler lookup, scoped DI, service mailbox claim/settlement, coroutine handler wrapper, typed result·terminal 정리를 하지 않는다. 그래서 사용자 CPU 차이는 단순히 protobuf decode 차이가 아니다. 측정 기준을 구분하면, profiled main target 9.535와 binding 1.329의 차이는 8.206 µs/msg이고, source 변경 이후 target 8.519와 binding의 차이는 사용자가 말한 7.190 µs/msg다. 2.3 예산까지 필요한 감축은 profiled 9.535 기준 7.235, source 변경 이후의 8.519 기준 6.219 µs/msg다. 후자의 target profile이 없으므로 아래 stage 귀속은 9.535 profile 기준이다.

반복 검사와 handoff 중 코드로 확정되는 항목은 다음과 같다.

- `raw_mesh_node_owner_t::receive_next`에서 service-wire header를 읽어 command·peer·owner를 분류한다(`raw_mesh_node_owner.cpp:3644-3654`). 이후 application envelope header는 `decode_mesh_record_header_view`가 한 번 decode한다(`mesh_node_host_service.cpp:3778-3793`). 결과 header는 `spot_node_runtime_t`와 `mesh_record_dispatcher_t`를 거쳐 move로 전달되며 같은 경로에서 다시 parse하지 않는다(`mesh_record_dispatcher.cpp:32-71`). `route_packet_dispatcher_t::dispatch(received)`의 별도 overload는 자체 decode하지만 이 receive 경로에서는 이미 header를 받는 overload를 호출한다(`route_packet_dispatcher.cpp:153-171`).
- `enqueue_received`의 flow tracing 활성 분기는 service-wire header, application payload, envelope header를 다시 읽는다(`raw_mesh_node_owner.cpp:3292-3304`). 이 application envelope header는 이후 host dispatch에서 다시 읽힐 수 있어 tracing 활성 시 중복 decode다. 기본 profile의 flow tracing은 비활성이라 이 조건부 비용을 현재 9.535에서 빼거나 savings에 넣지 않았다.
- `decode_header_bytes`는 canonical JSON reader를 먼저 시도하고 실패할 때 general SAX parser로 재시도한다(`envelope_codec.cpp:1204-1211`). 정상 canonical header에서는 첫 파서에서 끝난다. 비canonical-but-valid header에서는 동일 bytes를 두 parser가 볼 수 있다.
- queue 이동은 copy가 아니라 다수 move다. `receive_next`에서 `service_mailbox_record_t`로 parts를 move한다(`raw_mesh_node_owner.cpp:4221-4229`), `try_enqueue`가 record를 queue에 move한다(`service_mailbox.cpp:79-115`), claim은 record 주소를 잡고 `take`가 원본을 move한다(`service_mailbox.cpp:172-215, 264-271`). profile은 record/message/header move constructor 비용을 보였지만 payload 전체를 복사했다고 증명하지 않는다. `memmove` leaf 0.111 µs/msg도 호출 목적을 식별하지 못해 payload copy라고 단정하지 않았다.
- mailbox claim은 매 batch `make_shared<service_mailbox_claim_t>`와 records·record_bytes·settled storage를 만들고 각 record에 settlement credit을 준다(`service_mailbox.cpp:186-215`). 이후 shared claim handle이 record별 take/settlement까지 살아 있다. owner 문자열과 source routing id, `message_context`도 단계 사이에 복사 또는 재구성되는 경로가 있다(`service_mailbox.cpp:94-102`, `route_packet_dispatcher.cpp:25-38`).
- 수신 역할에서 application 실행으로 handoff가 하나 보인다. `mesh_node_host_service_t`가 terminal/permit을 만든 뒤 dispatch하고(`mesh_node_host_service.cpp:3755-3793`), `route_handler_invoker_t`가 coroutine으로 handler와 completion을 실행한다(`route_handler_invoker.cpp:22-50, 63-95`). 규격 hot path §4.4가 허용하는 역할 인계는 최대 한 번이다. 기존 profile로 record당 OS thread 전환 횟수를 세지는 않았으므로 추가 handoff가 있다고 단정할 수 없다.
- `std::function`, coroutine frame, `result_t` wrapping은 per-message receive path에서 확인된다. `route_packet_dispatcher.cpp:292-309`는 callback·completion closure를 만들고 header/route state를 capture한다. `route_handler_invoker.cpp:10-20, 35-50`은 coroutine task와 scoped invocation을 구성하며 `result_t`를 만든다. 성공 경로도 wrapper가 있지만 이것이 중복 의미인지, 정확한 frame 수가 얼마인지는 profile에서 분리되지 않았다.
- mutex와 shared_ptr 비용은 단일 원인이 아니다. mailbox `try_enqueue`와 claim이 mutex를 잡고(`service_mailbox.cpp:79-90, 118-163`), `receive_next`가 lifecycle mutex로 port를 읽는다(`raw_mesh_node_owner.cpp:3605-3609`). 샘플에서 mutex lock+unlock leaf 합은 약 0.194 µs/msg다. shared_ptr refcount leaf 두 종류는 `_M_release` 0.136, shared count copy 0.080 µs/msg다. 이 비용은 여러 owner 수명 구간의 합이며 전부 한 lock 또는 한 shared_ptr에 귀속하지 않았다.
- `application_dispatch_terminal_owner_t` move와 `permit_t` 해제는 각기 handler 실행 권한 및 완료 예약을 정확히 한 번 정리하는 계약을 지킨다. 이 bookkeeping은 프로파일에서 보이며 단순 제거할 수 있는 중복 호출로 분류하지 않았다.

## 4. .NET header reuse 비교

C++은 `.NET`과 같은 decoded-header cache를 갖지 않는다. C++ `envelope_codec_t::decode_header_view`는 zero-copy text view를 만들지만 각 호출마다 `decode_header_bytes<true>`와 새 `envelope_header_t`를 만든다(`runtime/messaging/envelope_codec.cpp:1196-1218, 1238-1263`). 이 receive path에서는 정상 경로 header를 한 번 파싱해 handler dispatch까지 넘기므로 현재 확인된 재파싱은 tracing 조건부 경로다. 반복되는 byte-identical header 사이에서 C++ cache reuse는 없다.

현재 snapshot의 .NET `ZLinkEnvelopeCodec.cs:377-411`은 `FindDecodedHeaderCacheEntry` 조회를 먼저 하고 cache miss면 decode한 뒤 제한 조건을 만족하는 header를 cache에 넣는다. 대상은 correlation/deadline/flow/metadata가 없는 반복 가능한 header다. cache는 최대 64 entries이며 header bytes를 복사해 보관한다(`:1385-1428`). 반면 사용자가 함께 적은 `:188`은 decoded cache가 아니라 단순 outbound header encoding 최적화 설명이다(`:183-205`). 그래서 receive reuse 근거는 `:377` onward이며 `:188`은 encoding 비교에만 해당한다.

C++에서 이 cache 형태를 추가했을 때의 이득은 profile만으로 확정할 수 없다. header cache는 동일한 단순 bytes가 반복된다는 전제에서 decode 작업을 줄이지만 bytes 비교·copy와 동기화 비용을 만든다. 현 C++ 경로가 이미 한 번 parse 후 전달한다는 사실 때문에 기대 savings를 전체 1.041 µs로 잡을 수 없다.

## 5. 후보 제거와 2.3 µs 목표

아래 수치는 새 측정치가 아니다. existing profile에서 각 stage 또는 관련 leaf가 차지한 **절대 상한**과 실제 제거 가능성이 있는 일부 구간을 구분했다. 상한은 서로 겹치므로 더하지 않는다.

| 후보 | 현재 근거 | 기대 savings 해석 |
|---|---|---:|
| application receive pump·coroutine·dispatch wrapper 경로 축소 | `dispatch/coroutine/pump` 2.912 µs/msg. 주요 exclusive 함수는 두 `route_handler_invoker::invoke` clone 합 0.285, `dispatch_application_claim` 0.142, `mesh_record_dispatcher::dispatch` 0.108, route `dispatch_send` 0.174다. | 전체 stage 제거 불가능. 다단 wrapper/frame/closure를 통합하는 경우 **0.5–1.5**가 우선 후보이며, 2.912는 stage 전체 상한이다. 상한은 추정이지 예측 보장이 아니다. |
| mailbox claim/settlement과 owner record 보관 단순화 | mailbox stage 1.007. `service_mailbox_record` move만 0.164이며 claim은 `shared_ptr`, record index vectors, atomic settlement을 할당한다. | batch handoff와 기존 ordering·settlement을 유지하면서 **0.3–0.8** 후보. 1.007은 제거 절대 상한이다. |
| route classification/handler 선택 사전 준비 | route stage 1.357. topology peer·record kind·handler registry·message context를 message마다 수행한다. | 불변 route/key와 호출 closure를 등록 시점에 준비하면 **0.2–0.6** 후보. profile stage 전체는 상한이다. |
| allocation/refcount/mutex 감소 | 식별된 allocator leaf 합은 상위 allocator symbols 기준 약 0.88, mutex lock+unlock 0.194, shared_ptr release+count copy 0.216 µs/msg다. 서로 다른 stage에 이미 포함된 leaf라 별도 stage savings로 추가하지 않는다. | 풀·inline storage·shared ownership 경로 축소로 **0.2–0.6** 후보. allocator 합 0.88은 framework 전용이 아니어서 모두 제거 가능한 savings가 아니다. |
| decoded header reuse 검토 | `wire decode` 1.041. canonical parser, header construction, `decode_mesh_record_header_impl` leaf들이 있다. 기본 route receive에서는 parse 결과를 재사용한다. | cache가 없애는 비용의 이론상 상한은 header에 직접 귀속된 일부 decode leaf 약 **0.2–0.4**다. 단순 반복 header hit와 cache lookup 비용을 profile에서 검증할 수 없으므로 기대치를 확정할 수 없다. |
| flow tracing 경로 decode 중복 제거 | flow tracing 활성일 때만 `enqueue_received`가 protocol/application/envelope header를 추가 decode한다. | 현재 profile의 savings는 **0.000**이다. tracing 활성 시 별도 비용 후보이나 이번 9.535 기준에서 제거 효과로 계산하지 않았다. |
| Core/binding receive 차이 검토 | Core/binding stage 2.047 대 binding target user 1.329, 즉 profile 분류 차이 0.718. | Framework 외부 Core 경계를 포함하므로 source만으로 절감량을 지정할 수 없다. 0.718은 단순 비교 차이지 제거 가능한 상한이 아니다. |

2.3 µs 목표에 도달하려면 profiled main 9.535에서 **7.235 µs/msg**, 또는 source 변경 이후 8.519에서 **6.219 µs/msg**를 줄여야 한다. binding 1.329보다 허용하는 framework 초과분은 0.971뿐이다. 지금 profiled Core/binding 이외 stage가 약 7.488 µs/msg이므로 그 대부분을 줄이고 handler·한 번의 typed payload decode·정확한 terminal 처리·필수 단일 owner handoff 비용만 남기는 규모의 receive architecture 간소화가 필요하다. 위 각 후보의 중간 범위를 합해도 이 목표를 보장하지 않으며, 보수적으로 계산해도 현재 evidence만으로 2.3 달성이 입증되지는 않는다.

## 재계산 원시 자료

- `raw/r{1,2,3}-target-user-exclusive.txt`와 각 `.stderr`, `.status`: `perf report --no-children` 결과.
- `raw/r1-target-samples-misc-period.txt`와 상태 파일: 사용자/커널 mode 및 period 확인.
- `raw/r1-target-full-script.txt`, `raw/r{2,3}-target-full-script.txt.gz`: 전체 callchain. gzip 파일도 그대로 `gzip -cd`로 읽을 수 있다.
- `raw/r{1,2,3}-target-user-attribution.json`: 사용자 표본별 symbol·stage 집계.
- `raw/analyze_user_stacks.py`: 위 callchain에서 사용자 표본만 다시 분류하는 스크립트.
