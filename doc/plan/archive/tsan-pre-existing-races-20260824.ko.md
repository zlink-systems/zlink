# TSan 기존 race 기록 (2026-08-24)

0.13.0 send_complete 작업의 필수 TSan 검증(WSL2에서 `setarch -R`로 ASLR
비활성 후 실행) 결과. **신규 send_complete/pending 경로의 race는 0건**이다.

기존 코드에서 27건의 경고가 관찰되었고 전부 0.12.0에도 존재하던 경로다.
별도 수정 대상으로 기록한다:

- `pipe_t::get/set_server_socket_routing_id` (pipe.cpp:383/388) — 4+1건
- `socket_lifecycle_coordinator_t::mark_destroy_pending` (socket_lifecycle_runtime.cpp:387) — 3건
- `pipe_t::detach_peer_backref` (pipe.cpp:278) — 2건
- `blob_t::clear` (blob.hpp:123-127) 및 관련 free/memcpy — STREAM 계열

TSan에서 실패한 테스트: test_retained_hwm_credit,
test_stream_send_blocking_wakeup,
test_stream_socket_raw_multiclient_load_send_complete,
test_stream_threadsafe_send_msg — 모두 위 기존 race에 기인한다.

참고: WSL2에서 TSan은 ASLR 재배치와 충돌하므로("unexpected memory mapping")
반드시 `setarch $(uname -m) -R` 하에서 실행해야 한다.
