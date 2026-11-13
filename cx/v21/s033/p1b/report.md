지정 branch perf/1639-cpp-p1-receive-record에서 테스트 8개 파일만 수정했다. 제품 코드와 sample은 변경하지 않았다. 빌드, 테스트, 음성 대조 실행과 lock 획득은 하지 않았다. 컴파일 성공 여부는 PM의 빌드로 확인해야 한다.

아래 T는 framework/languages/cpp/tests/Zlink.Framework.UnitTests/다. 오류 위치는 수정 전 line이며, 달라진 현재 line은 괄호에 적었다.

(1) 오류별 변경

T/test_cpp_framework_reply_submit_claim.cpp:322의 brace 생성을 service_mailbox_record_t::from_parts로 바꿨다. owner spot:canonical과 parts {{1}, {2}}를 유지했다.
T/test_cpp_framework_reply_submit_claim.cpp:583의 brace 생성을 from_parts로 바꿨다. owner spot:race와 parts {{1}, {2}}를 유지했다.
T/test_cpp_framework_reply_submit_claim.cpp:614의 brace 생성을 from_parts로 바꿨다. owner spot:expired와 parts {{1}}를 유지했다.

T/test_cpp_framework_m6a_runtime.cpp:1751의 brace 생성을 from_parts로 바꿨다. owner-a와 parts {{1}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1757의 brace 생성을 from_parts로 바꿨다. owner-a와 parts {{2}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1760의 brace 생성을 from_parts로 바꿨다. owner-b와 parts {{3}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1764의 brace 생성을 from_parts로 바꿨다. owner-a와 parts {{6}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1773의 brace 생성을 from_parts로 바꿨다. owner-a와 parts {{1, 2, 3}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1776의 brace 생성을 from_parts로 바꿨다. owner-a와 parts {{4, 5}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1780(현재 1779)의 brace 생성을 from_parts로 바꿨다. peer-a, infrastructure domain과 parts {{9}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1783(현재 1782)의 brace 생성을 from_parts로 바꿨다. owner-b와 parts {{7}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1808의 brace 생성을 from_parts로 바꿨다. accounted와 parts {{1, 2, 3}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1813의 brace 생성을 from_parts로 바꿨다. accounted와 parts {{4}}를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1826의 brace 생성을 from_parts로 바꿨다. owner-large와 값 7인 20-byte part를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:1834(현재 1835)의 brace 생성을 from_parts로 바꿨다. first와 parts {{1}}를 유지했다. 위 application record의 domain, claim budget, byte accounting, FIFO와 settlement assertion도 유지했다.

T/test_cpp_framework_m6a_runtime.cpp:3093(현재 3094)의 request_record.reply_token을 reply_token()으로 바꿨다.
T/test_cpp_framework_m6a_runtime.cpp:3096(현재 3097)의 request_record.parts를 parts()로 바꿨다. header correlation 비교를 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:3156(현재 3157)의 actor_create_record.parts를 parts()로 바꿨다. operation과 actor ID 기대값을 유지했다.
T/test_cpp_framework_m6a_runtime.cpp:3417(현재 3418)의 record.reply_token을 reply_token()으로 바꿨다. 동시 request의 token 존재 assertion을 유지했다.
T/test_cpp_framework_mesh_metrics.cpp:563의 record.parts를 parts()로 바꿨다.
T/test_cpp_framework_mesh_metrics.cpp:566의 record.reply_token을 reply_token()으로 바꿨다. request, reply_route_id와 native token 존재 검사를 유지했다.
T/test_cpp_framework_client_server_runtime.cpp:1340의 record.parts[1]을 record.parts()[1]로 바꿨다. index별 payload 비교를 유지했다.
T/test_cpp_framework_route_mesh_weight_update.cpp:780의 received.parts[1]을 received.parts()[1]로 바꿨다. payload와 flow 검증을 유지했다.

T/test_cpp_framework_instance_spot_activation.cpp:1405–1407의 rt::auto recovered = mesh::...를 auto recovered = rt::mesh::...로 고쳤다. infrastructure domain, 빈 parts, source {2}, correlation 17과 reply 거부 assertion을 유지했다.
T/test_cpp_framework_receive_record.cpp:397(현재 396)의 private send_control 호출을 공개 send_to_node(target RID, payload())로 바꿨다. 정상 입력 수락 assertion을 유지했다.

(2) 추가 검색 결과

tests와 samples 전체에서 service_mailbox_record_t, 해당 alias, brace 인자와 .parts / ->parts / .reply_token / ->reply_token을 검색했다. source 파일 205개와 sample source 파일 139개를 대상으로 확인했다. 각 field 접근의 receiver 선언과 callback·반환 타입을 대조했다.

추가로 T/test_cpp_framework_actor_maintenance_join.cpp:4625의 빈 record brace 생성을 from_parts(owner, application, {})로 바꿨다. EXPECT_THROW의 std::invalid_argument 기대값을 유지했다. mailbox.try_enqueue의 인자가 service_mailbox_record_t&&임을 확인했다.

이 타입의 오래된 parts/token field 접근과 parts vector를 받는 brace 생성은 남아 있지 않다. host::receive_record_t, local_application_dispatch_t, operation_completion_t, raw_request_completion_t, route_received_packet_t, route_dispatch_reply_t, route_outbound_packet_t와 fixture 전용 reply state의 동명 field는 선언대로 유지했다. 빈 service_mailbox_record_t{}와 envelope_header만 지정하는 allocation fixture의 aggregate 생성도 유효하므로 유지했다. sample에서 수정할 항목은 없었다.

변경 전후 소스의 token을 비교했다. 허용된 factory·accessor·namespace·송신 호출 변경과 formatting을 제외한 token이 같았다. 변경한 파일의 assertion 수와 기대값을 유지했으며 테스트 삭제나 skip은 없다.

(3) receive_record의 관찰과 음성 대조

private 송신을 기존 friend에 추가하는 대안과 공개 send_to_node를 사용하는 대안을 비교했다. 이미 admit된 서로 다른 두 node 사이에서 정상 node-send를 보내는 검사이므로 공개 send_to_node를 선택했다. 이 호출은 encode_node_send_header_message와 encode_application_payload_message(payload())를 거쳐 같은 두 part를 전송한다. 기존 private 호출의 수동 header/body 조립을 제거했으며 새 friend, wrapper나 private 접근을 추가하지 않았다.

CountsAndOriginalTokenAcrossOneRotation은 기존 공개 request_to_node → begin_receive_turn → receive_next → mailbox claim/take → reply를 그대로 사용한다. 최초 native storage 관찰과 실제 service 분류·lease 계수는 이미 존재하는 ZLINK_FRAMEWORK_TEST_CREATION_PROBE의 receive_path_probe 경계를 유지했다. 요청 3건, body materialization 0, native body copy 0, classifications 3, port_leases 1과 pointer·RID·generation·원래 token·reply completion assertion을 모두 유지했다.

ZLINK_RECEIVE_RECORD_NEGATIVE=lease는 기존 receive_record_test_access_t::lease를 통해 실제 lease_receive_port를 한 번 더 호출한다. 첫 record에서 port_leases가 2가 되어 현재 280행의 기대값 1과 달라진다. record마다 lease를 다시 얻는 동작을 넣어도 같은 실제 획득 지점의 계수가 증가한다.

ZLINK_RECEIVE_RECORD_NEGATIVE=classification은 첫 record를 실제로 한 번 분류한 뒤 cache를 비운다. ingress가 같은 record를 다시 분류하므로 classifications가 4가 되어 현재 279행의 기대값 3과 달라진다. copy, clone, token, drop의 기존 음성 대조도 변경하지 않았다. malformed는 정상 request 두 part를 잘못된 입력 대신 보내므로 기대한 protocol_error와 application queue 0 검사가 실패하는 구조를 유지했다. 정상 node-send의 application 수락 검사도 남아 있다.

이 설명은 선언과 호출 흐름을 읽어 확인한 실패 조건이다. 정상·음성 대조의 실제 실행 결과를 주장하지 않는다.

(4) format 결과

bash scripts/format/format.sh cpp로 formatting을 적용했다. 이후 bash scripts/format/format.sh --check cpp가 exit 0으로 통과했다. git diff --check도 exit 0이었다. formatter 적용 후에도 변경 파일은 위 테스트 8개뿐이다.

(5) 리팩토링 점검

from_parts는 framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp:169에서 owner, domain, parts와 source/correlation/generation 등을 조립한다. 같은 field 조립을 직접 수행하는 기존 helper 8곳을 확인했다.

T/test_cpp_framework_mailbox_fifo_cost.cpp:25의 record()는 owner, domain과 한 part를 직접 조립한다.
T/test_cpp_framework_receive_allocations.cpp:57의 infrastructure_record()는 owner, domain, parts, source와 generation을 직접 조립한다.
T/test_cpp_framework_claim_dispatch_cost.cpp:96의 enqueue()는 raw_request 분기에서 owner, domain, parts, source, correlation과 before callback을 직접 조립한다. local application 분기는 factory의 parts 조립과 별개다.
T/test_cpp_framework_actor_maintenance_join.cpp:3592의 actor_record()는 wire 생성 후 owner, domain, parts, source와 generation을 직접 조립한다.
T/test_cpp_framework_relocation_seal_contract.cpp:190의 spot_send()는 owner, domain, parts, source, correlation과 generation을 직접 조립한다. source_original 설정은 이 helper의 추가 역할이다.
T/test_cpp_framework_relocation_seal_contract.cpp:1596의 record lambda는 owner, domain과 parts를 직접 조립한다.
T/test_cpp_framework_receive_allocations.cpp:393의 enqueue lambda는 owner, domain과 parts를 직접 조립한다. envelope_header 설정은 별도 역할이다.
T/test_cpp_framework_local_enqueue_allocations.cpp:790의 enqueue lambda는 owner, domain, parts, source와 generation을 직접 조립한다.

점검 결과 조립 중복 8곳을 보고했다. 이번 요청의 stale 생성·접근 수정 외에 이 helper들을 재작성하지 않았다. 새 helper·상태·queue·lock은 추가하지 않았다. 제품의 service 분류 판정 위치 수는 1→1, 회전 lease 획득 위치 수는 1→1로 유지했다.
