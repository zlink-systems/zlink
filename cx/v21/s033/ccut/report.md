도달 판정은 미확정이다. 현행 계약을 지키는 6.513 µs 설계 예산은 구성할 수 있으나, 기존 표본만으로 ≤6.6 µs의 예상 성능을 입증할 수 없다.
추천안은 public binding request/reply에 Framework logical entry와 typed 결과 전달만 남기는 prepared request 구조다.
PR 1은 admission 변환 task와 결과 관찰 객체를 기존 request 저장소의 view로 대체하며, 목표 절감은 5.376 µs다.
PR 1 뒤 caller 예산은 17.480−5.376=12.104 µs다.
PR 2는 등록된 codec·header plan으로 최종 service message를 구성하며, 목표 절감은 3.791 µs다.
PR 2 뒤 caller 예산은 12.104−3.791=8.313 µs다.
PR 3은 socket 직렬화와 등록 저장 표현을 기존 소유자에 집중하며, 목표 절감은 1.800 µs다.
합계 목표 절감은 10.967 µs이고, 등록 0.650+결과 0.800+encoding 0.650+binding 분류 3.300+기타 1.113=6.513 µs다.
이 숫자는 구현 승인용 비용 예산이며 실측 예측값이 아니다. 잔여 비용을 더 보수적으로 잡은 비교 시나리오는 9.113 µs다.
PM은 대안 B를 선택한 뒤 구현을 승인해야 하며, C1 terminal·deadline·admission·최종 typed 전달의 판정자는 각각 1개를 유지한다.

**판정의 근거와 자료 범위**

독자는 다음 구현을 선택할 PM이다. 이 보고서는 Framework가 binding 기능을 재사용하면서 어떤 중간 표현을 없앨 수 있는지와, ≤6.6 µs 주장에 부족한 증거를 설명한다. 코드 수정 제안의 분류는 계약을 유지하는 A 또는 B이고, 계약을 삭제하는 대안은 D로 따로 표시한다. 구현 승인은 하지 않았다.

경로의 `F/`는 `/root/worktree/zlink-3867h/framework/languages/cpp/framework/`, `B/`는 `/root/worktree/zlink-3867h/bindings/cpp/`, `S/`는 `/root/worktree/zlink-3867h/framework/doc/framework/common/spec/server/`를 뜻한다. `bench/`는 저장소의 `framework/bench/grpc/cpp/`다. `self`는 leaf 표본 비용이고 `I`는 하위 호출을 포함한 비용이다. I 값들은 중첩되므로 더하지 않는다. 비용의 단위는 모두 caller CPU µs/request다. Target과 source completion 자원에서만 실행되는 일에는 caller 비용 0과 별도 자원의 미계측을 함께 표시한다.

사용한 기준은 [ccall 보고서](../ccall/report.md), 그 보고서의 `profile-summary.json`, caller function CSV와 `source.samples.json`, [csites 보고서](../csites/report.md), [crtc 보고서](../crtc/report.md), [cprof 보고서](../cprof/report.md)다. ccall의 Framework caller 19.682 µs를 사용자 기준 17.480 µs로 정규화했다. Binding caller는 3.685 µs다. Framework caller 표본은 1,225개여서 작은 함수의 0.014 µs 차이를 정밀한 함수 시간으로 해석하면 안 된다. 다른 실행에서 얻은 csites의 할당 횟수를 ccall 함수별 시간과 곱하지 않았다.

| 배타적 표본 분류 | Framework | Binding | 차이 |
|---|---:|---:|---:|
| 등록 | 4.4235 | 0.0149 | 4.4086 |
| 결과 관찰 | 4.2523 | 0.0471 | 4.2052 |
| Encoding | 2.1547 | 0.1390 | 2.0157 |
| Binding submit으로 분류된 표본 | 5.5365 | 2.5162 | 3.0203 |
| 기타 | 1.1130 | 0.9678 | 0.1452 |
| 합계 | 17.4800 | 3.6850 | 13.7950 |

이 분류는 호출 구간을 계측한 stopwatch가 아니다. `ccall/scripts/summarize.py:7–32`는 leaf부터 처음 만나는 codec/task/binding/registry symbol로 표본을 분류한다. 예를 들어 `message_t::allocate`의 상위 stack이 잘리면 Framework header 할당도 binding 분류에 들어간다. 반대로 header writer가 인라인되어 부모만 남으면 등록 분류로 들어간다. Binding 분류에는 completion drain도 들어 있다. 따라서 5.5365를 `zlink_request()` 한 호출의 시간으로 보거나, 4.4235를 registry hash insertion만의 시간으로 볼 수 없다.

[profile-check.json](profile-check.json)은 기존 표본의 배타 합과 아래 재분류를 보존한다. [check_profile.py](check_profile.py)는 기존 자료만 읽으며 합계를 다시 계산한다. 새 build, benchmark, test, Git/GitHub 작업, 다른 모델 호출은 수행하지 않았다. Core C header도 읽지 않았다. 내부 문서 준비 스크립트는 pull·clone·파일 복사를 수행하므로 실행하지 않고 기존 `/root/zlink-docs/zlink/AGENTS.md`를 읽었다. 이번 작업의 쓰기는 이 `ccut/` 안뿐이다. 확인한 MemAvailable은 시작 약 23 GiB, 계산 시 약 22.78 GiB로 12 GiB 이상이다. 연속 계측값은 아니다.

**Binding 기능과 Framework 메커니즘의 대조**

아래 표가 설계 대안의 기준이다. ‘직접 사용’은 public C++ binding API를 호출한다는 뜻이다. Binding의 `detail::completion_owner_t` 등은 기능의 구현 근거이며 Framework에서 직접 호출할 대상이 아니다. 현재 경로가 이미 binding을 호출하는 경우에는 재사용이 완료된 부분과 중복된 표현을 구분했다.

