#1639 step 1 PR #4155 수정 보고

작업 branch는 perf/1639-cpp-p1-receive-record입니다. 차단 항목 두 건과 friend 정리를 수정했습니다. 빌드, 테스트, 음성 대조, lock 획득, 성능 측정은 실행하지 않았습니다. 아래 복사·할당 수는 코드에서 산정한 값과 새 시험의 기대값이며 실행 측정값이 아닙니다.

경로 C는 framework/languages/cpp/framework/src/runtime/이고 T는 framework/languages/cpp/tests/Zlink.Framework.UnitTests/입니다. 검사 목록에는 저장소 기준 전체 경로를 씁니다.

(1) service-header cache의 유효성

parts 교체를 record가 소유하는 방식을 선택했습니다. C/mesh/service_mailbox.hpp:222의 parts()는 const vector만 반환합니다. :224의 body_parts()는 첫 service-prefix frame을 제외한 mutable span을 반환합니다. 따라서 본문을 이동하거나 바꿔도 prefix는 바뀌지 않습니다. 전체 교체는 :228의 replace_parts()만 수행하고 두 cache를 함께 지웁니다. :234의 take_parts()도 이 경로로 cache를 지웁니다. _received와 두 cache는 :258 이후 private입니다. native ingress만 friend로 새 record에 수신하며 그 위치는 C/mesh/raw_mesh_node_owner.cpp:3606입니다. from_received()도 새 record만 만듭니다.

유효성 판정 위치는 수정 전 1곳, 수정 후 1곳입니다. 두 버전 모두 C/mesh/service_mailbox.cpp:27의 cache 존재 검사입니다. service-prefix 해석과 application-kind 분류 위치도 수정 전 1곳, 수정 후 1곳이며 :32의 decode와 같은 함수의 switch가 소유합니다. 무효화 위치는 제품 코드 0→1곳, 시험의 직접 reset 1→0곳입니다. 전체 코드에서 reset을 지시하는 위치는 1→1곳으로 시험에서 제품의 replace_parts()로 옮겼습니다. 교체마다 호출자가 유효성을 판단하는 규칙은 추가하지 않았습니다. 기존 버전에는 parts 교체 뒤 무효화가 없었으므로 제품의 무효화 위치 수가 줄었다고 주장하지 않습니다.

ingress가 분류 값을 따로 전달하는 대안은 native 수신과 local wire 생산자에 분류 결과 전달 경로를 따로 마련해야 합니다. 선택한 방식은 기존 분류 위치 하나를 유지하고 prefix 변경 권한을 한곳에 둡니다. cache를 감추려고 별도 storage wrapper를 추가하지 않았습니다. 그 결과 record는 aggregate 속성을 잃습니다. framework/languages/cpp/tests/support/creation_counter.hpp:25의 aggregate 단언은 기본 생성 가능성 단언으로 바꿨고 :22의 복사 금지와 :24의 move 단언은 유지했습니다.

T/test_cpp_framework_receive_record.cpp:377에 nodeSend→nodeRequest→reply→잘못된 prefix 교체 시험을 추가했습니다. application kind 변경, application kind 해제, 잘못된 prefix의 예외와 분류 횟수를 확인합니다. 본문만 바꿀 때 분류 횟수가 늘지 않는지도 확인합니다.

기존 ZLINK_RECEIVE_RECORD_NEGATIVE=classification은 T/test_cpp_framework_receive_record.cpp:131에서 먼저 분류한 다음 replace_parts(take_parts())로 같은 parts를 공개 경로로 재설정합니다. 이어지는 ingress가 다시 분류하므로 세 record의 분류 횟수가 4가 되어 :281의 기대값 3을 깨뜨리는 구조입니다. cache 필드에 직접 접근하지 않습니다. 이 실패는 실행 확인 전입니다.

(2) payload 없는 reply source

