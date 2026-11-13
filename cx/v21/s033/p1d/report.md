기록을 가진 13개 지정 지점을 from_parts 호출로 바꿨고, 각 필드 값을 유지했다. 나머지 한 지점은 parts 설정이 조건부라 기존 조립을 유지했다.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_reply_submit_claim.cpp:446은 local일 때 application.parts를, 아닐 때 record.parts를 채우는 조건부 조립이므로 유지했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_seal_contract.cpp:1645는 owner, domain과 materialize_binding_parts 결과를 from_parts에 전달했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_seal_contract.cpp:1674는 owner, domain과 materialize_binding_parts 결과를 from_parts에 전달했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_seal_contract.cpp:2915는 application domain, source routing ID, generation과 parts를 from_parts로 전달했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_seal_contract.cpp:3485는 infrastructure domain, source routing ID, generation과 parts를 from_parts로 전달했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp:890은 node owner, application domain, source, no-op handler와 parts를 from_parts로 전달했다. 기존 parts 대입과 같은 initializer-list 생성이며 추가 복사는 없다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:5565는 owner, application domain, source, generation과 parts를 from_parts로 전달했다. materialize_binding_parts 결과는 by-value 인수로 바로 전달되어 추가 복사가 없다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:5631은 빈 owner, application domain, 조건별 source와 generation 및 parts를 from_parts에 전달하고 source_original은 기존처럼 별도 대입했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp:1692는 기존 단일 parts push_back을 유지하는 임시 vector를 만든 뒤 std::move로 from_parts에 넘겨 추가 복사를 피했다. 비용 측정 구간은 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp:443은 malformed record를 from_parts로 만들고 permit 설정은 별도로 유지했다. measure_allocations 구간 밖이며 vector를 std::move해 추가 복사가 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp:510은 reply source fixture를 from_parts로 만들었다. measure_allocations 구간 밖이며 vector를 std::move해 추가 복사가 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp:549는 multicast fixture를 from_parts로 만들고 permit 설정은 별도로 유지했다. measure_allocations 구간 밖이며 vector를 std::move해 추가 복사가 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp:742는 prestart terminal fixture를 from_parts로 만들었다. measure_allocations 구간 밖이며 vector를 std::move해 추가 복사가 없다.
framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_maintenance_join.cpp:4617은 malformed record의 owner, application domain과 parts를 from_parts로 전달했다. 비용 측정 구간은 없다.

추가로 같은 파일 안의 mesh_node_vertical.cpp:847 multicast record도 from_parts로 바꿨다. 이 지점은 지정 목록 밖에서 찾은 무조건부 parts 조립이었다.

추가 검색에서 남은 조건부 조립은 reply_submit_claim.cpp:455와 claim_dispatch_cost.cpp:867이다. 둘 다 조건에 따라 application.parts 또는 record.parts를 구성하므로 유지했다. handler_entry_permit.cpp:156-159는 queued(owner)가 permit을 예약하고 queued 상태로 만든 뒤 parts를 추가하며 해당 파일은 수정 허용 목록 밖이므로 남겼다. application_job_queue.cpp:487은 이미 claim된 wire record의 parts를 바꾸는 테스트 조작이고, m6c_runtime.cpp:4732는 from_parts로 만든 record의 wire parts를 교체하는 테스트 조작이라 hand assembly 잔여로 세지 않았다.

bash scripts/format/format.sh cpp와 bash scripts/format/format.sh --check cpp가 통과했다. git diff --check도 통과했다. 테스트와 빌드는 실행하지 않았다.