| Framework file:function | 이미 제공하는 binding file:function | 현재 비용 근거 | 직접 사용과 남겨야 할 Framework 계약 |
|---|---|---|---|
| `F/src/runtime/backend/raw_route_port.cpp:351 request_impl`의 `_socket_mutex`와 `:461 receive_if_ready`, `:506 reply`의 같은 잠금 | `B/src/Runtime/Messaging/operation_submit.hpp:128 submit_raw_request_state`, `B/src/Runtime/Sockets/router.cpp:85 request`; binding 계약 `bindings/doc/spec/cpp/README.ko.md:380–385`의 whole-message submit·동시 submit·Core lifecycle gate | `request_impl` self 0.1570/I 0.5422. 모든 caller pthread mutex leaf 합 0.6421이며 이 잠금의 독립 시간은 없음 | Transport submit 직렬화는 중복이다. Public builder 호출로 집중한다. 다만 `_socket` wrapper 수명·close와 공유 `_received` 출력 객체 보호는 Core gate와 다르다. 이 수명까지 입증하기 전 mutex만 삭제하지 않는다. S08 E3–E4와 binding lifecycle 계약을 따른다. |
| `raw_route_port.cpp:383–391 request_impl`의 builder 조립 | `B/src/Runtime/Sockets/router.cpp:85 request`, `B/src/Runtime/Messaging/request_reply.cpp:62 message`, `:102 async` | builder factory self 0.1427; async self 0.1427/I 0.2426 | 이미 public API 사용 중이다. 새 request engine은 만들지 않는다. Framework는 service-wire 두 part를 제공하는 얇은 호출만 남긴다. |
| `F/src/runtime/mesh/raw_mesh_node_owner.cpp:1543 request_with_header`, `F/src/runtime/foundation/operation_registry.cpp:382 register_operation`, `:539 take` | `B/src/Runtime/Messaging/completion_owner.cpp:585 start_async_request`, `:572 insert_entry_locked`, `:628 unregister_entry`의 native pending map | Framework registry self 0.0856/I 0.4852. Binding pending insertion 귀속 0.2283. 각각 다른 표이므로 시간도 판정자도 합치지 않는다 | 단순 중복이 아니다. Binding은 native context·completion id, Framework는 full OperationId·ReplyRouteId·relocation·shutdown을 소유한다. Framework C1은 유지한다. `S/01-execution/01-submit-and-completion.ko.md:392–428,447–463`. |
| 같은 registry의 `_entries` node와 `index_deadline`의 저장소 | Binding pending map의 PMR node 재사용 `B/src/Runtime/Messaging/completion_owner.hpp:121–134` | registry I 0.4852 안에 포함. Framework map node는 요청마다 emplace, deadline 링크 자체는 무할당 | Binding pending map을 Framework C1으로 쓰는 public API는 없다. Framework node 저장소만 기존 allocator를 재사용 가능한 것으로 대체한다. 두 map을 동일 terminal authority로 만들지 않는다. |
| `raw_mesh_node_owner.cpp:1519 request_with_header`의 state lane | Binding은 transport 제출을 Core에 맡기며 Framework C2 lane은 제공하지 않음 | `state_lane_t::schedule_drain` self 0.0713/I 2.6826. 이 I에는 실행된 요청 본문도 포함 | 제거 불가. Registry 등록·deadline index·close admission의 C2 소유자다. caller가 같은 owner의 private method를 호출하면 재진입이 필요 없다. 외부 호출은 기존 FIFO lane을 얻는다. `S/01-execution/06-state-ownership-and-lanes.ko.md:166–176,212–255`. |
| `raw_mesh_node_owner.cpp:225 start_admitted_request`, `:272 run_request_admission` | `B/src/Runtime/Messaging/completion_owner.cpp:279 submit_request_attempt`, `:668 drain_impl`의 WRITABLE 처리와 정확한 target 재시도 | `start_admitted_request` self 0.0571/I 0.8134. Ready 성공 경로에서 readiness 대기 coroutine은 실행하지 않음 | 이미 physical HWM retry는 binding에 위임한다. Framework에 남는 것은 operation 시작 전 logical Ready/descriptor 대기다. 이를 binding WRITABLE과 합치면 안 된다. S01 §5·§9, S08 E2–E4. |
| `raw_binding_adapter.hpp:147 observe_request_completion`, `raw_mesh_node_owner.cpp:1585 await_request_admission`, 상위 `public_host_runtime_t::request_to_node`, `mesh_node_runtime.cpp:4461 submission_observer` | `B/include/zlink/Contracts/Messaging/operation_contracts.hpp:175 request_submission_t`의 result/admitted/reply | observer coroutine self 0.0143/I 0.0713만 분해됨. `await_request_admission` self 0.0713/I 0.1284. 상위 host request self 0.1427/I 3.5531 | Stage 표현의 중복이다. result snapshot을 같은 최종 admission view에 반영하고 pending일 때만 binding admitted를 await한다. bool→submit_result→void task와 관찰 연결을 없앤다. Public admission과 physical reply의 차이는 유지한다. S01 `:331–333,357–365`. |
| `raw_mesh_node_owner.cpp:235` 잔여 timeout, `operation_registry.cpp:488 index_deadline` | `.timeout(ms).async()` → `B/src/Runtime/Messaging/operation_submit.hpp:159–168`, Core reply timeout 계약 `bindings/doc/spec/cpp/README.ko.md:389–392` | deadline 단독 시간 미분리. registry I 0.4852와 binding 분류의 native timeout 경로에 포함 | 의미가 다른 경계다. Framework 진입 전 구간·Ready 대기를 포함한 절대 deadline 하나는 C2 index가 유지한다. Binding에는 E4 시점의 잔여값만 넘긴다. Binding timer를 Framework가 구현하거나 Framework timeout을 매 단계 새로 시작하지 않는다. S01 `:357–365,453–487`. |
| `raw_route_port.cpp:407 poll`, `raw_binding_adapter.hpp:147 observer` | `B/src/Runtime/Eventing/poller.cpp:404 completion_owner->drain`, `B/src/Runtime/Messaging/completion_owner.cpp:668 drain_impl` | caller에서 Framework poll 비용은 별도 미분리. Binding 비교 caller에는 completion consume/drain 0.4591이 이미 포함 | 이미 같은 binding poller를 사용한다. 두 번째 receive loop나 native completion map은 추가하지 않는다. Framework는 permit·관리 deadline을 결합하고 receive 역할을 배치한다. S08 `:181–205,300–324`. |
| `raw_route_port.cpp:467–487 receive_if_ready`의 source RID byte vector 재구성 | `B/include/zlink/Contracts/Messaging/received.hpp`의 `received_t::routing_id/reply_token/parts`, public `routing_id_t` | caller 0, target receive 자원 비용 미계측 | RID를 public 값으로 보유하고 그 값을 reply builder에 직접 전달할 수 있다. 실제 Ready lifecycle generation 검증은 Framework에 유지한다. Native physical route 선택은 binding/Core가 소유한다. |
| 같은 함수의 `std::move(_received.parts())`, `raw_binding_adapter.hpp:136 copy_binding_parts`, `:200 copy_binding_messages` | `B/include/zlink/Contracts/Messaging/message.hpp:45–48,84–89`의 move/copy/clone, `received_t`의 retained native storage | caller의 native allocation 귀속 0.7848은 이 세 helper의 비용이 아님. Target와 source receive 비용 별도 미계측 | 주 application receive는 이미 move하고 close한다. `copy_binding_parts`는 별도 infrastructure 경로에서 사용한다. 활성 성공 경로의 memcpy로 다시 계산하지 않는다. Public native message 소유를 그대로 이동한다. S payload ownership `:19–75`, S01 `:333`. |
| `raw_route_port.cpp:501 reply`의 messages vector→lvalue `.message(messages[i])` | `B/src/Runtime/Sockets/router.cpp:98 reply`, `B/src/Runtime/Messaging/operation_state.hpp:174–212`의 lvalue/rvalue part 이동 | caller 0, target reply 자원 비용 미계측 | Binding lvalue `.message()`도 원본 part를 stage로 이동하고 실패 때 복원한다. 이것을 무조건 deep copy라고 세면 틀린다. 소유한 reply parts를 rvalue로 전달해 원본 복원 연관을 없애되 native 재시도는 binding에 둔다. |
| `raw_route_port.cpp:510`의 RID+reply token 재조립 | `B/include/zlink/Contracts/Messaging/received.hpp`의 reply builder 및 `router_socket_t::reply(RID, token)` | caller 0, target 비용 미계측 | Core의 one-shot reply token을 그대로 사용한다. Service-wire correlation·operation-kind 검증과 reply body 구성만 Framework에 남는다. S01 `:407–428`, S08 `:223`. |
| `F/src/runtime/mesh/mesh_node_host_service.cpp:2123 node_receive_role_t::run`, `:2266` enqueue, `:3805 drain_application_owner` | Public binding `poller_t::wait`와 socket `recv`는 readiness/record만 제공 | caller 0, target 전체 비용을 이 보고서에서 분해하지 않음 | Framework permit·owner queue·scope·execution gate는 중복이 아니다. Binding callback에서 handler를 바로 호출하는 것으로 대체할 수 없다. S08 `:183–193,220–251,309`. |
| `F/src/runtime/channels/route_packet_dispatcher.cpp:348 dispatch_request`, `:416` invocation completion, `route_handler_invoker.cpp:10 invoke`, `channel_reply_writer.cpp:12 write` | Binding async request/reply는 typed handler/filter/DI scope를 제공하지 않음 | caller 0. Target 별도 µs 미계측 | Framework 고유 계약이다. #3957이 invocation 저장 표현·reply header view를 이미 개선했다. 해당 개선을 새 절감으로 재계상하지 않는다. S08 `:220–245,253–282`. |
| `F/src/runtime/mesh/raw_mesh_node_owner.cpp:1369 observe_request`의 reply frame 검증·service correlation 비교 | Binding completion은 native request에 reply를 연결하고 `async_result_t<vector<message_t>>`로 전달 | caller 0, source receive 자원 비용 미계측 | Native matching은 재구현하지 않는다. Service-wire correlation·frame 수·error kind·payload profile 검증은 binding에 없으므로 유지한다. S wire `:49–50,186–211`, S01 `:407–428`. |
| `F/src/runtime/foundation/operation_registry.cpp:269 post_completions`, `provider_task_context.cpp:174 dispatch_completion` | `B/include/zlink/Contracts/Messaging/operation_contracts.hpp:84–101` promise scheduler와 `B/src/Runtime/Messaging/async_operation_state.hpp:51 resume_async_slot` | caller의 결과 분류 4.2523 전체와 같지 않음. Source completion worker CPU는 별도 | Binding scheduler를 Framework 사용자 continuation의 종단으로 바로 쓰면 C1 terminal을 새 turn까지 미루거나 receive stack에서 사용자 코드를 실행할 수 있다. Internal observer는 native 알림 자리에서 C1 take, 그 뒤 기존 reserved item을 process dispatcher에 게시한다. S01 `:462,490–544`, S08 `:309–324`. |
| `F/include/zlink/framework/contracts/dispatch/request_task.hpp:648 receiver_state_t`, `:713 receive`, `task.hpp:622 complete`, `:721 on_completed` | Binding의 `async_result_t`는 move-only·single-consumer이며 already-ready/take/suspend/detach를 제공. `request_reply.cpp:16`은 admitted+reply+entry를 한 bundle에 배치 | `receive` self 0.1427/I 8.0194, task on_completed self 0.0999/I 0.2997. Framework 결과 분류 4.2523 | 범용 task 표현 일부가 겹친다. Framework public task의 결과 참조·observer·ambient context·gate 재개와 public/physical 결과 구별은 binding에 없다. Prepared slot의 동일 task facade로 대체하며 binding awaiter는 직접 보유한다. API를 binding single-consumer로 바꾸지 않는다. S01 `:331–333,515–519`, S gate §2–4. |
| `F/src/runtime/messaging/client_call_codec.cpp:73 create_envelope`, `envelope_codec.cpp:323 header_plan_t`, `:425 encode_header` | `message_t::allocate/data/bytes`, `advanced::external_message_t::from`은 바이트 저장·해제를 제공하며 Framework envelope 규칙은 제공하지 않음 | create envelope self 0.1998/I 0.2568, encode header self 0.0571/I 0.7135 | 정적 plan을 등록/첫 사용 때 준비하고 public message에 구성한다. Schema·UTF-8·metadata·flow·deadline 검증은 codec 한 곳에 유지한다. Binding의 raw request token을 JSON correlation 문자열의 별칭으로 사용하지 않는다. |
| `F/src/runtime/protocol/service_wire_codec.cpp:3120` 부근 `encode_application_payload_into`, `:3178 encode_application_payload_message` | Binding multipart는 part 수·native 소유권을 제공하며 nested Framework application profile은 제공하지 않음 | payload pack self 0.0999/I 0.5280. memcpy leaf 전체 0.4709는 이 pack만의 비용이 아님 | 현행 wire는 내부 [JSON header,payload]를 외부 payload frame 안에 count/length와 함께 넣는다. Binding multipart 호출만으로 이 framing을 삭제할 수 없다. 최종 frame의 한 번 구성으로 임시 native header를 제거하되 byte layout은 유지한다. S wire `:186–211`. |
| `bench/client/bench_cpp_client.cpp:942–949` admission/reply observer와 `bench/common/bench_async.hpp:41 completion_signal_t::notify` | Binding benchmark는 같은 caller에서 ready queue·poller·coroutine을 구동함 (`bench_cpp_client.cpp:580–600,673` 이하) | run_tasks self 0.5565/I 11.2728, take_completed self 0.0713/I 0.0856; 결과 분류 일부 | Benchmark가 고른 완료 queue·검증은 Framework 중복 메커니즘이 아니다. 제품 변경의 절감에 전부 넣지 않는다. Framework task facade의 통지 비용만 줄인다. 동일 harness에서 최종 판정한다. |