bindings/cpp/include/zlink/Contracts/Messaging/received.hpp:73 이후의 공개 API와 reply_token_t, 같은 파일의 private 생성자, bindings/cpp/include/zlink/Contracts/Core/routing_id.hpp의 저장 형태를 확인했습니다. received_t에는 metadata만 생성하거나 추출하는 공개 API가 없습니다. typed RID는 inline 저장소이고 token 복사는 기존 shared owner를 보유하며 새 할당을 하지 않습니다. binding 내부 API를 Framework에서 쓰지 않았습니다.

C/mesh/service_mailbox.hpp:150의 service_mailbox_reply_target_t가 optional typed RID, 원래 token, route generation과 wire reply에 필요한 correlation만 보유합니다. parts, application, claim, permit, callback, lookup과 logical RID vector는 보유하지 않습니다. C/mesh/service_mailbox.cpp:72의 생성자 한곳에서 record를 target으로 바꿉니다. native RID가 없는 local original은 기존 source_rid()를 통해 typed target으로 바꿉니다. :84의 reply_source()는 이 작은 target 하나만 make_shared합니다. 수신 record에는 별도 reply metadata 저장소를 추가하지 않았습니다.

metadata만 쓰는 reply 함수는 이 target을 받습니다. 동기 함수는 const reference를 받아 retained target을 다시 복사하지 않습니다. prepare_relocation_ready_reply()는 coroutine이므로 값으로 받아 임시 변환 객체의 수명을 보유합니다. 원래 prefix를 확인하는 reply_bound_session_bind()는 record를 받은 뒤 reply_infrastructure()에 target을 전달합니다. native reply 라우팅과 token 검증은 기존 binding의 typed reply builder가 계속 소유합니다.

N개 parts와 nonempty logical RID를 가진 materialized 수신 record 기준으로, 수정 전 reply_source()는 native message handle 복사 N회와 C++ 할당 3회(shared record, parts vector, logical RID vector)가 필요했습니다. 수정 후는 native message handle 복사 0회와 C++ 할당 1회(shared target)입니다. RID와 token은 metadata 값만 복사하며 route generation과 correlation도 그대로 보유합니다. payload bytes의 deep-copy 수나 CPU 개선은 측정하지 않았습니다.

T/test_cpp_framework_receive_record.cpp:330의 ReplySourceRetainsOnlyTargetWithoutPartCopies가 count_allocations와 native_call_scope_t를 reply_source() 호출에만 걸어 할당 1회와 native copy 0회를 단언합니다. 원래 parts가 남아 있는지, RID·token·generation·correlation이 같은지도 확인합니다. 원래 record를 파괴하고 mailbox claim을 해제한 뒤 실제 reply completion까지 확인하는 시험입니다. framework/languages/cpp/CMakeLists.txt:3058에 기존 allocation_counter.cpp를 연결했습니다.

새 음성 대조는 ZLINK_RECEIVE_RECORD_NEGATIVE=reply-source입니다. 계수 구간에서 원래 두 parts를 복사하고 버립니다. native copy 2회와 C++ 할당 2회가 되어 기대값 0회와 1회를 모두 깨뜨리는 구조입니다. 정상·음성 대조 모두 실행 확인 전입니다.

(3) friend 정리

raw_mesh_node_owner.hpp 앞부분의 receive_record_test_access_t friend를 없앴습니다. 기존 C/mesh/raw_mesh_node_owner.hpp:534의 raw_mesh_node_owner_test_access_t만 사용합니다. T/test_cpp_framework_receive_record.cpp:33의 access 정의와 :135, :448의 lease 호출을 이 이름으로 바꿨습니다. 이 클래스의 test-access friend 수는 2→1곳입니다.

(4) caller와 시험 변경

C/channels/channel_host_service.cpp:297은 from_received()로 생성합니다. :158, :303, :376, :420의 전체 이동은 take_parts(), :373의 전체 교체는 replace_parts()로 바꿨습니다.

