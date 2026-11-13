# #1639 1단계 결과

1. `test_cpp_framework_mailbox_fifo_cost.cpp:25`의 `record()`가 owner, domain, 빈 메시지 part를 `from_parts`로 만듭니다. `original`은 enqueue 할당 측정 전에 만들어지며, `rotate()`에서의 호출 위치도 유지했습니다. parts 벡터를 move해 추가 복사는 없습니다.
2. `test_cpp_framework_receive_allocations.cpp:57`의 `infrastructure_record()`가 owner, domain, 메시지 part, source, generation을 `from_parts`로 전달합니다. 헬퍼 호출은 dispatch 측정 전에 유지했습니다. 기존 `message_t::from(bytes)` 복사는 그대로이고 parts 벡터를 move해 추가 복사는 없습니다.
3. `test_cpp_framework_claim_dispatch_cost.cpp:96`의 `enqueue()`가 raw request에서 만든 parts, source, correlation, callback을 `from_parts`에 전달합니다. 호출 위치는 dispatch 측정 전이며, 기존 raw request parts의 move를 유지하고 outer parts와 source도 move해 추가 복사는 없습니다.
4. `test_cpp_framework_actor_maintenance_join.cpp:3592`의 `actor_record()`가 owner, domain, source, generation, 인코딩 parts를 `from_parts`로 전달합니다. 할당 측정 창 밖의 fixture 구성이고 parts와 source를 move합니다.
5. `test_cpp_framework_relocation_seal_contract.cpp:190`의 `spot_send()`가 owner, domain, source, correlation, generation, parts를 `from_parts`로 전달합니다. 이후 `source_original` 설정은 그대로 두었고, parts와 source를 move합니다.
6. `test_cpp_framework_relocation_seal_contract.cpp:1595`의 record lambda가 owner, domain, materialized parts를 `from_parts`로 전달합니다. 할당 측정 창 밖이며 parts를 move합니다.
7. `test_cpp_framework_receive_allocations.cpp:391`의 enqueue lambda가 owner, domain, parts를 `from_parts`로 전달합니다. enqueue는 dispatch 측정 전에 수행되고 parts를 move합니다. `envelope_header` 필드 설정은 factory 호출 뒤에 유지했습니다.
8. `test_cpp_framework_local_enqueue_allocations.cpp:790`의 enqueue lambda가 owner, domain, source, generation, parts를 `from_parts`로 전달합니다. enqueue는 drain의 측정 창 밖에서 호출되고 parts를 move합니다.

추가 검색에서 동일한 직접 조립 형태가 `test_cpp_framework_manual_dispatch_owner.cpp:169`, `test_cpp_framework_receive_dispatch_cost.cpp:175`, `test_cpp_framework_send_allocations.cpp:922`에 남아 있습니다. 세 파일은 이번 변경 대상으로 지정되지 않아 수정하지 않았습니다. 그 밖에도 tests/에 레코드별 필드 조립은 있지만 위 여덟 헬퍼와 같은 단순 owner/domain/parts 조립은 아닙니다.

`bash scripts/format/format.sh --check cpp` 통과. `git diff --check` 통과. 빌드와 테스트는 실행하지 않았습니다.