표에서 S01은 `S/01-execution/01-submit-and-completion.ko.md`, S08은 `S/01-execution/08-messaging-hot-path.ko.md`, S gate는 `S/01-execution/02-handler-turn-and-execution-gate.ko.md`, S wire는 `S/02-channel-transport/06-wire-protocol.ko.md`다.

**Caller의 요청별 작업과 실행 위치**

명세가 특정 OS caller thread를 요구하는 것은 아니다. 요구하는 것은 E1에서 payload 소유권을 확정하고, E3·E4를 같은 operation owner turn에서 수행하며, 등록이 완료된 뒤 한 번 submit하고, 반환 시 등록 완료를 관찰하게 하는 순서다. 이 작업의 성능 목표 때문에 encode와 submit은 caller에 둔다. 다른 worker에 요청별 작업을 게시하여 caller CPU만 줄이는 안은 계산에서 제외했다.

| 요청별 작업과 file:function | 표본 시간 self / I | 현재 할당·원자 연산·잠금 | 필요한 시점과 대체 위치 |
|---|---:|---|---|
| `bench_cpp_client.cpp:929 make_payload`, `:940 submit` | driver self 0.5565 / I 11.2728의 일부 | protobuf body·benchmark pending 배열, counters atomic | Application/harness 작업이다. 제거 절감으로 잡지 않는다. |
| `request_task.hpp:713 receive` | 0.1427 / 8.0194 | `:717 make_shared<receiver_state_t>` 1회. admission/reply/physical은 이미 내부 subobject다. callback lifetime control block은 내부 arena지만 refcount 연산은 남음 | E3 전에 최종 수신자를 준비해야 한다(S01 `:331,424`). 동일 request 저장소를 재사용한다. C1 completion slot 재배치를 새 개선으로 세지 않는다. |
| `channel_runtime.cpp:1742–1749` clock·flow·ambient capture | ambient capture self 0.0999/I 0.1284, enter context self 0.0999 | steady clock, flow 설정 확인, ambient 값과 수명 참조 | Deadline 시작·flow identity는 호출별이다. bind 때 같은 값으로 고정할 수 없다. 구성 조회와 빈 context 표현만 immutable plan에 둔다. |
| `channel_runtime.cpp:1755 resolve_node_transport`, `:1843–1849` decoder·RID 보관 | transport resolve self 0.0571. decoder 독립 시간 없음 | host/transport shared_ptr 복사, RID 값 보관. #3957의 target capture는 이미 request arena 사용 | 등록된 transport 연결·decoder 함수·정적 이름은 bind/첫 사용에 준비한다. 실제 target·generation·현재 Ready 판정은 호출별 소유자에 남긴다. |
| `serializer.hpp:303 get`, `serializer.cpp:164 cached_serializer` | 0.1284 / 0.1570 | atomic shared_ptr snapshot와 type lookup, descriptor 공유 | outbound declared type의 기존 resolved descriptor를 plan이 유지한다. 수신 handler plan은 #3957에 이미 있음. Unregistered type·content type·등록 우선순위는 `S/01-execution/05-payload-ownership-and-codec.ko.md:180–244` 그대로다. |
| `client_call_codec.cpp:73 create_envelope`, `:16 next_correlation_id`, `:34 deadline_bytes` | 0.1998 / 0.2568 | correlation atomic fetch_add 1, 이름 값·20byte deadline 생성 | 정적 이름·escape·기본 field는 plan. 동적 corr·deadline·metadata·flow는 호출별이다. Deadline text와 envelope correlation 생성 자체를 0회로 만들지 않는다. |
| `envelope_codec.cpp:425 encode_header`, `:332 matches`, `:382 write_header` | 0.0571 / 0.7135; append self 0.2711, string self 0.1712 | TLS 한 항목 cache를 매번 비교. validate 후 measure와 write로 같은 구성 순회 2회. native header message 할당 | plan이 준비한 정적 fragment와 동적 field를 최종 service buffer에 한 번 구성한다. 동적 필드 길이·escape 계산은 필요하다. 별도 encoder는 남기지 않는다. |
| typed serializer encode | codec 전체 분류 2.1547의 일부; 독립 protobuf 시간 미분리 | serializer가 반환하는 `encoded_payload_t`의 native/string 저장소 | Typed serializer는 호출별이다. Public `serialize_fn_t(const T&) -> encoded_payload_t`에는 caller 제공 final-buffer sink가 없다(`serializer.hpp:216`). 모든 사용자 codec에 무복사 직접 encode가 가능하다고 가정하지 않는다. |
| `public_host_runtime.cpp`의 request 전달, `mesh_node_runtime.cpp:4407 submit_node_request_reply` | host 0.1427 / 3.5531; mesh 0.0856 / 4.0525 | function/callback capture와 shared_ptr 복사, 여러 admission 결과 표현 | 전송·projection의 정적 연결은 bind plan. 수신자 포인터와 동적 timeout만 요청 인자로 전달한다. 내부 값 변환용 비동기 결과는 삭제한다(S01 `:331`). |
| `raw_mesh_node_owner.cpp:1519` owner turn, `state_lane.hpp:289 run_sync` | schedule_drain 0.0713 / 2.6826 | 무경합 mailbox lock+scheduled CAS, drain 종료 lock/store. 경합 시 promise/shared state·FIFO grant 대기 | owner turn 자체는 유지한다. 같은 owner 내부는 private on-lane 메서드를 호출한다. 외부 정상 무경합 경로는 이미 caller 실행이므로 새 fast path 절감으로 세지 않는다. |
| `raw_mesh_node_owner.cpp:1526 encode_application_payload_message`, `:1536 wire` | pack 0.0999 / 0.5280; node header 0.0571 / 0.0856 | 중간 envelope header·payload를 최종 service payload로 복사. wire reserve(2)용 vector backing 1회 | 최종 service frame 구성을 한 codec plan에 집중한다. Wire의 nested count/length는 유지한다. 현재 native writer를 또 구현하지 않는다. |
| `raw_mesh_node_owner.cpp:1533 take_reply_route_id_locked`, `:1535 operation_id` | 독립 시간 미분리, 등록 4.4235에 포함 | owner 직렬화 아래 correlation 증가, full high/low ID 조립 | 활성 request identity는 호출 때 필요하다. 값 범위의 저장 공간 예약은 가능하지만 bind 때 pending으로 미리 공개하면 안 된다. |
| `operation_registry.cpp:382 register_operation`, `:488 index_deadline` | 0.0856 / 0.4852 | C1 mutex 1구간, unordered_multimap node 1회, 드문 bucket 성장. C2는 node 내부 intrusive link, `_next_visit` atomic store. cancel 가능하면 stop_callback subobject | C1+C2 등록은 E4 전에 같은 owner에서 끝낸다. allocator/node capacity만 미리 준비하거나 page 단위 상각한다. C1 take·deadline authority는 유지한다. |
| `raw_mesh_node_owner.cpp:225 start_admitted_request` | 0.0571 / 0.8134 | peer Ready 확인, clock, 남은 ms 올림. 성공 시 logical 대기 객체 없음 | Peer Ready 판정·남은 deadline은 호출별이다. Readiness 대기 task를 정상 경로에 선할당하지 않는다. 이미 그렇게 동작하는 부분은 절감 0이다. |
| `raw_route_port.cpp:351 request_impl`, `raw_binding_adapter.hpp:57 submit_request_once` | 0.1570 / 0.5422 | outer mutex, RID 재구성, rvalue part 이동, public binding async result | prepared message와 값 RID를 public `.request().message().timeout().async()`에 한 번 전달한다. E3와 E4 사이 hand-off는 추가하지 않는다. |
| `raw_binding_adapter.hpp:147 observe_request_completion` | 0.0143 / 0.0713 | observer coroutine frame, binding awaiter. Binding 내부 shared_ptr·mutex도 사용 | observer frame을 기존 request 저장소에 둔다. Binding admitted/reply를 public co_await로 소비하며, C1 take 이전에 completion worker로 넘기지 않는다. |
| `raw_mesh_node_owner.cpp:413 await_request_admission`, 상위 admission 관찰 연결 | 0.0713 / 0.1284, 나머지는 task·등록 분류에 분산 | bool task, submit-result task, terminal observer/context·수명 참조 | 최종 admission view에 한 번 전달한다. 이미 ready인 result snapshot은 즉시 확정, pending stage만 await한다. 사용자 continuation의 새 turn 규칙은 유지한다. |
| `task.hpp:721 on_completed`, `bench_async.hpp:41 notify` | on_completed 0.0999 / 0.2997, take_completed 0.0713 / 0.0856 | pending observer마다 `make_shared<observer_t>` 1회(`task.hpp:767`), task mutex·wait-owner 등록, benchmark signal mutex | request 슬롯의 observer 저장소로 첫 observer를 수용한다. 임의 개수 public observer는 삭제하지 않고 같은 소유자가 확장 저장한다. benchmark signal queue와 검증은 그대로다. |
| 완료된 task의 result 관찰·파괴, payload 검증 | driver/결과 분류에 포함, 독립 시간 없음 | task result mutex, 공유 소유 release, protobuf/payload 해제 | caller의 검증을 다른 thread에 옮겨 시간을 숨기지 않는다. 이미 게시된 immutable typed 결과를 같은 task facade로 관찰한다. |

Allocator·refcount·mutex 비용은 이 표에 다시 더하지 않는다. 기존 CSV에서 allocator 관련 leaf를 모은 진단 합은 2.2974, shared-count 관련 leaf 합은 0.9703, pthread mutex 계열 합은 0.6421이다. Symbol 선택 범위에 따라 사용자 요약의 약 2.0/0.8/0.66과 다르다. 원자 연산 전체 횟수나 모든 allocation 횟수를 CPU sampling에서 복원할 수는 없다. 특히 0.6421 µs를 socket mutex 한 개의 절감이라고 보고하면 안 된다.