C/client_server/client_server_location_runtime.cpp:1418과 C/stateful/public_host_runtime.cpp:8325, :8424는 본문만 body_parts()로 이동합니다. C/mesh/raw_mesh_node_owner.cpp:3302도 본문만 수정합니다. C/stateful/public_host_runtime.cpp:8009와 C/stateful/raw_stateful_dispatch.cpp:368, :1361의 전체 제거는 replace_parts({})로 바꿨습니다. C/client_server/raw_client_server_owner.cpp:515와 native mesh 수신부의 RID·generation 읽기는 encapsulated accessor로 바꿨습니다.

C/stateful/public_host_runtime.cpp:6835, :6884, :6923, :7254, :7287의 reply_source() 호출은 모두 새 shared target을 받습니다. :5175, :5460의 Instance Spot coroutine 인자와 :7291의 waiter vector, 대응하는 public_host_runtime.hpp:987, :991, :1386의 선언도 target으로 바꿨습니다. C/stateful/raw_stateful_dispatch.cpp:1268, :1292도 같은 타입을 사용합니다. reply 함수 선언과 정의는 C/mesh/raw_mesh_node_owner.hpp 및 .cpp에서 함께 바꿨습니다. 기존 record 인자는 target 생성자로 변환되므로 원래 동기 reply 호출들의 인자 순서와 기본 인자는 유지됩니다.

기존 reply_source() 시험 caller는 T/test_cpp_framework_claim_dispatch_cost.cpp:513, T/test_cpp_framework_m6a_runtime.cpp:3645, :4078, :4581, T/test_cpp_framework_m6b_runtime.cpp:3008입니다. claim-dispatch 시험은 typed source_rid와 route generation을 확인하도록 바꿨습니다. 새 target 타입에는 이전에 비어 있음을 확인하던 application·parts·permit 등의 멤버 자체가 없습니다. native copy·할당 단언은 새 count 시험이 담당합니다. m6a의 명시 shared_ptr 타입(:3641)과 correlation 음성 시험의 const_pointer_cast(:4077)를 target으로 바꿨습니다. auto로 받는 나머지 caller는 소스 수정 없이 새 타입을 받으며 사용하는 reply 함수도 target을 받습니다.

T/test_cpp_framework_m6c_runtime.cpp:4732와 T/test_cpp_framework_application_job_queue.cpp:487은 전체 parts 재설정을 replace_parts()로 바꿨습니다. T/test_cpp_framework_handler_entry_permit.cpp:156, T/test_cpp_framework_claim_dispatch_cost.cpp:867, T/test_cpp_framework_reply_submit_claim.cpp:455의 생성도 전체 교체 한 번으로 바꿨습니다.

T/test_cpp_framework_actor_maintenance_join.cpp:3899, :3944, :4337과 T/test_cpp_framework_relocation_seal_contract.cpp:226, :248, :3718은 body_parts()를 씁니다. relocation-seal 시험의 :249, :2380은 replace_parts({})를 씁니다. receive-record 시험의 직접 native storage 읽기와 close도 accessor와 replace_parts()로 바꿨습니다.

framework/languages/cpp/{framework,tests,samples,cross-language}에서 parts 변경, native received 접근, cache 직접 접근, reply_source()와 명시 pointer 타입을 검색했습니다. samples와 cross-language에는 추가 변경이 필요한 caller가 없었습니다. 제품의 직접 native 수신 쓰기는 새 ingress record에 대한 한곳만 남고 시험의 cache 직접 reset은 없어졌습니다.

(5) 괄호·인자 정적 대조

변경된 C++ 20개 파일 전체를 comment·문자열·raw string을 제외한 token으로 읽어 (), [], {}의 짝과 중첩을 확인했습니다. 변경 구간과 겹치는 관련 호출·선언 160곳의 인자 수를 선언과 대조했습니다. 이 검사는 컴파일이나 타입 검사가 아닙니다.

