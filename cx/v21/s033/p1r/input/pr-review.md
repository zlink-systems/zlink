전 줄 검토(제품 2,509줄 diff 전부, 시험 2,633줄: 기존 시험 23개 파일은 단언·기대값 전후 동일 확인 후 생성 방식 변경만 확인, 새 test_cpp_framework_receive_record.cpp 402줄 전부). 빌드·시험은 감독자 머신 build7(pre-pr 301/301, samples·cross 649/649)과 음성 대조 7종 실패 출력으로 확인했습니다.

방향(수신 record = binding received_t 하나, typed RID/token 회신, 분류 1회, 회전당 lease 1회)과 판정 수 감소는 승인합니다. 아래 두 건은 머지 전 수정이 필요합니다.

**1. (차단) service_header 캐시가 parts 교체 뒤 낡은 값을 돌려줄 수 있음**
- `service_mailbox.hpp`: `_service_header`·`_application_kind`는 첫 `service_header()` 호출 때 `parts().front()`에서 채우고 이후 다시 읽지 않습니다. 그런데 non-const `parts()`로 parts 전체나 첫 part를 바꿀 수 있습니다.
- 실제로 그 형태가 있습니다: `test_cpp_framework_m6c_runtime.cpp:4732` `message.parts () = ...`로 다른 relay를 넣고 `replies.process (message)`를 다시 부릅니다. 두 번 모두 replyRelay라 지금은 결과가 같지만, 첫 part의 kind가 바뀌면 캐시된 kind로 분기합니다. `channel_host_service.cpp:377`도 `record.parts () = ...`로 교체합니다(이 경로는 service_header를 쓰지 않음).
- 요청: 캐시가 유효한 조건을 코드로 하나만 두세요. 예: parts 교체는 `replace_parts (...)` 한 경로로만 하고 거기서 캐시를 지우며, non-const `parts()`는 원소 수정(`[1]` 교체·move)만 쓰는 곳으로 줄이는 방식. 또는 캐시를 record가 아니라 ingress에서 한 번 만든 값으로 넘기는 방식. 판정 위치 수 전/후를 적어 주세요. 음성 대조 'classification'이 계속 잡히는지 확인.
- 캐시 필드가 public이라 시험이 `record._service_header.reset ()`을 직접 부릅니다(receive_record.cpp:131). 위 정리와 함께 시험도 공개 경로(예: replace_parts)로 재분류를 일으키게 바꿔 주세요.

**2. (차단) reply_source()가 payload를 복사한 뒤 닫음**
- `service_mailbox.cpp:72` `source->received = received; source->received.close ();` — reply에 필요한 것은 routing id와 reply token인데 parts까지 복사합니다(message_t 복사 + close). 호출 6곳(`public_host_runtime.cpp:6835,6884,6923,7254,7287`, `raw_stateful_dispatch.cpp:1292`). 이전 코드는 token만 복사했습니다.
- 요청: routing id·token·route generation만 옮기는 방법을 쓰세요(binding received_t에 그런 공개 API가 없으면 record에 reply 대상만 담는 형태). 할당·복사 수가 늘지 않음을 시험으로.

**3. (정리) 새 friend 대신 기존 test access 재사용**
- `raw_mesh_node_owner.hpp:131` `friend struct receive_record_test_access_t`를 새로 두었는데 같은 클래스에 `raw_mesh_node_owner_test_access_t` friend가 이미 있습니다(:536). 하나로 합쳐 주세요.

참고(변경 불필요)
- raw_route_port의 `_received` 재사용 제거: 이전에도 parts를 move해 나가 capacity 재사용 효과가 없었으므로 동등합니다.
- `consume_control`의 received 복사는 이전 raw_received_t 복사와 같은 비용입니다.
- trace 문자열을 `mesh_trace_enabled ()` 뒤로 옮긴 것 확인.