csites의 #3957 이후 별도 계측에서 source caller는 heap 13.9243회, lock 36.4516회/request였다. 이 숫자는 process role의 실제 횟수이고 함수별 정확한 횟수는 아니다. 같은 보고서의 source completion 6.0033 heap/request도 caller heap에 합치지 않는다. 이번 설계의 ‘0 heap’은 warmed request 결과 저장·기본 observer·C1 node의 Framework 추가 할당 목표다. Payload serializer·native message·binding bundle·사용자가 추가하는 observer까지 포함한 전체 호출의 0 heap을 뜻하지 않는다.

**등록과 deadline·admission의 소유 경계**

등록 4.42 µs를 해석할 때 먼저 확인할 사실은 현재 무경합 fast path가 이미 있다는 점이다. `state_lane.hpp:318`은 `schedule_drain(true, invoke_work)`를 시도하고, `state_lane.cpp:139–157`은 선행 mailbox가 없고 기존 `_scheduled` claim을 얻었을 때 caller의 stack에서 current scope를 설정해 실행한다. 같은 lane 위에서 public `run_sync()`를 다시 호출하면 `throw_if_reentrant()`가 거부한다. `operation_registry.cpp:428–458`은 이미 registry owner lane이면 별도 owner post 없이 index를 갱신한다.

| 호출 상황 | 현행 동작 | 채택할 설계 | 판정자와 위험 |
|---|---|---|---|
| caller가 operation owner의 private turn 내부 | private on-lane 동작은 직접 호출 가능. public lane 재진입은 거부 | 등록과 submit의 본문을 같은 owner의 private 메서드로 집중한다. owner 내부 호출자가 public 진입을 재사용하지 않게 한다 | owner 1→1. `IsOnLane`이면 public 재진입까지 허용하는 예외를 추가하지 않는다. |
| 외부 caller, lane 무경합 | 이미 caller가 FIFO 순서·single turn 권한을 얻어 실행 | 같은 경로 유지. 별도 future·다른 worker 게시가 없는지 구조 검사 | 1→1. 제거 가능한 새 왕복은 0개다. I 2.6826 전체를 절감으로 잡지 않는다. |
| 외부 caller, lane 경합 | 선행 work 뒤에 grant를 넣고 caller가 자기 turn을 실행 | 하나의 기존 FIFO 경로를 유지. 저장 객체 재사용은 가능하지만 pending/closed 규칙은 동일 | 1→1. trylock 실패 때 규칙이 다른 두 번째 등록 경로를 만들지 않는다. |
| 실제 요청을 묶어 등록 | 아직 시작하지 않은 요청의 identity/deadline까지 미리 공개하거나 즉시 제출을 지연할 위험 | C1 node·buffer·dispatcher 저장 자리만 page 단위로 미리 확보한다. 각 호출의 C1+C2 publication→submit 순서는 그대로다 | terminal 1→1. Batch API·batch timeout·추가 send queue 없음. request-serial도 batch를 기다리지 않는다. |

| 항목 | 요청별 heap | 요청별 lock/atomic | Binding으로 제거 가능한지 |
|---|---|---|---|
| C1 map node | 보통 1 node, bucket rehash 별도 | register의 `_mutex` 1구간. take는 완료 자원에서 같은 mutex | Native pending과 다른 권한이다. Framework node allocator 재사용은 가능하고 C1은 유지한다. |
| C1 completion item | hot request는 #3957 이후 기존 512byte slot이므로 별도 heap 0 | drain accounting atomic, callback lifetime 참조 | 이미 개선된 부분이다. 재계상하지 않는다. |
| Deadline index 연결 | 별도 heap 0. 위 pending node 안의 previous/next/slot 사용 | 이 정상 경로에서는 C1 mutex 안에서 link. `_next_visit` store. 별도 index lock 없음 | Framework 전체 budget이므로 유지한다. Index를 위해 별도 timer·heap·lock을 제거한다는 주장은 현재 경로와 맞지 않는다. |
| 다음 deadline 관리 예약 | request마다 timer 없음 | 첫 등록 또는 더 이른 deadline일 때 `_deadline_changed`가 wake를 요청할 수 있음 | 관리 wake가 요청마다 발생한다고 가정하지 않는다. S01 `:475–483`의 단일 다음 방문 계산을 유지한다. |
| Framework admission 상태 | typed hot path는 receiver_state 안의 subobject이므로 별도 TCS heap 0 | `task.hpp:622`의 task mutex와 publish/observer. 별도 bridge coroutine·observer는 남음 | 이미 inline인 상태를 다시 inline화하지 않는다. 중간 task 변환을 없앤다. |
| Binding admission 상태 | `request_reply.cpp:116` bundle 1 heap에 reply·entry와 함께 있음 | binding completion entry/result mutex·atomic | Public API로 이 bundle을 caller 저장소에 배치할 수 없다. Framework pool로 복제하지 않는다. |
| Ready peer의 logical admission | 성공 경로에는 대기 heap 0 | topology lookup과 deadline 확인 | Binding의 native WRITABLE과 다르다. 실제 대기 시만 기존 비동기 경로를 쓴다. |

현재 C1 `_pending_count`는 observation이고 admission cap이 아니다. 새 prepared pool의 크기를 이유로 request를 거부하거나 대기시킬 수 없다(S01 `:521–537`). Pool은 저장소 allocator의 교체이며 pending 권한 표나 대기 queue가 아니다. 동시 outstanding 수가 초기 확보량을 넘으면 page를 늘린다. 그 cold-path 할당은 숨기지 않고 따로 계측해야 한다.

**Binding submit 분류의 추가 3.020 µs**

다음 표는 각 표본에서 처음 확인되는 binding/Core frame을 기준으로 서로 겹치지 않게 묶었다. Frame이 잘린 표본의 원래 Framework 호출자는 복원하지 않았다. 차이의 원인별 확정량이나 syscall 횟수가 아니다.

| 확인된 frame 묶음 | Framework | Binding | 차이 | 코드와 해석 |
|---|---:|---:|---:|---|
| native `msg_t::init_size` 계열 | 0.7848 | 0.2457 | +0.5392 | Framework는 JSON header와 typed body 외에 packed application payload와 작은 service header를 만든다. `raw_mesh_node_owner.cpp:1526–1539`, `service_wire_codec.cpp:3145–3159`. 부모가 없는 표본도 포함되어 정확한 할당 site별 차이는 미확정이다. |
| epoll_ctl syscall 계열 | 0.4424 | 0.0199 | +0.4225 | stack에 `epoll_ctl → boost::asio ... mailbox_t::schedule_if_needed_unlocked`가 있다. Framework의 새 syscall인지 Core mailbox 스케줄 차이인지, 빈도인지 단가인지 미분리다. Native 호출 하나가 epoll_ctl을 늘렸다는 인과관계는 입증되지 않았다. |
| binding pending insertion | 0.2283 | 0.0422 | +0.1861 | `completion_owner.cpp:572`의 inline entry/PMR map. 더 많은 live request, cache locality, 경합 여부는 독립 계측이 없다. Framework에 두 번째 pending map을 만들어 이 비용을 없앨 수 없다. |
| builder factory | 0.1427 | 0.0372 | +0.1055 | `router.cpp:85`의 operation state acquire·RID cache. 양쪽 모두 같은 public builder를 사용한다. |
| request attempt | 0.1570 | 0.0645 | +0.0924 | `completion_owner.cpp:279`. Core submit·admission publish 아래 표본과 별개 묶음이다. |
| native array submit adapter | 0.1284 | 0.0372 | +0.0912 | `operation_submit.hpp:128`. Framework도 binding benchmark도 outer part는 2개다. Framework에만 multipart native-array heap이 생긴다는 근거는 없다. |
| async bundle 생성·처리 | 0.2426 | 0.1588 | +0.0838 | `request_reply.cpp:102–142`. 한 `make_shared` bundle에 admitted/reply/entry가 함께 있다. Caller 제공 bundle storage API는 없다. |
| completion consume/drain | 0.0428 | 0.4591 | −0.4163 | Binding benchmark caller가 직접 poller/완료를 진행한다. Framework는 receive 역할에서 대부분 진행한다. Caller 시간 비교에 역할 차이가 포함되어 있다. |
| 나머지 binding/Core/kernel frame | 3.3676 | 1.4516 | +1.9159 | Core request/pipe·message close/copy·mailbox·잠금·scheduler와 부모 미확인 표본이다. 이를 하나의 제거 가능한 Framework wrapper로 이름 붙일 수 없다. |
| 합계 | 5.5365 | 2.5162 | +3.0203 | 반올림 오차가 있다. |

실제 호출 수와 소유권을 코드로 대조한 결론은 다음과 같다.