parts·body_parts·take_parts·routing_id·route_generation·reply_token·source_rid·service_header·application_kind·reply_source는 인자 0개입니다. replace_parts는 1개, from_received는 3개, record의 from_parts는 필수 3개와 선택 6개입니다. application_payload_t::from_parts는 1개입니다. decode_application_parts(message_t&&, bool)는 1개 또는 2개, encode_actor_message_header는 필수 4개와 선택 3개입니다. materialize_binding_parts는 1개, receive_if_ready와 is_route_connection 및 encode_route_connection_id는 2개, admit_descriptor_record는 5개입니다.

reply는 2개, reply_failure는 3개, prepare_relocation_ready_reply와 reply_relocation_failed 및 reply_infrastructure는 2개입니다. reply_user_spot_create와 reply_actor_create는 2개 또는 3개, reply_actor_join은 5개부터 8개, reply_instance_spot_activation은 3개 또는 4개, reply_user_spot_close는 2개입니다. dispatch_instance_spot_activation은 2개 또는 3개이고 core 함수는 5개입니다. 대응하는 header와 definition의 타입·순서·기본 인자를 대조했습니다.

추가로 C/mesh/service_mailbox.cpp:86의 make_shared<service_mailbox_reply_target_t>(*this)는 인자 1개와 괄호 1쌍이며 생성자 선언 :152와 맞습니다. m6a_runtime.cpp:4077의 const_pointer_cast도 인자 1개와 괄호 1쌍입니다. CMakeLists.txt:3058의 target_sources는 target, PRIVATE, 두 source 항목과 괄호 1쌍을 확인했습니다.

기존 deferred caller 여섯 곳의 reply_source는 모두 인자 0개와 괄호 1쌍입니다. public_host_runtime.cpp:6843의 reply_actor_create는 3개, :6892의 reply_actor_join은 8개, :6924의 dispatch_instance_spot_activation은 2개, :7254의 emplace_back은 2개, :7305의 reply_user_spot_close는 2개입니다. raw_stateful_dispatch.cpp:1300의 reply는 2개, :1302의 reply_failure는 3개입니다. 제어 흐름을 포함한 해당 함수 전체의 괄호도 확인했습니다.

아래 목록의 n은 바깥 호출·선언의 인자 수입니다. p, s, b는 그 표현 안의 (), [], {} 쌍 수이며 내부 호출을 포함합니다. 각각의 여는 괄호와 닫는 괄호 개수는 같습니다.