- `raw_route_port.cpp:375`의 Framework socket mutex는 `.async()` 바깥에 있다. 일부 lock 표본은 등록 분류로 들어간다. 추가 3.020 µs 전부의 원인으로 볼 수 없다.
- `raw_route_port.cpp:383–391`과 binding benchmark `bench_cpp_client.cpp:684–689`는 모두 `.request(target).message(first).message(second).timeout(...).async()`다. Framework 내부 application payload가 nested라는 차이는 있지만 public binding 호출 수나 outer multipart part 수가 두 배인 것은 아니다.
- `materialize_binding_part(zlink::message_t)`는 move return이다(`raw_binding_adapter.hpp:223`). 성공 경로에서 raw byte vector를 external message로 다시 만드는 `:211` overload가 매 part 실행된다는 주장은 맞지 않는다.
- `raw_request_callback_t`는 inline storage다(`raw_route_port.hpp:116–148`). Constructor symbol이 관측됐다고 이 callback 자체를 요청별 heap 하나로 세지 않는다.
- Binding operation-state pool은 이미 존재한다(`operation_state.hpp:304–345`). 즉시 admission이면 `completion_owner.cpp:327–335`가 caller에서 state를 즉시 pool로 돌려준다. Framework는 무조건 receive thread에서 state를 해제해 caller TLS pool을 못 쓴다는 설명도 맞지 않는다. WRITABLE 재제출 뒤 반환 자원은 별도지만 비율은 미계측이다.
- `native_message_parts.hpp:22`의 작은 native array stack capacity는 8이다. 이 workload의 2개 outer part에 별도의 native 배열 heap을 없애겠다는 제안은 이미 있는 기능을 재구현한다.
- Header plan은 매 요청 `matches()`를 재확인한다(`envelope_codec.cpp:438–440`). 정적 prefix/middle cache는 이미 있으므로 cache 도입 자체를 새 구조 개선으로 보고하지 않는다. 바꿀 것은 plan 소유를 TLS 한 항목에서 불변 등록으로 옮기고, measure/write 중복 순회와 중간 native header 형성을 없애는 것이다.

Public API만으로 확실히 바꿀 수 있는 입력은 최종 message의 구성 방식, 소유한 part의 move, RID 값의 유지, 불필요한 Framework socket 직렬화, binding result를 받는 Framework observer의 표현이다. Core mailbox·native timeout·binding pending map의 lock·bundle allocator를 Framework에서 다시 구현할 수는 없다. 그 비용을 줄인다고 추가 retry·두 번째 poller·physical generation 표를 도입하지 않는다.

따라서 binding 분류를 5.5365→3.3000으로 줄이는 아래 예산에는 미검증 가정이 있다. 직접 관찰한 native allocation 차이는 약 0.5392이고, epoll 차이 0.4225까지 모두 회수한다고 가정해도 0.9617이다. 이 둘을 전부 줄이고 다른 분류도 최종 목표에 도달한다고 가정하면 17.480−3.7735−3.4523−1.5047−0.9617=7.7878 µs다. 6.513에는 추가 1.2748 µs가 필요하다. 이 차이를 ‘binding 직접 호출 효과’라고 확정할 자료가 없다. 반대로 7.7878도 하한은 아니다. 앞의 두 차이가 실제 제거 가능하다는 가정부터 검증이 필요하다.

**설계 대안과 최종 호출 구조**

| 대안 | 대체하는 기존 메커니즘 | caller 비용과 근거 | 소유 계층·공통 계약 영향 |
|---|---|---|---|
| A. Public binding + 기존 task 표현의 얇은 adapter | bool/submit-result admission chain과 raw completion wrapper만 최종 receiver로 집중. C1, task_shared_state와 현재 wire pack은 유지 | 보수적 비교 시나리오 9.113 µs = 등록 1.500+결과 1.500+encoding 1.000+binding 분류 4.000+기타 1.113. 각 잔여값은 설계 가정이며 예측 실측이 아니다. ≤6.6을 설명하지 못한다 | Framework C1/C2와 task 소유자 유지. Binding native operation 1개. S01 `:331–333,424–473,490–519`, S08 `:98–100`. 언어별 구현 최적화이며 네 runtime의 사용자 동작은 동일하다. |
| B. Prepared request 저장소 + 등록된 message plan + public binding 직접 제출 — 추천 | A의 adapter 통합에 더해 request task·observer·등록 node allocator와 최종 wire 구성을 하나의 수명 구조로 대체. 아래 PR 1–3 | 목표 6.513, 보수적 비교 9.113. 6.6 달성에는 등록 0.650/결과 0.800/encoding 0.650/binding 분류 3.300을 모두 만족해야 한다. 현재 증거는 이 동시 달성을 보장하지 못한다 | 결정은 그대로 Framework logical entry·deadline index·dispatcher와 binding native owner가 나눠 소유한다. S01·S08·S gate·lane §5를 유지한다. Public API·wire·네 언어 동작 변경 없음. 저장/수명 표현의 변경은 C++ 내부다. |
| C. Binding async_result만 public 결과로 노출하는 binding-only 경로 — 기각 | Framework C1/C2, public/physical typed 수신자, service-wire wrapper와 completion dispatcher까지 삭제 | Binding 3.685가 비교 시작점이지만 동일 기능이 아니다. 삭제 대상 등록+결과+encoding 분류의 합 10.8305는 최대 노출 비용이지 예상 절감이 아니다. typed decode·harness 등도 포함되어 전부 제거할 수 없다 | S01 `:331–333,392–428,447–463,490–519`, S08 `:309`와 충돌한다. Full OperationId·logical deadline·late physical 처리·사용자 continuation 실행 위치가 사라진다. PM의 판정자 유지 조건에도 위배되므로 D다. 구현 후보로 승인 요청하지 않는다. |

추천 B의 호출 구조는 다음과 같다. 각 단계의 종결 권한은 현재 소유자에 남긴다.

```text
등록/첫 사용
  기존 registration plan이 serializer descriptor·정적 JSON fragment·wire layout·projection을 보유
  기존 request allocator가 결과/관찰/frame 저장소와 C1 node용 page를 준비

caller의 한 요청
  request slot 획득 → monotonic deadline·동적 값·typed encode
  → 같은 operation owner turn에서 identity·C1·C2·dispatcher 자리 등록
  → 준비된 service header + 최종 application payload를 public binding builder에 한 번 제출
  → result snapshot을 최종 admission view에 반영 → 동일 public request_task 반환

binding completion을 받은 자원
  public binding awaiter가 내부 frame에서 native 결과 소비
  → 기존 service-wire/오류 변환 소유자 → 기존 C1 take
  → 이미 예약된 completion item을 기존 process dispatcher에 게시

독립 completion turn
  service/envelope/typed 변환을 각 codec 소유자에서 한 번 수행
  → 같은 slot의 public·physical 결과 view 확정 → 사용자 observer/continuation
  → callback capture·native 결과 정리
```

이미 public 결과가 끝났어도 전달된 physical 결과는 같은 준비된 수신자가 한 번 처리한다. Public 실패로 일찍 끝난 slot은 physical observer가 끝나기 전 재사용하지 않는다. C2의 stale metadata 참조에는 callback·payload·사용자 continuation을 남기지 않는다. Binding이 닫힌 native correlation에서 폐기한 reply를 다시 Framework로 요구하지 않는다(S01 `:333`, cancellation 문서 `:69–76`).

Prepared slot은 새로운 request 상태기계가 아니다. 현행 receiver_state에 이미 있는 admission/public/physical 결과, C1 completion storage와 capture 수명을 표현하는 방법을 바꾼다. Public admission 종료와 physical 자원 종료라는 서로 다른 사실은 합치지 않는다. C1 take의 승패를 판단하는 두 번째 `completed` flag도 추가하지 않는다.

결과 표현은 `task_t`의 동일한 private state handle로 통일한다. Shared control block을 여러 겹 복사하는 대신 task 모듈이 한 개의 intrusive 수명 참조를 제공하고, admission/reply/physical은 그 소유자의 view가 된다. 이 참조는 기존 shared ownership을 대체하며 terminal 여부를 판정하지 않는다. Pending task/observer의 기존 owner 수명을 한 방식으로 옮기고, request만의 `fast_task`나 별도 public API를 만들지 않는다. 기존 shared-state 요청 경로를 최적 경로의 fallback으로 유지하면 중복 제거가 아니므로 이 안의 완료로 보지 않는다.

결과 준비 여부와 observer 등록의 경쟁은 task 모듈 한 곳에서 처리한다. 기존 stage별 mutex·condition variable·continuation list를 그대로 둔 채 intrusive counter만 추가하지 않는다. 동일 request storage의 동기화로 교체하고 blocking 결과 대기·추가 observer·ambient context·owner shutdown·gate 재개를 같은 facade가 처리한다. 0 shared_ptr 목표는 이 Framework 결과 저장/기본 관찰 내부에 한정한다. Binding async_result의 shared_ptr와 physical 종료까지 필요한 host/port lifetime 참조는 별도다. 그 참조까지 지우려면 기존 shutdown/drain 순서로 수명을 증명해야 하며, 비용 예산을 맞추려고 raw pointer로 바꾸지 않는다.

기본 observer와 내부 binding observer coroutine frame은 같은 request storage에서 생성한다. 언어의 coroutine frame allocation을 request resource로 공급하는 구현은 가능하지만 public binding bundle에 caller allocator를 주입할 수는 없다. Binding 내부 `detail` state를 Framework 슬롯에 복사하거나 binding pending을 대신 소유하지 않는다. 여러 public observer가 필요한 경우 같은 owner의 확장 저장소를 사용한다. 첫 observer의 0 heap 목표가 observer 수 제한이 되면 계약 위반이다.

Prepared message plan은 codec의 기존 표현을 교체한다. Static escaped fragment·정적 field 길이·기본 metadata 구조를 등록/첫 사용에 준비하고, correlation·deadline·flow·metadata·payload 길이는 호출마다 검증·기록한다. 현재 `header_plan_t::matches/rebuild`와 별도 `create_envelope→native header→pack` 구성을 그대로 남겨 optimized plan과 경쟁하게 하지 않는다. 동적 호출도 같은 plan 구성기에서 동적 fragment를 처리한다.

Custom serializer의 public 계약은 `serialize(const T&) -> encoded_payload_t`다. 그 반환 bytes를 최종 outer frame에 한 번 복사하는 일까지 모두 없앤다는 약속은 하지 않는다. Default codec의 native message 기능은 사용하되 새 public sink API나 sample 수동 codec을 요구하지 않는다. 현행 nested wire의 part count·length·순서·UTF-8·metadata 상한·flow 검증은 유지한다. 기존 byte writer를 최종 frame 구성의 단일 소유자로 확장하며 별도 byte-layout 구현을 추가하지 않는다.

**PR 단위의 변경과 비용 예산**

크기순은 목표 절감 기준이다. 각 PR은 PM이 대안 B를 선택한 뒤에만 구현한다. 아래 숫자는 표본에서 확인한 제거 대상의 범위를 바탕으로 설정한 합격 예산이다. 함수별 실측 절감의 합이 아니며, 특히 allocator·scheduler 간접 효과는 미검증이다. ccall 보고서의 비례 배분 감축표를 예상 성능으로 다시 사용하지 않았다.

| 배타 분류 | 현재 | PR 1 뒤 목표 | PR 2 뒤 목표 | PR 3 뒤 목표 | 총 목표 절감 |
|---|---:|---:|---:|---:|---:|
| 등록 | 4.4235 | 2.1000 | 1.3500 | 0.6500 | 3.7735 |
| 결과 관찰 | 4.2523 | 1.2000 | 1.2000 | 0.8000 | 3.4523 |
| Encoding | 2.1547 | 2.1547 | 0.6500 | 0.6500 | 1.5047 |
| Binding 분류 | 5.5365 | 5.5365 | 4.0000 | 3.3000 | 2.2365 |
| 기타 | 1.1130 | 1.1130 | 1.1130 | 1.1130 | 0.0000 |
| 합계 | 17.4800 | 12.1042 | 8.3130 | 6.5130 | 10.9670 |

기타 1.113은 절감하지 않았다. Framework 외부의 harness와 미해결 표본을 0으로 만드는 가정을 피하기 위해서다. 각 최종 분류가 목표를 넘으면 다른 분류에 같은 절감을 다시 넣지 않고 실제 총 caller CPU로 판정해야 한다.

**PR 1. 최종 receiver 기반의 admission·task 저장 표현**

목표 절감은 등록 2.3235+결과 3.0523=5.3758 µs다. 가장 큰 두 분류를 만드는 별도 결과 표현과 관찰 연결을 구조적으로 제거한다. 원래 request block을 하나 더 합치는 변경이 아니다.

변경 파일은 `F/include/zlink/framework/contracts/dispatch/{task.hpp,request_task.hpp}`, `F/src/runtime/{mesh/raw_mesh_node_owner.cpp,mesh/mesh_node_runtime.cpp,stateful/public_host_runtime.cpp,backend/raw_binding_adapter.hpp}`, 이 내부 task state를 사용하는 호출 경로다. `task_t::shared_state`는 private이므로 public task API를 바꾸지 않고 저장 handle을 교체할 수 있다(`task.hpp:1263–1279`). 다만 내부 사용처가 넓어 단순한 callback 한 곳 수정은 아니다.

기존 `request_with_header → task<bool> → host submit-result task → mesh submission observer → admitted`의 값 변환 chain을 최종 admission receiver에 대한 전달 하나로 교체한다. 즉시 `OK`는 snapshot으로 확정하고, `BACKPRESSURED`만 binding admitted를 await한다. Registry/close/readiness에서 시작 전 실패가 난 경우도 같은 admission view로 전달한다. 사용자 admission continuation이 수신 stack에서 실행되는 경로를 만들지 않는다.

기존 request block은 유지하면서 generic task control block과 observer 저장 표현을 task 모듈의 한 수명 구조로 교체한다. 기본 observer와 내부 coroutine frame은 요청 storage에 둔다. 새 pool/freelist를 도입한다면 별도의 completion queue가 아니라 기존 request allocator를 대체해야 한다. Producer·public view·physical processing에 필요한 수명 참조를 하나로 모으되, payload와 callback은 기존 정리 시점에 해제한다. 사용자 handle이 오래 남았다고 callback capture까지 유지하지 않는다.

근거는 결과 분류 4.2523, 등록 분류 4.4235, source caller의 별도 계측 heap 13.9243과 관측된 shared-count leaf 비용이다. `receive` I 8.0194 전체를 절감으로 세지 않는다. `on_completed` I 0.2997과 `await_request_admission` I 0.1284만으로 5.3758 절감을 입증할 수 없으며, 나머지는 객체 생성·파괴·context·관찰 chain을 실제로 없애야 얻는 목표다. 첫 구현 결과가 그 이하라고 가정해 후속 예산을 확정하지 않는다.

판정 위치 수는 C1 terminal 1→1, deadline 1→1, native admission 1→1, 최종 typed 전달 1→1이다. 중간 비동기 결과·observer가 여러 개에서 하나의 결과 view 체계로 줄어드는 것은 판정자 감소와 구별한다. 새 intrusive 수명 counter는 기존 shared control block들의 strong/weak 참조를 대체한다. 기존 `_result`와 별도 `completed` flag를 함께 두지 않으며, observer 저장과 ready publication의 동기화도 한 소유자가 처리한다. 같은 요청이 기존 shared-state 경로와 새 slot 경로 중 하나를 고르는 분기는 남기지 않는다.

추가 검사는 기존 `test_cpp_framework_{async_task,request_transport_projection,request_observer_lifetime,request_call_allocations,request_reply_cost}.cpp`와 task contract test에 둔다. 이미 완료된 admission, pending admission, admission failure 뒤 late physical 성공/실패, 요청 handle 조기 파괴, 값 참조 수명, 여러 observer, ambient context 복원, callback 예외 뒤 다른 완료 진행을 검증한다. 정상 자료는 전달·해제 각각 1회, caller result는 유지되어야 한다. 의도적으로 이중 terminal 또는 조기 slot 반환을 주입하면 동일 assertion이 실패해야 한다. Allocation 검사에는 ordinary allocator를 사용한 대조 입력도 넣어 검사가 실제 allocation을 검출하는지 확인한다.

위험은 API 의미를 바꾸지 않고 저장 표현을 교체하는 범위가 넓다는 점, task/observer 등록과 완료의 경쟁, public 결과 이후 physical 수명, alignment·coroutine 크기, C++ 소멸 순서다. Binding bundle 할당은 그대로 남는다. 수명 증명 없이 0 shared_ptr 목표를 달성했다고 보고하지 않는다.

**PR 2. 등록된 message plan과 최종 native frame 구성**

목표 절감은 encoding 1.5047+binding 분류 1.5365+등록 0.7500=3.7912 µs다. 이 셋은 동일 최종 frame 구성의 서로 다른 표본 귀속이다. ‘header 캐시 추가’, ‘memcpy 몇 개 삭제’를 독립 PR로 나누지 않는다.

변경 파일은 `F/include/zlink/framework/contracts/codecs/serializer.hpp`, outbound typed call을 소유한 `F/include/zlink/framework/contracts/channels/{call.hpp,channel.hpp}`의 기존 route/channel call plan, `F/src/runtime/{channels/channel_runtime.cpp,messaging/client_call_codec.cpp,messaging/envelope_codec.cpp,protocol/service_wire_codec.cpp,mesh/raw_mesh_node_owner.cpp,backend/raw_route_port.cpp}`다. Target의 `F/src/runtime/channels/channel_reply_writer.cpp`와 기존 handler codec plan도 동일 구성기를 사용한다. 이미 적용된 handler descriptor와 borrowed reply header를 재구현하지 않는다.

기존 ‘typed body message + JSON header message → application_payload_t → 별도 packed native message → 임시 wire vector → binding builder’에서 최종 service payload buffer를 소유한 codec 구성이 typed bytes·header fragment를 기록하도록 바꾼다. Public custom serializer의 반환값은 유지하며 필요한 한 번의 body copy를 남긴다. 정적 header field는 immutable plan, 동적 field는 같은 구성기의 입력이다. Service command header와 outer payload의 2-part wire는 그대로다. 소유한 native parts는 public binding `.message(std::move(part))`로 넘긴다. 이미 native writer가 있는 service codec를 단일 소유자로 사용한다.

요청마다 serializer cache를 조회하는 outbound 부분은 실제 frozen descriptor를 plan이 유지하게 한다. 등록되지 않았거나 규범이 재평가를 요구하는 declared type은 같은 serializer 소유자의 규칙을 따른다. 임의 type의 cache miss를 성공으로 고정하거나 등록된 content-type 우선순위를 바꾸지 않는다. 수신측 scope·handler·DI의 수명은 건드리지 않는다.

근거는 `encode_header` I 0.7135, `encode_application_payload_message` I 0.5280, cached_serializer I 0.1570, native allocation 묶음의 F/B 차이 0.5392다. 함수 I와 native 할당 표본이 겹칠 수 있으므로 이 숫자를 합쳐 PR 절감으로 계산하지 않는다. 3.7912 전체가 설명된 값은 아니다. 기존 분류에서 부모가 잘린 allocator·writer 표본과 mutable envelope 구성까지 사라진다는 목표를 포함한다. 특히 사용자 serializer가 final-buffer sink를 제공하지 않는다는 제약 때문에 실제 잔여 encoding이 0.650보다 클 수 있다.

판정 위치 수는 codec 선택 1→1, wire layout/validation 소유자 1→1, C1/deadline/admission/typed 전달 각각 1→1이다. TLS 한 항목 header plan과 새 plan을 병행하지 않는다. Wire schema의 두 번째 매핑표도 만들지 않는다. Buffer pool이나 free callback을 쓰면 기존 임시 message/bytes 소유를 대체하며, Core message가 아직 참조하는 memory는 반환하지 않는다. 여기서 pool은 Core의 native message allocator를 복제하는 것이 아니라 Framework의 결과·임시 구성 storage를 재사용하는 범위다. Public `message_t::allocate`로 충분한 부분은 그것을 사용한다.