framework/languages/cpp/framework/src/runtime/channels/channel_host_service.cpp 전체는 p=606, s=37, b=168입니다.
확인한 위치는 157 report_dispatch_rejection: n=1, p=3, s=0, b=0; 158 message_parts_t: n=1, p=2, s=0, b=0; 158 take_parts: n=0, p=1, s=0, b=0; 297 from_received: n=3, p=3, s=0, b=0; 298 application_owner: n=2, p=1, s=0, b=0; 303 message_parts_t: n=1, p=2, s=0, b=0; 303 take_parts: n=0, p=1, s=0, b=0; 373 replace_parts: n=1, p=3, s=0, b=0; 376 report_dispatch_rejection: n=1, p=3, s=0, b=0; 376 message_parts_t: n=1, p=2, s=0, b=0; 376 take_parts: n=0, p=1, s=0, b=0; 420 message_parts_t: n=1, p=2, s=0, b=0; 420 take_parts: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/client_server/client_server_location_runtime.cpp 전체는 p=1338, s=129, b=364입니다.
확인한 위치는 1418 body_parts: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.cpp 전체는 p=963, s=104, b=332입니다.
확인한 위치는 514 is_route_connection: n=2, p=2, s=0, b=0; 515 route_generation: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp 전체는 p=2808, s=123, b=810입니다.
확인한 위치는 2017 prepare_relocation_ready_reply: n=2, p=1, s=0, b=0; 2020 reply_token: n=0, p=1, s=0, b=0; 2031 reply_relocation_failed: n=2, p=1, s=0, b=0; 2034 reply_token: n=0, p=1, s=0, b=0; 2046 reply_token: n=0, p=1, s=0, b=0; 2065 routing_id: n=0, p=1, s=0, b=0; 2070 routing_id: n=0, p=1, s=0, b=0; 3048 reply_infrastructure: n=2, p=1, s=0, b=0; 3051 routing_id: n=0, p=1, s=0, b=0; 3055 routing_id: n=0, p=1, s=0, b=0; 3074 reply_user_spot_create: n=3, p=1, s=0, b=0; 3097 routing_id: n=0, p=1, s=0, b=0; 3122 reply_actor_create: n=3, p=1, s=0, b=0; 3137 routing_id: n=0, p=1, s=0, b=0; 3151 reply_actor_join: n=8, p=1, s=0, b=0; 3161 reply_token: n=0, p=1, s=0, b=0; 3182 reply_instance_spot_activation: n=4, p=1, s=0, b=0; 3198 reply_token: n=0, p=1, s=0, b=0; 3199 routing_id: n=0, p=1, s=0, b=0; 3206 routing_id: n=0, p=1, s=0, b=0; 3222 reply_user_spot_close: n=2, p=1, s=0, b=0; 3302 body_parts: n=0, p=1, s=0, b=0; 3606 receive_if_ready: n=2, p=1, s=0, b=0; 3622 routing_id: n=0, p=1, s=0, b=0; 3660 routing_id: n=0, p=1, s=0, b=0; 3666 is_route_connection: n=2, p=2, s=0, b=0; 3667 route_generation: n=0, p=1, s=0, b=0; 3677 admit_descriptor_record: n=5, p=5, s=0, b=0; 3678 route_generation: n=0, p=1, s=0, b=0; 3718 route_generation: n=0, p=1, s=0, b=0; 3719 encode_route_connection_id: n=2, p=2, s=0, b=0; 3719 route_generation: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.hpp 전체는 p=184, s=0, b=47입니다.
확인한 위치는 403 reply_user_spot_create: n=3, p=1, s=0, b=0; 407 reply_user_spot_create: n=3, p=1, s=0, b=0; 416 reply_actor_create: n=3, p=1, s=0, b=0; 420 reply_instance_spot_activation: n=4, p=1, s=0, b=0; 425 reply_user_spot_close: n=2, p=1, s=0, b=0; 454 prepare_relocation_ready_reply: n=2, p=1, s=0, b=0; 456 reply_relocation_failed: n=2, p=1, s=0, b=0; 468 reply_actor_join: n=8, p=1, s=0, b=0; 688 reply_infrastructure: n=2, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp 전체는 p=327, s=11, b=75입니다.
확인한 위치는 72 service_mailbox_reply_target_t: n=1, p=1, s=0, b=0; 75 routing_id: n=0, p=1, s=0, b=0; 76 reply_token: n=0, p=1, s=0, b=0; 77 route_generation: n=0, p=1, s=0, b=0; 80 source_rid: n=0, p=1, s=0, b=0; 84 reply_source: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp 전체는 p=185, s=3, b=72입니다.
확인한 위치는 153 service_mailbox_reply_target_t: n=1, p=1, s=0, b=0; 154 routing_id: n=0, p=1, s=0, b=0; 155 reply_token: n=0, p=1, s=0, b=0; 156 route_generation: n=0, p=1, s=0, b=0; 157 source_rid: n=0, p=1, s=0, b=0; 202 replace_parts: n=1, p=2, s=0, b=0; 218 routing_id: n=0, p=1, s=0, b=0; 219 routing_id: n=0, p=1, s=0, b=0; 224 body_parts: n=0, p=1, s=0, b=0; 228 replace_parts: n=1, p=1, s=0, b=0; 234 take_parts: n=0, p=1, s=0, b=0; 237 replace_parts: n=1, p=1, s=0, b=1; 240 routing_id: n=0, p=1, s=0, b=0; 242 routing_id: n=0, p=1, s=0, b=0; 244 route_generation: n=0, p=1, s=0, b=0; 244 route_generation: n=0, p=1, s=0, b=0; 247 reply_token: n=0, p=1, s=0, b=0; 251 routing_id: n=0, p=1, s=0, b=0; 251 routing_id: n=0, p=1, s=0, b=0; 256 reply_source: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp 전체는 p=5704, s=393, b=1712입니다.
확인한 위치는 5173 dispatch_instance_spot_activation: n=3, p=2, s=0, b=0; 5458 dispatch_instance_spot_activation_core: n=5, p=2, s=0, b=0; 8009 replace_parts: n=1, p=1, s=0, b=1; 8325 decode_application_parts: n=1, p=4, s=0, b=0; 8326 body_parts: n=0, p=1, s=0, b=0; 8424 decode_application_parts: n=2, p=4, s=1, b=0; 8425 body_parts: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp 전체는 p=397, s=5, b=134입니다.
확인한 위치는 985 dispatch_instance_spot_activation: n=3, p=2, s=0, b=1; 989 dispatch_instance_spot_activation_core: n=5, p=2, s=0, b=0입니다.