추가 검사는 기존 `test_cpp_framework_{envelope_allocations,envelope_key_cost,request_encoder_allocations,service_wire_codec,service_wire_codec_generated,request_snapshots,route_payload_ownership}.cpp`에 둔다. 기존 canonical/golden bytes와 새 구성의 byte 동일성, 교차언어 request/reply, escaped 이름, 빈/큰 metadata, 잘못된 UTF-8, flow on/off, 같은 type의 serializer 우선순위, caller payload 재사용·조기 해제, native part가 실제 수신까지 유효한지를 확인한다. 정상 frame은 통과하고 count/length/metadata 한도·trailing byte를 의도적으로 깨뜨린 frame은 거부되어야 한다. Allocation probe는 cold/warm·기본/사용자 codec을 구별한다.

위험은 wire 상호운용성, dynamic field의 길이·escape, serializer 부작용을 한 번만 실행해야 한다는 점, send/receive message move 소멸 순서다. Public serializer를 두 번 호출해 size를 재는 방식은 채택하지 않는다.

**PR 3. 기존 operation owner와 binding lifecycle에 제출 집중**

목표 절감은 등록 0.7000+결과 0.4000+binding 분류 0.7000=1.8000 µs다. 이미 있는 caller-inline lane을 다시 만드는 작업은 포함하지 않는다.

변경 파일은 `F/src/runtime/{backend/raw_route_port.hpp,backend/raw_route_port.cpp,mesh/raw_mesh_node_owner.cpp,foundation/operation_registry.hpp,foundation/operation_registry.cpp}`, 실제 socket close를 소유한 `mesh_node_host_service.cpp`·`public_host_runtime.cpp`의 호출 경로다. `state_lane.{hpp,cpp}`는 FIFO·재진입을 바꾸지 않고 필요한 기존 경계를 확인하는 대상이다. Target `receive_if_ready`와 `reply`도 동일 socket 수명 규칙을 사용해야 하며, request만 별도 transport locking 정책을 두지 않는다.

Transport submit 직렬화는 public binding→Core whole-message admission 한 곳에 둔다. Framework mutex가 보호하던 `_socket` C++ wrapper의 접근 가능 기간과 `_received` 출력 객체의 단일 수신자는 기존 host/port 수명과 receive owner가 보장해야 한다. 그 조건이 성립하지 않으면 outer mutex 제거를 구현하지 않고 blocker로 남긴다. Core raw handle 수명 보호와 C++ wrapper 객체 파괴 경합은 다르다. 새 inflight flag/counter나 세 번째 lifecycle gate를 추가해 맞추지 않는다.

C1 map node의 일반 heap 할당은 동일 registry allocator의 page 재사용으로 바꾼다. C2 index의 intrusive link는 이미 무할당이므로 새 wheel/index를 만들지 않는다. 같은 lane 안의 등록 helper는 private body를 직접 부르고, 다른 lane/public 재진입은 계속 거부한다. 실제 등록을 background batch queue에 게시하지 않는다. 동일 호출의 typed 결과 publication에 남은 불필요한 관찰 연결은 PR 1의 소유자로 모으며 두 번째 result delivery 경로는 남기지 않는다.

근거는 Framework outer mutex의 코드 위치, csites caller lock 36.4516회, registry node의 매 호출 emplace와 비용 I 0.4852다. 이 I보다 큰 등록 절감은 allocator·상위 전달 체계까지 줄어드는 가정을 포함한다. 모든 pthread mutex leaf 0.6421보다 큰 ‘socket mutex 절감’을 주장하지 않는다. Binding 분류의 epoll 0.4225는 삭제 보장량이 아니다. 따라서 이 PR의 1.8000도 합격 예산이며, 남은 1.2748 µs 증거 부족을 이 PR이 자동 해결한다고 간주하지 않는다.

판정 위치 수는 물리 submit 직렬화 Framework+Core 2→Core 1, native request/retry 1→1, logical C1 1→1, C2 deadline 1→1, admission 1→1, typed 전달 1→1이다. `_socket_mutex`를 없애면서 같은 역할의 새 mutex·flag를 만들지 않는다. `_poller_mutex`는 poller 사용/close의 다른 책임이므로 같은 결정으로 세어 무조건 삭제하지 않는다. Node allocator의 재사용 수명은 기존 pending node 수명을 대체하며 두 번째 active map이나 capacity counter를 추가하지 않는다.

추가 검사는 기존 `test_cpp_framework_{state_lane,operation_registry,raw_route_port_contract,request_transport,request_native_counts}.cpp`와 binding의 기존 exact-target/WRITABLE/close contract에 맞춘 Framework public repro다. 무경합 caller-inline, 경합 FIFO, 같은 lane public 재진입 거부, shutdown/close와 submit 경쟁, 즉시 reply, 동일 full OperationId 경쟁, node/address 재사용, deadline 만료와 reply 경쟁, 관리 한 회전 안의 stale 참조 해제, permit 0에서도 binding completion 진행을 검증한다. 정상 자료는 모두 한 번 완료하고, 의도적으로 register-before-submit 순서를 뒤집거나 이전 generation의 결과를 넣으면 거부되어야 한다. 새 poller·재시도 경로가 생기지 않았는지도 구조 검사로 확인한다.

위험은 C++ wrapper와 poller lifetime, 다른 thread의 last-owner 해제, deadline index에 남은 node 참조의 재사용, ABA, close race다. 이 위험을 설명할 새 generation 상태를 Framework에 더하는 안은 채택하지 않는다. 기존 고유 OperationId와 node 수명으로 해결하지 못하면 blocker다.

**결정 소유 표와 네 runtime 대조**

| 결정 | 현행 C++ file:line | .NET | Java/Kotlin | Node.js | B에서의 단일 소유자와 판정 위치 전→후 |
|---|---|---|---|---|---|
| logical terminal | `F/src/runtime/foundation/operation_registry.cpp:539 take` | `Runtime/Service/ZLinkManagedMeshNode.cs:5845 TryRemoveOperation`, `:11115 CompleteManagedOperation` | `runtime/internal/service/ZLinkServiceOperationRegistry.java:398 complete`, `:513 take` | `packages/framework/src/runtime/foundation/operation-registry.ts:434 take` | Framework operation registry 1→1. Binding native completion은 다른 층의 사실이다. |
| deadline·등록·close 수락 | `raw_mesh_node_owner.cpp:1519`, registry `:382,488` | ManagedMeshNode `:5608 CreateOperationAsync`, `:5663 _operationLane.RunAsync`, `:86 _operationDeadlines` | OperationRegistry `:329 submitTerminal`, owner turn 안 등록/시작 | raw service runtime `:761` 부근 등록, operation-registry `:240 register`, `:406` 관리 cursor | Framework operation owner와 deadline index 각각 1→1. 새 timer·별도 등록 owner 없음. |
| native admission과 WRITABLE | `B/src/Runtime/Messaging/completion_owner.cpp:279,668`, Framework adapter `:147` | Binding submission의 result/admitted/reply를 관찰 | Java raw port가 binding submission을 관찰 | node raw binding port가 binding submission을 관찰 | Binding/Core 1→1. Framework는 stage를 변환할 뿐 별도 retry 결정 0개다. |
| 최종 사용자 결과 전달 turn | registry `:269`, provider context `:174`, request_task `:682` | `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13 Post`가 ThreadPool WorkItem 게시 | `ZLinkServiceCompletionDispatcher.java:51 post`, `:69–74`가 item별 독립 게시 | `raw-service-mesh-runtime.ts:790,794,805,808`의 queueMicrotask | 기존 process dispatcher와 최종 typed receiver 각각 1→1. Receive stack에서 callback 실행하지 않는다. |
| state lane 진입·재진입 | `state_lane.hpp:289 run_sync`, `state_lane.cpp:57 throw_if_reentrant` | `Runtime/Execution/ZLinkStateLane.cs:80` 재진입 검사·현재 owner scope | `runtime/internal/execution/ZLinkStateLane.java:125–142` inline turn, `:290` 재진입 거부 | `runtime/execution/state-lane.ts:56,92` 재진입 거부, `:129` lane scope | 같은 owner의 private 상태 접근만 직접 호출. Public lane 진입 규칙 1→1. |
| handler 실행·reply 시작 | host service `:3805`, route handler invoker `:63` 완료 turn | 기존 application worker/handler continuation | 공유 Java runtime의 dispatcher/handler continuation | 기존 JS application turn | Framework queue/gate/scope 1→1, reply transport admission 1→1. Binding recv callback으로 대체하지 않는다. |

.NET 경로는 `framework/languages/dotnet/src/Zlink.Framework/`, Java는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/`, Node는 `framework/languages/node/` 기준이다. Kotlin은 같은 Java service runtime을 사용한다. 대조는 저장 표현이 아니라 terminal·deadline·turn이라는 공통 사용자 동작의 소유를 확인한 것이다. 새 build나 네 언어 contract 실행은 하지 않았다.

공통 동작의 차이를 새로 설계하는 안은 없다. B의 task slot/compiled frame은 C++ 내부 최적화이며 다른 언어에 같은 allocator를 도입할 필요는 없다. 다만 위의 race 시나리오와 wire golden 자료는 네 runtime에서 같은 결과가 나오는 contract로 검증해야 한다. 구현 중 public 완료 순서나 timeout 의미가 달라지면 B가 아니라 D가 되므로 PM 재판정 대상이다.

**6.6 µs와 스펙 변경의 관계**

현 자료는 ‘스펙을 고쳐야만 6.6에 도달한다’는 결론도 뒷받침하지 않는다. 필수 작업의 CPU 하한을 측정하지 않았기 때문이다. B는 스펙을 유지하는 구현 가능 구조이고 6.513은 그 구조의 공격적인 예산이다. 반면 현행 계약이 caller에게 binding 호출 이외의 일을 전혀 허용하지 않는 구조는 아니다. C1 등록과 동적 Framework wire 형성은 남는다. 다음은 목표 달성을 위해 자동 승인할 변경이 아니라, 그 사실을 삭제하려면 무엇을 바꿔야 하는지의 경계다.

| 삭제하고 싶은 일 | 실제로 바꿔야 하는 조항 | 현재 노출 비용과 해석 |
|---|---|---|
| 모든 Framework 등록을 없애고 binding pending만 사용 | S01 `:392–428`의 Framework OperationId/ReplyRouteId와 register-before-submit, `:447–463`의 logical C1/C2. Native registry 수명은 cancellation 문서 `:69–76`이 별도로 binding 소유로 규정 | registry I 0.4852 안의 일부가 직접 대상이다. 등록 분류 전체 4.4235는 상위 codec·task·본문까지 포함하므로 전부 회수할 수 없다. PM의 terminal owner 유지 조건을 위반한다. |
| Logical deadline index를 없애고 binding timeout만 사용 | S01 `:357–365`의 Ready/owner turn/admission 포함 절대 budget과 `:453–487`의 C2 단일 인덱스 | Ready 정상 경로의 index는 이미 무할당이며 시간은 registry I 0.4852 안에 포함된다. 삭제의 독립 µs는 미계측이다. 단순 spec 완화로 4.4 µs가 없어지지 않는다. |
| Registered receiver·typed projection 없이 binding 결과를 public 결과로 반환 | S01 `:331–333`의 최종 typed 수신자와 public 종료 뒤 physical 처리, `:515–519`의 Framework 완료 계약 | 결과 분류 4.2523 중 typed 결과 표현의 일부가 대상이다. 사용자 typed decode/harness 검증까지 없어지는 것이 아니므로 4.2523을 해제량으로 볼 수 없다. |
| Binding callback에서 사용자 continuation을 즉시 실행 | S01 `:496–519`와 S08 `:309–324`의 새 turn·수신 역할 분리 | Source completion worker와 wake 지연을 노출하지만 그것을 caller 4.2523의 전액 절감으로 셀 수 없다. 이 작업은 receive-role stack에서 user continuation 실행을 허용하지 않는다. |
| Nested multipart pack을 없애고 service header/JSON header/body를 각각 native part로 전송 | S wire `:186–211` 및 규범 `framework/runtime/protocol/service-wire-v1.schema.json`의 application payload/frame layout | 현행 pack 함수 I 0.5280와 관련 native allocation 표본 일부가 노출된다. I는 필요 길이 기록·최종 message 작업도 포함한다. 해제 가능한 독립 µs는 미계측이고 ≤0.5280 범위의 해당 함수 내부 CPU와 별도/중복 native 할당 비용을 구별해야 한다. 네 언어 wire/capability/golden 변경이 필요한 D다. |
| 모든 serializer를 caller 제공 최종 버퍼에 직접 encode | C++ exact serializer API 계약과 `serializer.hpp:216`의 현재 `serialize_fn_t` 반환형. Common payload codec 선택·오류 의미는 유지해야 함 | 재포장 body copy·임시 body storage 일부가 노출된다. 추가 sink 계약이 없는 사용자 codec에는 불가능하다. 기존 API만 쓰는 B는 이 절감을 보장하지 않으며 신규 public API는 별도 설계 승인 대상이다. |

위 행의 비용들은 서로 중첩되므로 더해서 ‘스펙 변경 시 몇 µs 확보’라고 보고하지 않는다. 정확한 µs가 필요한 항목은 미계측으로 표시했다. 현재 코드와 기존 프로파일에서 추론할 수 없는 숫자를 생성하는 것보다, 어떤 계약과 비용이 아직 분리되지 않았는지 PM이 알고 선택하는 것이 필요하다.

**Request-serial과 send-saturation에 미치는 영향**

Request-backpressure 1 KiB에서 6.513 µs면 caller 한 core의 산술 상한은 약 153,539 request/s다. 비교 binding 253,036 request/s를 사용하면 약 0.607이다. 6.6을 그대로 쓰면 약 0.599이므로 실제 caller 예산은 기존 crtc 계산의 6.587 µs가 더 정확하다. 이는 target/receive/completion 자원이 충분하다는 조건부 상한이다. Caller 목표만 만족했다고 모든 C++ cell의 ≥0.6을 판정할 수 없다. 보수적 비교 9.113 µs면 같은 상한 비율이 약 0.434에 그친다.

Request-serial은 동일 경로지만 window 1이므로 storage 배치 상각이 되지 않는다. B는 batch 충족을 기다리지 않으므로 새 지연을 만들지 않는다. Codec/결과 표현의 CPU 감소는 도움이 되지만 target handler scope·reply 시작, source의 독립 completion turn, 잠든 caller의 wake는 남는다. crtc의 1 KiB 간격은 128.644 µs이고 ≥0.6에 필요한 간격은 110.228 µs로 18.416 µs 감소가 필요하다. 이번 caller 목표 절감 10.967 µs가 전부 지연에 반영된다고 가정해도 117.677 µs이므로 그 셀의 0.6을 설명하지 못한다. 그 가정 자체도 실측 예측은 아니다. Target와 source completion의 구조 비용이 별도 남으며, 그것을 caller 절감에 이중 계산하지 않는다. 수신 역할 인계는 crtc가 이미 검토한 별도 실행 자원 배치 주제이므로 이 redesign의 절감에 합치지 않았다.

Send-saturation 1 KiB는 caller 약 4.702 µs, binding 약 1.3 µs다. Existing crtc의 ≥0.6 caller 예산은 2.578 µs이므로 약 2.124 µs를 더 줄여야 한다. 즉시 admission된 send에는 request C1 node·deadline entry·reply wait·request 결과 관찰 4.2523을 붙이지 않는다(S01 `:399–405`, S08 `:115–118`). 공통으로 적용되는 B의 준비된 outbound codec·최종 frame 구성·불필요한 socket 직렬화 제거·ready 결과 view가 감소 대상이다. Request 전용 task/registration 절감을 send 예상치에 옮겨 적지 않는다. HWM pending send는 binding이 가진 WRITABLE 결과를 기존 논리 완료 자리로 연결한다. 실제 pending 비율과 send의 함수별 비용이 없어 2.578 도달 예상값은 미확정이다.

4 KiB에서는 payload pack/copy의 byte 비용과 cache 영향이 달라진다. 1 KiB에서 설정한 각 절감 µs를 그대로 재사용하지 않는다. crtc 자료의 request-backpressure 4 KiB caller 예산 10.606 µs와 send 4 KiB 5.365 µs를 각각 별도로 판정해야 한다.

**완료 조건과 리팩토링 점검**

PM의 선택 대상은 대안 B다. 구현 후 최소 관련 검증으로 public request lifecycle과 binding 기능 위임을 먼저 확인하고, 별도 측정 담당 작업이 같은 harness·동일 payload/timeout/window 조건에서 caller CPU와 모든 cell의 ratio를 판정해야 한다. Caller CPU만 다른 worker로 이전한 결과, 제출/완료 수 불일치, late reply/payload 누락, 한 번의 고립 실패를 반복 통과로 무시한 결과는 완료가 아니다. 이 job에서 그 build·test·측정은 수행하지 않았다.

수명·할당 검사는 정상 입력에서 warmed 결과 저장/기본 observer/C1 node의 추가 heap 0과 Framework 결과 내부 shared_ptr 0을 관찰하고, deliberately ordinary storage 또는 중복 전달·조기 반환·잘못된 wire 입력을 넣었을 때 같은 검사가 실패해야 한다. Binding 자체의 bundle·native message·Core timeout 비용은 계층별로 따로 기록한다. Cold growth, 늦게 해제되는 public task, 추가 observer, caller 외 자원의 allocation도 숨기지 않는다. 총 source/target CPU와 메모리까지 확인해야 호출 thread 이동을 절감으로 오인하지 않는다.

소유 계층은 Framework의 logical operation·deadline·typed result·application gate와 binding/Core의 physical admission·native completion·retry·route·message 수명이다. 기준 조항은 S01 §9–11, S08 E1–E5/§4.1–4.4, lane §5, S gate §1–4/§13과 cancellation §3이다. 교차언어 대조에서 terminal take·deadline owner·별도 completion turn은 공통이며, 제안은 그 사용자 동작을 유지한다. 분류는 PR 1–2 B(중간 표현 비용 결함), PR 3의 transport owner 집중 A/B이며, C와 wire/공개 계약 삭제는 D로 기각했다.

수정 전/후 규칙 수의 설계 목표는 logical terminal 1→1, deadline 1→1, native admission 1→1, 최종 typed 전달 1→1, physical submit 직렬화 2→1이다. 새 terminal flag·pending 표·retry queue·deadline timer·poller·Framework capacity counter는 0개다. Pool·intrusive 수명 참조는 기존 allocator와 shared control block을 대체하며, 기존 요청 경로와 병행하는 두 번째 workflow는 남기지 않는다.

리팩토링 점검 결과는 구조 변경 묶음 3개를 제안하고 구현 수정 0개다. 각 묶음의 기존 메커니즘과 대체 소유자를 위 PR에 적었으며, 이미 완료된 #3957의 handler codec plan·borrowed header/body·invocation frame·C1 completion slot은 새 절감에서 제외했다. 남은 문제는 binding 분류 3.0203 µs의 인과별 미분리, 6.513 예산의 미검증, task·wrapper 수명 교체의 정확성 증명이다. 새 이슈·문서·코드 작업으로 넘기거나 변경하지 않았다.

보고서 수치 검사는 실행했다. 기존 두 caller의 단계 합·binding 재분류 합을 수락했고, registration 비용과 binding 묶음 비용을 각각 0.01 µs 변조한 자료는 모두 거부했다([검사 결과](profile-check-validation.json)). 이는 기존 자료의 회계 검증이며 runtime 성능·정확성 test 통과를 뜻하지 않는다.