framework/languages/cpp/framework/src/runtime/stateful/raw_stateful_dispatch.cpp 전체는 p=1157, s=58, b=380입니다.
확인한 위치는 368 replace_parts: n=1, p=1, s=0, b=1; 1361 replace_parts: n=1, p=1, s=0, b=1입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_maintenance_join.cpp 전체는 p=8086, s=332, b=1533입니다.
확인한 위치는 3899 decode_application_parts: n=1, p=4, s=0, b=0; 3899 body_parts: n=0, p=1, s=0, b=0; 3944 decode_application_parts: n=1, p=4, s=0, b=0; 3944 body_parts: n=0, p=1, s=0, b=0; 4337 decode_application_parts: n=1, p=4, s=0, b=0; 4337 body_parts: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_application_job_queue.cpp 전체는 p=1383, s=95, b=258입니다.
확인한 위치는 487 replace_parts: n=1, p=11, s=0, b=3; 487 materialize_binding_parts: n=1, p=10, s=0, b=3; 493 encode_application_payload: n=1, p=3, s=0, b=0; 494 from_parts: n=1, p=2, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp 전체는 p=741, s=63, b=188입니다.
확인한 위치는 514 source_rid: n=0, p=1, s=0, b=0; 518 route_generation: n=0, p=1, s=0, b=0; 518 route_generation: n=0, p=1, s=0, b=0; 867 replace_parts: n=1, p=7, s=0, b=2; 867 encode_actor_message_header: n=4, p=1, s=0, b=1; 869 encode_application_payload: n=1, p=3, s=0, b=0; 870 from_parts: n=1, p=2, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_handler_entry_permit.cpp 전체는 p=203, s=10, b=59입니다.
확인한 위치는 156 replace_parts: n=1, p=7, s=0, b=1; 157 encode_node_send_header: n=0, p=1, s=0, b=0; 158 encode_application_payload: n=1, p=3, s=0, b=0; 159 from_parts: n=1, p=2, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp 전체는 p=4823, s=114, b=679입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6c_runtime.cpp 전체는 p=4977, s=478, b=1830입니다.
확인한 위치는 4732 replace_parts: n=1, p=3, s=0, b=1; 4732 materialize_binding_parts: n=1, p=2, s=0, b=1; 4733 encode_reply_relay: n=1, p=1, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_receive_record.cpp 전체는 p=432, s=23, b=82입니다.
확인한 위치는 121 routing_id: n=0, p=1, s=0, b=0; 121 reply_token: n=0, p=1, s=0, b=0; 122 route_generation: n=0, p=1, s=0, b=0; 133 replace_parts: n=1, p=2, s=0, b=0; 133 take_parts: n=0, p=1, s=0, b=0; 135 lease: n=1, p=1, s=0, b=0; 266 routing_id: n=0, p=1, s=0, b=0; 267 route_generation: n=0, p=1, s=0, b=0; 318 replace_parts: n=1, p=1, s=0, b=1; 340 routing_id: n=0, p=1, s=0, b=0; 341 reply_token: n=0, p=1, s=0, b=0; 342 route_generation: n=0, p=1, s=0, b=0; 355 reply_source: n=0, p=1, s=0, b=0; 362 routing_id: n=0, p=1, s=0, b=0; 363 reply_token: n=0, p=1, s=0, b=0; 364 route_generation: n=0, p=1, s=0, b=0; 383 from_parts: n=3, p=4, s=0, b=1; 385 encode_node_send_header: n=0, p=1, s=0, b=0; 387 service_header: n=0, p=1, s=0, b=0; 388 application_kind: n=0, p=1, s=0, b=0; 390 body_parts: n=0, p=1, s=0, b=0; 391 service_header: n=0, p=1, s=0, b=0; 393 replace_parts: n=1, p=4, s=0, b=1; 393 encode_node_request_header: n=1, p=1, s=0, b=0; 395 service_header: n=0, p=1, s=0, b=0; 396 application_kind: n=0, p=1, s=0, b=0; 399 replace_parts: n=1, p=3, s=0, b=1; 399 encode_reply_header: n=3, p=1, s=0, b=0; 400 service_header: n=0, p=1, s=0, b=0; 401 application_kind: n=0, p=1, s=0, b=0; 403 replace_parts: n=1, p=2, s=0, b=1; 404 service_header: n=0, p=1, s=0, b=0; 417 route_generation: n=0, p=1, s=0, b=0; 431 route_generation: n=0, p=1, s=0, b=0; 448 lease: n=1, p=1, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_seal_contract.cpp 전체는 p=3119, s=247, b=901입니다.
확인한 위치는 226 body_parts: n=0, p=1, s=0, b=0; 248 decode_application_parts: n=1, p=4, s=0, b=0; 248 body_parts: n=0, p=1, s=0, b=0; 249 replace_parts: n=1, p=1, s=0, b=1; 2380 replace_parts: n=1, p=1, s=0, b=1; 3718 body_parts: n=0, p=1, s=0, b=0입니다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_reply_submit_claim.cpp 전체는 p=497, s=17, b=131입니다.
확인한 위치는 455 replace_parts: n=1, p=2, s=0, b=1입니다.

framework/languages/cpp/tests/support/creation_counter.hpp 전체는 p=32, s=2, b=8입니다.

(6) format과 실행 경계

bash scripts/format/format.sh --check cpp는 최종 수정 후 exit 0입니다. git diff --check도 exit 0입니다. 이 작업에서 build, test, negative control, lock, Core header 읽기, internal-docs.sh, GitHub 작업, commit과 push는 하지 않았습니다. 실행 실패는 수집하지 않았으며 PM 머신의 컴파일과 정상·음성 대조 실행이 남아 있습니다.

(7) 리팩토링 점검

점검 범위는 service_mailbox_record_t와 이번 receive→reply 호출 경로입니다. _application_kind(C/mesh/service_mailbox.hpp:266)는 _service_header(:265)에서 파생되는 두 번째 cache 값이며 같은 replace_parts()가 함께 무효화합니다. 이 두 값 이외에 canonical service record의 parts를 읽고 lazy하게 갱신되는 cache는 찾지 못했습니다.

parts에서 읽어 admission 시 고정하는 다른 값은 owner와 domain(C/mesh/raw_mesh_node_owner.cpp:3641), correlation(:3643), operation(:3645), bound_session_source(:3646)입니다. 실제 header 해석은 같은 파일 :4168, :4185 이후와 :4200 이후에 있습니다. Client/Server record의 envelope_header는 C/client_server/raw_client_server_owner.cpp:521과 :666에서 보관합니다. 이 값들은 admission 결과의 snapshot이며 이번 변경으로 재해석하지 않습니다. replace_parts()는 이 admission metadata를 재작성하는 API가 아니므로, 새 논리 메시지를 만들 때는 기존 from_parts()/from_received() 경로를 사용해야 합니다. source_node_generation은 일반 native 경로에서 parts가 아니라 admitted descriptor에서 가져옵니다(C/mesh/raw_mesh_node_owner.cpp:4219). source_original도 source lookup의 snapshot입니다(:4223).

인접한 claimed_ingress_t에도 service_header와 body_bytes snapshot이 있습니다(C/stateful/raw_stateful_dispatch.hpp:73, :79). C/stateful/raw_stateful_dispatch.cpp:622에서 header/body native view를 각각 보유하고 :625에서 body size를 계산하며 :627에서 record의 분류 값을 전달합니다. 이것은 독립된 claim view와 함께 보관하는 기존 snapshot이고 이번 작업에서 별도 cache 판정을 추가하지 않았습니다.

reply source의 whole received_t 복사와 payload close는 제거했습니다. 같은 reply 호출 경로에서 다른 whole service_mailbox_record_t 복사는 발견하지 못했습니다. 이 타입의 copy 금지 단언도 유지됩니다. C/stateful/public_host_runtime.cpp:4499와 :8013은 std::move로 원래 record를 보유하며 :8377은 batch에 대한 alias shared_ptr입니다. 복사가 아닙니다. C/stateful/raw_stateful_dispatch.cpp:833의 pending.transport 대입도 move입니다.

C/client_server/raw_client_server_owner.cpp:543에는 received_t control = received라는 whole native record 복사가 남습니다. 이 위치는 consume_control의 control-plane 소유권 전달이며 PM 리뷰에서 변경 불필요로 지정한 기존 비용입니다. deferred reply_source의 payload 복사와 구분하여 유지했습니다. claimed_ingress의 header/body.copy() 두 곳(:622, :623)도 whole-record 복사가 아닌 기존 native view 보유입니다.

소유 계층은 service 분류·parts 표현의 Framework record와 native reply 라우팅·token 검증의 binding입니다.
Spec 근거는 01-execution/08-messaging-hot-path.ko.md §4.1의 I3·I4와 bindings/doc/spec/README.ko.md의 소유권 정책이며 wire 계약은 02-channel-transport/06-wire-protocol.ko.md를 유지합니다.
교차언어 대조에서 .NET ZLinkMeshDispatchPump.cs:718은 typed kind와 CaptureReplyRoute()를 전달하고 FrameworkServiceTypes.cs:946은 payload와 별개 reply callback을 보유합니다. Java ZLinkJavaRawMeshNode.java:5937의 ClaimedRecord가 header를 보유하며 :5979에서 해석하고 :6030에서 재사용합니다. Kotlin은 framework/languages/java/zlink-framework-kotlin/build.gradle.kts:13에서 같은 Java core를 공유합니다. 이번 수정은 C++ 내부 소유 표현의 기존 결함을 고치며 언어별 public 동작 차이나 spec 개정은 도입하지 않습니다.
변경 분류는 B입니다.

수정 전/후 판정 위치 수는 cache 유효성 1→1, 분류 1→1, cache reset 소유 위치는 시험 포함 1→1(제품 0→1, 시험 1→0), reply_source의 payload 복사·폐기 1→0, test-access friend 2→1입니다. 새 queue·lock·timer·thread·제어 flag는 추가하지 않았습니다. 추가한 reply target은 whole reply-source record를 대체하며 수신 record에 중복 metadata를 보관하지 않습니다. 점검 결과는 요청된 세 항목 수정, 추가 결함 발견 0건, 별도 이관 0건입니다. 기존 admission snapshot과 control-copy 위치는 위에 명시했습니다.
