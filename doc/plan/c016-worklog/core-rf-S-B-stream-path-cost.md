# S-B — STREAM tcp 경로 정적 비용 분해 (읽기 전용)

> 작성: 2026-09-06, main `0c01bbb7c9` (계획 `core-refactor-stream-perf-0.17.0-plan.ko.md` §1.1–1.3, §4.1)
> 대상 셀: `bindings/c/bench/with_stream` CCU 1000, echo, io_threads 4, 64 B / 1024 B / 65536 B
> 방법: 코드 정적 추적만. 빌드·벤치·프로파일 없음. 실측이 필요한 항목은 "확인 방법"을 붙였다.

## 0. 먼저 확정한 사실 (가설 정정 포함)

| # | 사실 | 근거 |
|---|---|---|
| F1 | 벤치 zlink 스택은 **RAW 수신 모드**다(`ZLINK_STREAM_RECV_MODE_RAW`). 따라서 `decode_packet_bytes`·`pump_packet_receive_queue`·`packet_record_t`·64-chunk bounded pump·fragment 조립은 **이 셀에서 한 번도 실행되지 않는다**. PACKET 경로는 `zlink_packet` 스택 전용이다 | `test_scenario_stream_zlink.cpp:173-180`, `stream.cpp:1041-1044`(`xrecv_routed`는 RAW만), `stream.cpp:1028-1031`(`xrecv`는 PACKET만), `stream.cpp:1086-1093`(`xhas_in` 분기) |
| F2 | 벤치는 `ZLINK_SEND_FLAGS_NONE`으로 보낸다 → **DONTWAIT fast path가 아니라 blocking submit 경로**(`send_completion_submit_blocking`)를 탄다 | `test_scenario_stream_zlink.cpp:255,271,323,360`, `socket_message_send_api.cpp:375-384`, `socket_send_submit.cpp:471-511` |
| F3 | zlink 수신은 **zero-copy**다(decoder allocator 버퍼를 msg가 그대로 소유). asio 레퍼런스는 payload를 2회 복사한다(`append_frame_bytes` + `pending_write` memcpy). 즉 격차는 **복사량이 아니라 고정 비용**이라는 §1.1의 판정이 코드로도 확인된다 | `raw_decoder.cpp:48-77`, `test_scenario_stream_asio.cpp:259,274-275` |
| F4 | STREAM raw 엔진의 `build_gather_header()`는 **항상 false를 반환**한다. 그런데 `prepare_gather_output()`은 매 write turn마다 전부 실행된다(encode 탐침 → `_next_msg` pull → 크기 판정 → 가상 호출 → `load_msg` → false) | `asio_raw_engine.cpp:114-124`, `asio_engine.cpp:757-802`, 호출부 `asio_engine.cpp:689, 1350` |
| F5 | `fast_mutex_t`와 `mutex_t`가 **둘 다 `PTHREAD_MUTEX_RECURSIVE`**다. 이름과 달리 "fast"가 아니다(소유자 tid 기록·재진입 카운트, elision 불가) | `utils/fast_mutex.hpp:41-46`, `utils/mutex.hpp:89-94` |
| F6 | 수신 msg 버퍼는 `raw_decoder`가 `max_messages_=1`로 만든 shared allocator에서 나오며, 메시지가 버퍼를 가져가면 다음 read는 **새 버퍼를 잡는다**(spare 슬롯 1개 CAS 재사용 실패 시 `malloc`) | `asio_raw_engine.cpp:88-89`, `decoder_allocators.cpp:249-287, 126-163` |
| F7 | mailbox의 primary signaler는 **coalescing이 아니다**. `_cpipe`가 잠들어 있다가 깨어나는 전이마다 `write(2)` 1회 | `signaler.cpp:191-210`, `mailbox.cpp:69-80`, `mailbox.cpp:104-105` |

---

## 1. 수신 경로 — 패킷 1개당 실행되는 단계

전제: 1024 B 프레임 1개 = TCP chunk 1개(요청/응답 핑퐁이므로 read당 대략 메시지 1개), 연결 1개당 pipe 1쌍.
"잠금"은 lock/unlock 쌍 수. R = 재귀 pthread mutex, F = `fast_mutex_t`(역시 재귀, F5).

### 1.1 I/O 스레드 (asio 워커)

| # | 단계 | file:line | 잠금 | 원자 연산 | 할당 | syscall | 가상/간접 호출 | STREAM에서 상수인 분기 | 계약 근거 |
|---|---|---|---|---|---|---|---|---|---|
| R1 | epoll wake + boost handler dispatch | `asio_engine.cpp:517-535` (handler alloc 경유) | 0 | 0 | 0 (handler allocator 재사용) | epoll_wait 1 (배치 공유) | 1 (asio 완료 핸들러) | `use_stream_handler_alloc` (type==STREAM 상수) | 03-io-thread §4 |
| R2 | `on_read_complete` 진입 검사 8건 | `asio_engine.cpp:908-999` | 0 | `callback_guard.expired()` weak_ptr lock = 원자 CAS 1~2 | 0 | 0 | 0 | `terminating`/`plugged`/`ec`/`zmp_transport_has_message_boundaries`/`transport_has_message_boundaries`/`_input_stop_reason` — 정상 STREAM에서 전부 고정 | — |
| R3 | `maybe_grow_stream_decoder_read_target` | `asio_engine.cpp:869-881, 1001` | 0 | 0 | 0 | 0 | 0 | 정상 상태에서 항상 조기 return(목표치 포화) | 08-stream §371-384(read drain·target) |
| R4 | `process_input` 루프 진입 | `asio_engine.cpp:1183-1255` | 0 | 0 | 0 | 0 | 0 | `handshaking`/`input_in_decoder_buffer` 고정 | — |
| R5 | `_decoder->decode()` | `raw_decoder.cpp:38-81` | 0 | `msg_t::init(hint)`가 content refcount set + allocator `references` 조작 = 원자 1~2 | 0 (zero-copy) | 0 | 1 (i_decoder 가상) | `_max_msg_size>=0` / `in_allocator` 항상 true | protocol/02-raw |
| R6 | `_allocator.advance_content(); release()` | `raw_decoder.cpp:74-77` | 0 | 0 | 0 | 0 | 0 | `is_zcmsg()` 항상 true | — |
| R7 | `(this->*_process_msg)` → `decode_and_push` | `asio_engine.cpp:1251, 1854-1874` | 0 | 0 | 0 | 0 | 1 (멤버 함수 포인터) | `flags & command` 항상 false, `asio_trace_on` 항상 false | — |
| R8 | `session_base_t::push_msg_internal` | `session_base_pipe_io.cpp:159-209` | 0 | `get_transport_connection_id()` 원자 load 1 | 0 | 0 | 1 (가상 push_msg) | `command`/`is_subscribe`/`is_cancel`/`reservation_` 전부 상수 | README recv 절 |
| R9 | `pipe_t::write` → `_out_sync` 획득 | `pipe.cpp:2083-2094` | **F 1** | 0 | 0 | 0 | 0 | — | 06-auto-hwm(HWM 판정 위치) |
| R10 | `admit_write_unlocked` = `write_state_admission_unlocked` + `hwm_credit_ready_unlocked` | `pipe.cpp:1746-1792, 1527-1541, 1772-1785` | 0 | `remote_flow_blocked_unlocked` 원자 load 1~2, `refresh_peer_credit_snapshot` 원자 load 2 | 0 | 0 | 0 | `_state`, `_transport_pair_write_held`, `_out_active` 전부 고정 | 06-auto-hwm |
| R11 | `write_message_unlocked` 본체(≈150행) | `pipe.cpp:3687-3841` | 0 | `_registry_accounting` false → registry 경로 없음 | 0 | 0 | 0 | `_conflate`, `more`, `is_delimiter`, `is_routing_id`, `is_credential`, `_max_message_bytes` — 전부 상수 분기 | 06-auto-hwm §byte HWM |
| R12 | `_out_pipe->write()` (ypipe) | `pipe.cpp:3798` | 0 | 0 | ypipe chunk 소진 시에만 (1/N) | 0 | 0 | — | — |
| R13 | `publish_session_outbound_accounting_unlocked` | `pipe.cpp:4010-4027, 3838` | 0 | **release store 1~2** (`_session_io_writer`가 세션 쪽 pipe에서 true) | 0 | 0 | 0 | `provisional_changed_` 항상 false(단일 프레임) | 06-auto-hwm(스냅샷 발행) |
| R14 | `msg_->init()` (엔진 msg 재초기화) | `session_base_pipe_io.cpp:201` | 0 | 0 | 0 | 0 | 0 | — | — |
| R15 | `session->flush()` → `pipe_t::flush()` | `pipe.cpp:2731-2736` | **F 1 (2번째 획득)** | 0 | 0 | 0 | 1 (가상 flush) | — | README recv(가시성 경계) |
| R16 | `flush_unlocked` | `pipe.cpp:4066-4112` | 0 | `_transport_lane_count` acquire load 1, ypipe flush CAS 1 | 0 | 0 | 0 | `pending_peer_controls_unlocked`, `peer_uses_routed_protocol_unlocked`, `_transport_pair_id!=0`(STREAM raw는 0) → `reclassify_candidate` 항상 false | — |
| R17 | (pipe가 잠들어 있었을 때) `send_activate_read(peer)` → mailbox | `pipe.cpp:4109`, `object.cpp:365-370`, `mailbox.cpp:56-82` | **R 1** (`mailbox::_sync`) | `_command_pending_hint` store, `_scheduled` exchange | 0 | mailbox cpipe가 비어 있었으면 **eventfd write 1** | 1 | — | 05-polling(wake 조건) |
| R18 | 다음 read 준비: `speculative_read()`/`start_async_read()` → `get_buffer` → `allocator.allocate()` | `asio_engine.cpp:539-580, 466-509`, `decoder_allocators.cpp:249-287` | 0 | spare 슬롯 `xchg` 1 (+ 반납 측 `cas` 1) | **spare 미스 시 `malloc` 1** | `read(2)` 1 (speculative) | 1 | `use_stream_rx_slab`, `read_from_pending_pool` 등 고정 | 08-stream §378(read drain 활성) |
| R19 | read drain 루프 관리 | `asio_engine.cpp:1043-1080` | 0 | 0 | 0 | 0 | 0 | `should_drain` 항상 true; 루프 상한 64/1 MiB | 08-stream §383-384 |

I/O 스레드 수신 합계(메시지 1개): **재귀 mutex 3쌍**(F×2 + R×1), 원자 연산 ≈ 8–12, 힙 할당 0–1, syscall 1(read) + 0–1(eventfd write).

### 1.2 앱 스레드 (`zlink_poller_wait` → `zlink_recv_part`)

| # | 단계 | file:line | 잠금 | 원자 | 할당 | syscall | 가상/간접 | 상수 분기 | 계약 근거 |
|---|---|---|---|---|---|---|---|---|---|
| A1 | `zlink_poller_wait` → `check_socket_events` → `get_events_for_poller` | `socket_poller.cpp:813, 617-633, 574-593, 890-924` | **R 1**(API sync는 아님; `socket_public_api_scope_t`는 원자만) | `public_api_state` fetch_add/sub 2 | 0 | 0 | 1 | `_output_readiness`, `terminal_event_delivered` 고정 | 05-polling |
| A2 | `get_events_internal` → **`process_commands(0,false,false,…)`** — poller wait마다 전체 command drain | `socket_base_api.cpp:810-824`, `socket_base_lifecycle.cpp:358-604` | **R 2**(`api sync` + `command_owner_sync`) | 다수 | 0 | mailbox recv 시 **eventfd read 1** | 1 | `throttle_=false`이므로 rdtsc 스킵 없음 | 05-polling(레벨 트리거) |
| A2a | drain된 command 1개당(= 수신 메시지 1개당 `activate_read` 1개) | `socket_base_lifecycle.cpp:537-544` | **R 1**(`receive.sync`) + 아래 | ypipe read 1 | 0 | 0 | 1(`process_command` 가상) | — | — |
| A2b | `pipe_t::process_activate_read` | `pipe.cpp:2738-2766` | **F 1**(`_out_sync`) | `_head_reclassify_wake` acquire load 1 | 0 | 0 | 1(`_sink->read_activated`) | 재분류 조건은 STREAM raw에서 항상 false(`_transport_pair_id==0`) | — |
| A2c | `socket_base_t::read_activated` | `socket_base_dispatch.cpp:1603-1678` | **R 1**(`receive.sync` 재진입) | `get_transport_pair_id` 등 원자 load 2–3 | 0 | 0 | 1(`xread_activated`) | 첫 두 분기(pair_id!=0)는 STREAM raw에서 항상 false | — |
| A2d | `stream_t::xread_activated` → `fq_t::activated` | `stream.cpp:786-789`, `fq.cpp:178-196` | 0 | 0 | 0 | 0 | 0 | O(1) 인덱스(`array_item_t::get_array_index`) — 선형 탐색 아님 | — |
| A2e | `notify_receive_progress_locked` | `socket_base_lifecycle.cpp:1540-1546` | 0 | 0 | 0 | 0 | 0 | `waiters!=0` 항상 false(폴링 사용 시) | — |
| A3 | `has_in()` | `socket_base_api.cpp:984-1003` | **R 1**(`receive.sync`) | `part_helper_recv_ready_flag` load 1 | 0 | 0 | 1(`xhas_in`) | `part_helper_recv_ready()` 항상 false | 05-polling(POLLIN level) |
| A4 | `stream_t::xhas_in` → `fq_t::has_in` → pipe `check_read` | `stream.cpp:1084-1094`, `fq.cpp:401-424`, `pipe.cpp:1027-1057` | 0 | ypipe `read_if` = load/CAS 1–2 | 0 | 0 | 0 | `stream_recv_mode` 상수, `_stream_notify_routing_ids` 항상 빈 | 08-stream §5 |
| A5 | `has_out()` (POLLIN만 등록했으므로 미실행) | `socket_base_api.cpp:883` | — | — | — | — | — | `events_ & POLLOUT` false | — |
| A6 | `poll(2)` (이미 준비되었으면 생략) | `socket_poller.cpp:835` | 0 | 0 | 0 | poll 1 (배치당) | 0 | — | 05-polling |
| A7 | `zlink_recv_part` 입구 — `as_socket_handle` | `socket_message_api.cpp:132`, `socket_api_internal.hpp:96-120` | 0 | public handle acquire/release **원자 2** | 0 | 0 | 0 | `check_tag` 2회 | README `zlink_recv_part` |
| A8 | `clear_last_recv_source_rid`, `validate_recv_flags`, `socket_type`, `stream_mark_raw_part_receive` | `socket_message_api.cpp:135-179`, `stream_dispatch_lifecycle.cpp:7-15` | 0 | 0 | 0 | 0 | 2 (가상 `socket_type`, `stream_mark_raw_part_receive`) | 후자는 옵션 비교 1회 후 항상 0 반환 — **순수 no-op** | 08-stream §5 |
| A9 | `part_helper_state()` shared_ptr 획득 + `recv_sequence_active` | `socket_message_api.cpp:181-184`, `socket_base_request_reply_bridge.cpp:46-56` | 0 | `part_helper_state_present` load 1 (없으면 refcount 없음) | 0 | 0 | 0 | STREAM에서 항상 empty | — |
| A10 | `recv_socket_parts` → `recv_tls_view::begin_with_first_slot` | `socket_message_recv_api.cpp:131-146` | 0 | TLS 접근 | 0 | 0 | 0 | `direct_public_recv_fast` 항상 true | — |
| A11 | `socket_base_t::recv_routed` 입구 6개 출력 초기화 + `should_poll_commands_after_recv` | `socket_base_msg.cpp:929-967` | 0 | 0 | 0 | 0 | 0 | `inbound_poll_rate=1536`이므로 1536회에 1번만 추가 drain (`utils/config.hpp:31`) | — |
| A12 | `receive_once_guarded` — public receive lease | `socket_base_msg.cpp:53-98` | 0 | **CAS 1 + release store 1** | 0 | 0 | 1(람다) | `record_scope_` NULL, `defer_record_scope_` false | 04-thread-safety |
| A13 | `stream_t::xrecv_routed` | `stream.cpp:1034-1082` | 0 | `get_transport_connection_id` load 1 | 0 | 0 | 1 | `stream_recv_mode` 비교, notify 큐 비었음, `admission_` NULL | 08-stream §5 |
| A14 | `fq_t::recvpipe_internal` | `fq.cpp:271-399` | 0 | 0 | 0 | 0 | 1 | `msg_->close()` 1회(직전 init된 빈 msg), round-robin `_current` 갱신 | — |
| A15 | `pipe_t::read_internal<false>` → ypipe read | `pipe.cpp:1133-1229` | 0 | ypipe read 1 | 0 | 0 | 0 | `_registry_accounting` false, `is_credential`/`is_delimiter` false | — |
| A16 | `account_inbound_frame` | `pipe.cpp:3843-3951` | 0 | **store 2(relaxed) + peer `_waiting_for_byte_credit` acquire load 1**; LWM 도달 시 `seq_cst` fence 1 | 0 | 0 | 0 | `completes_multipart` 항상 false, `writer_waiting` 보통 false | 06-auto-hwm(credit·LWM) |
| A17 | (credit 경계에서만) `send_activate_write(peer)` | `pipe.cpp:3944-3946` | R 1 | — | 0 | eventfd write 0–1 | 1 | `_lwm_hint` 기본 4 KiB → 1024 B에서 ~4메시지마다 경계, 그러나 `writer_waiting`이 아니면 command 없음 | 06-auto-hwm |
| A18 | `copy_routing_id_from_bytes` (4 B) | `stream.cpp:1074-1076` | 0 | 0 | 0 | 0 | 0 | `blob_t` 참조 | 08-stream §5 |
| A19 | `clear_request_reply_metadata` | `socket_message_recv_api.cpp:190-192` | 0 | 0 | 0 | 0 | 0 | STREAM에서 항상 no-op | — |
| A20 | 앱: `zlink_msg_init` + (에코 후) msg 소유권 이전 | 벤치 `:379-383` | 0 | 0 | 0 | 0 | 0 | — | README part 소유권 |

앱 스레드 수신 합계(메시지 1개): **재귀 mutex 4–5쌍**(A2a/A2b/A2c의 command 처리 3 + A3의 has_in 1, poller wait당 A1/A2의 2를 배치로 분할), 원자 연산 ≈ 12–16, 힙 할당 0, syscall 0(배치당 poll 1 + eventfd read 1).

---

## 2. 송신 경로 — 메시지 1개당 (`zlink_send_part_rid`, FLAGS_NONE, PART_FINAL)

### 2.1 앱 스레드

| # | 단계 | file:line | 잠금 | 원자 | 할당 | syscall | 가상/간접 | 상수 분기 | 계약 근거 |
|---|---|---|---|---|---|---|---|---|---|
| S1 | 인자 검증 4블록 + `valid_routing_id` + `validate_send_flags` + `validate_part_flag` | `socket_message_send_api.cpp:643-671` | 0 | 0 | 0 | 0 | 0 | 전부 고정 | 08-stream §4 |
| S2 | `as_socket_handle` | `:673`, `socket_api_internal.hpp:96` | 0 | **원자 2**(acquire/release) | 0 | 0 | 0 | — | — |
| S3 | `socket_type()==STREAM` → `submit_public_send_record` | `:681-692`, `:365-395` | 0 | 0 | 0 | 0 | 1 | `part_flag_!=FINAL` 항상 false | 08-stream §4 |
| S4 | `flags_==NONE` → `send_completion_submit_blocking` | `:382-383`, `socket_send_submit.cpp:471-511` | 0 | 0 | 0 | 0 | 1 | `type` 4중 비교, DEALER 분기 스킵 | README part send |
| S5 | `completion_submit_wait_context_t` + `blocking_send_wait_guard_t` 생성/`bind_target` | `socket_send_submit.cpp:487, 363-368` | ? (guard 내부 등록은 실패 시에만) | 0–2 | 0 | 0 | 0 | `target=NULL`, `has_routed_target=true` | README WRITABLE |
| S6 | 루프 1회차: `observe_submit_progress` + `failure_errno` | `socket_send_submit.cpp:379-385` | 0 | acquire load 2 | 0 | 0 | 0 | — | — |
| S7 | `socket_public_send_scope_t physical_scope(…, needs_sync=true, complete)` → `enter_public_send` | `socket_send_submit.cpp:391-393`, `socket_lifecycle_runtime.cpp:94-155, 416-444` | **R 1**(socket API sync) | **CAS 루프 1**(성공 시 1회) + `mark_public_api_sync_owned` | 0 | 0 | 0 | closing/inflight/multipart 비트 전부 0 | 04-thread-safety(API→command-owner→receive 순서) |
| S8 | `process_submit_commands` | `socket_base_lifecycle.cpp:606-627` | 0 | acquire load 2 (command 없으면 즉시 return) | 0 | 0 | 0 | 보통 no-op | — |
| S9 | `try_admit_send_parts_scoped` 진입 + `pair_complete_record_eligible` 판정 | `socket_send_complete.cpp:242-320` | 0 | 0 | 0 | 0 | 0 | `count==1`이라 항상 false | — |
| S10 | `rid = *transient_target_rid_` (12 B 구조체 복사) | `socket_send_complete.cpp:307-314` | 0 | 0 | 0 | 0 | 0 | — | — |
| S11 | `send_direct_with_retry` 전문 검사 + `prepare_direct_send_message` | `socket_base_msg.cpp:320-399` | 0 | `_ctx_terminated` load 1 | 0 | 0 | 1 | `commands_already_processed_=true`, `size>UINT32_MAX` 체크는 STREAM 제외 | — |
| S12 | **`_auto_hwm_send_attempts.fetch_add(1, relaxed)`** — 구독자 유무와 무관하게 메시지마다 | `socket_base_msg.cpp:398-399` | 0 | **RMW 1** | 0 | 0 | 0 | `record_context_admission_` 항상 true | 06-auto-hwm(계측) |
| S13 | `stream_t::xsend_routed` 진입: 출력 5개 초기화 + rid 검증 | `stream.cpp:918-945` | 0 | 0 | 0 | 0 | 1(가상) | `LIBZLINK_UNUSED` 6개(=죽은 파라미터) | 08-stream §4 |
| S14 | **route shard 잠금 + `std::map` 조회** | `stream.cpp:947-954` | **F 1** | 0 | 0 | 0 | 0 | shard = rid % 64 → 1000 연결이면 shard당 ~16개, RB-tree 비교 ~4회 + 캐시 미스 | 08-stream §4(RID→연결) |
| S15 | `stream_exact_target_identity` | `stream.cpp:53-75, 960` | 0 | 원자 load 2–3 | 0 | 0 | 0 | `pair_id==0` 항상 true(raw STREAM) → connection_id 사용 | — |
| S16 | `write_single_message_and_flush_no_recursive_hwm_check` — `_out_sync` | `pipe.cpp:2566-2638` | **F 1** | 0 | 0 | 0 | 0 | 8항 fast-path 조건 전부 상수 | 06-auto-hwm |
| S17 | `write_state_ready_unlocked` + `can_commit_bytes_with_peer_snapshot_unlocked` (credit 확인) | `pipe.cpp:1746-1754, 3665-3676, 3361` | 0 | `remote_flow_blocked` load 1, peer credit 스냅샷 acquire load 2 | 0 | 0 | 0 | — | 06-auto-hwm |
| S18 | `_out_pipe->write` + 카운터 갱신 + `publish_session_outbound_accounting_unlocked` | `pipe.cpp:2617-2635` | 0 | 소켓 쪽 pipe는 `_session_io_writer=false` → **즉시 return**(비용 0) | ypipe chunk 소진 시 1/N | 0 | 0 | — | — |
| S19 | `flush_unlocked` → ypipe flush → 세션 pipe가 잠들어 있으면 `send_activate_read` | `pipe.cpp:2636, 4066-4112` | 0 | CAS 1 | 0 | 0 | 0 | R16과 동일하게 재분류 조건 항상 false | — |
| S20 | (S19에서) I/O 스레드 mailbox `send` | `mailbox.cpp:56-82` | **R 1** | 2–3 | 0 | **eventfd write 0–1** + `boost::asio::post` 1(`_scheduled` 전이 시) | 1 | — | 03-io-thread §4 |
| S21 | `msg_->init()`, 결과 매핑, scope 소멸(`leave_public_send`) | `stream.cpp:988-994`, `socket_lifecycle_runtime.cpp:465-492` | R 1 unlock | fetch_sub 1 | 0 | 0 | 0 | — | — |
| S22 | `consume_send_frames_from` | `socket_message_send_api.cpp:391-392` | 0 | 0 | 0 | 0 | 0 | STREAM에서 no-op | — |

앱 스레드 송신 합계: **재귀 mutex 3–4쌍**(API sync 1 + shard 1 + `_out_sync` 1 + mailbox 0–1), 원자 ≈ 14–18, 힙 0, syscall 0–1.

### 2.2 I/O 스레드 (응답 write)

| # | 단계 | file:line | 잠금 | 원자 | 할당 | syscall | 가상/간접 | 상수 분기 |
|---|---|---|---|---|---|---|---|---|
| W1 | mailbox 핸들러 → `process_command` → `pipe_t::process_activate_read` → `session_base_t::read_activated` → `engine->restart_output()` | `mailbox.cpp:330-344`, `pipe.cpp:2738-2766`, `asio_engine.cpp:1518-1533` | R 1(mailbox) + F 1(`_out_sync`) | 여러 | 0 | 0 | 3 |
| W2 | `speculative_write` 진입 검사 3건 | `asio_engine.cpp:1327-1348` | 0 | 0 | 0 | 0 | 0 | `write_pending`/`io_error`/`handshaking` 고정 |
| W3 | **`prepare_gather_output`(F4: 항상 실패로 끝나는 준비 작업)**: `encode(&buf,0)` 탐침 → `_next_msg` pull → 크기/threshold 판정 → `build_gather_header` 가상 호출(항상 false) → `load_msg` | `asio_engine.cpp:1350, 757-802` | 0 | 0 | 0 | 0 | **3**(encode, next_msg, build_gather_header) | `gather_enabled` 항상 true, 결과는 항상 false |
| W4 | `pull_msg_from_session` → `session_base_t::pull_msg` → `pipe_t::read` | `asio_engine.cpp:1884-1887`, `session_base_pipe_io.cpp:77-99`, `pipe.cpp:1081-1229` | 0 | ypipe read 1 | 0 | 0 | 2 | `transport_connection_id` stamp 비교 |
| W5 | `account_inbound_frame`(세션 쪽 읽기 credit) | `pipe.cpp:3843-3951` | 0 | store 2 + load 1 (+ 경계에서 fence) | 0 | 0 | 0 | LWM 힌트 4 KiB |
| W6 | `prepare_output_buffer` → `encoder->encode()` = **payload memcpy 1회**(out_batch 4096) | `asio_engine.cpp:1271-1325`, `raw_encoder.cpp:16-19` | 0 | 0 | 0 | 0 | 2 | `output_target_batch`는 STREAM 목표치, 루프는 배치가 찰 때까지 반복 |
| W7 | `write_some()` = `send(2)` | `asio_engine.cpp:1374` | 0 | 0 | 0 | **write 1**(배치 공유) | 1 | — |
| W8 | 예산 판정 후 루프 반복 | `asio_engine.cpp:1420-1457` | 0 | 0 | 0 | 0 | 0 | 예산 2 MiB |
| W9 | 이전 msg `close()` → decoder 버퍼 refcount 감소 → spare CAS 또는 `free` | `decoder_allocators.cpp:126-141` | 0 | RMW 1 + CAS 1 | **free 0–1** | 0 | 0 | — |

I/O 스레드 송신: 재귀 mutex 2쌍, 원자 ≈ 8–10, 힙 0–1, syscall 1(write, 배치 공유).

---

## 3. (a) asio 레퍼런스에 대응물이 **없는** 단계 — 추정 비용 순

asio 서버는 read 완료 핸들러 안에서 파싱→에코 버퍼 구성→`async_write`를 이어 붙인다. 스레드 핸드오프도, 소켓 상태 기계도, 잠금도 전혀 없다(`test_scenario_stream_asio.cpp:249-298`). 아래는 zlink에만 있는 단계다.

| 순위 | 단계 | 위치 | 추정 메시지당 비용 | 근거 |
|---|---|---|---|---|
| 1 | **I/O↔앱 스레드 핸드오프 자체**: `send_activate_read` command 2회(수신 방향 1 + 송신 방향 1) — mailbox `_sync` 재귀 mutex, cpipe write/read, command dispatch, `pipe::process_activate_read`(`_out_sync`), `read_activated`(`receive.sync`), `fq::activated` | `pipe.cpp:4109`, `mailbox.cpp:56-82`, `pipe.cpp:2738`, `socket_base_dispatch.cpp:1603-1678` | 매우 큼. 재귀 mutex 5–6쌍 + 캐시 라인 왕복 2회 + 조건부 eventfd write 2 | R17·A2a–A2e·S19–S20·W1 |
| 2 | **재귀 pthread mutex 남용**: 핫 경로의 모든 잠금이 `PTHREAD_MUTEX_RECURSIVE`. 메시지당 총 7–10쌍 | `utils/fast_mutex.hpp:41-46`, `utils/mutex.hpp:89-94` | 큼(재귀 mutex는 비경합에서도 owner tid 로드/스토어 추가, elision 불가) | F5 |
| 3 | **수신 버퍼 malloc/free**: decoder allocator가 메시지당 새 버퍼를 잡고(spare 1칸 CAS), 응답 write 후 반납 | `decoder_allocators.cpp:249-287, 126-163` | 중간~큼. spare 미스율에 비례(read drain 중 연속 read면 반드시 미스) | F6 |
| 4 | **`prepare_gather_output`의 죽은 준비 작업**: STREAM raw는 gather header를 만들 수 없는데 매 write turn마다 encode 탐침 + pull + 가상 호출 + load_msg | `asio_engine.cpp:757-802`, `asio_raw_engine.cpp:114-124` | 중간(가상 호출 3 + 분기 6) | F4 |
| 5 | **blocking submit 진입 비용**: `send_completion_submit_blocking` → wait context/guard 생성, `enter_public_send` CAS + API sync 재귀 mutex, 진행 epoch 관찰 | `socket_send_submit.cpp:471-511, 351-441`, `socket_lifecycle_runtime.cpp:94-155` | 중간 | S4–S8 |
| 6 | **route shard 잠금 + `std::map<uint32_t,pipe_t*>` 조회**(송신마다 1회, `xsend_writable_target_*`가 겹치면 더) | `stream.cpp:947-954, 199-202` | 중간(RB-tree ~4 비교, 포인터 추적 캐시 미스) | S14 |
| 7 | **`poller_wait`마다 전체 command drain + `receive.sync` 하 `xhas_in`** | `socket_base_api.cpp:810-824, 984-1003` | 배치당이지만 배치가 짧으면 큼 | A2·A3 |
| 8 | **auto-HWM 계측 원자 RMW**(`_auto_hwm_send_attempts`)가 구독자 없어도 메시지마다 | `socket_base_msg.cpp:398-399` | 작음~중간(경합 시 캐시 라인 핑퐁) | S12 |
| 9 | **credit/HWM 회계**: `account_inbound_frame`의 published store 2 + peer waiter load, `can_commit_bytes_*`의 peer 스냅샷 재로드 | `pipe.cpp:3843-3951, 3665-3676` | 작음~중간 | A16·S17 |
| 10 | **`publish_session_outbound_accounting_unlocked`의 release store**(세션 쪽 pipe에서만, 메시지마다) | `pipe.cpp:4010-4027` | 작음 | R13 |
| 11 | **공개 핸들 acquire/release 원자 2쌍**(recv 1, send 1) | `socket_api_internal.hpp:96-120` | 작음 | A7·S2 |
| 12 | **no-op 호출들**: `stream_mark_raw_part_receive`(항상 0), `clear_request_reply_metadata`, `consume_send_frames_from`, `xsend_routed`의 `LIBZLINK_UNUSED` 6개 | `stream_dispatch_lifecycle.cpp:7-15`, `stream.cpp:928-932` | 작음 | A8·A19·S22 |

**asio에 있는데 zlink에 없는 것**: payload 복사 2회. zlink는 zero-copy다(F3). 즉 zlink가 이기는 항목은 복사뿐이고, 그 이득을 위 12항목이 전부 상쇄한다.

## 3b. (b) 계약을 바꾸지 않고 줄이거나 묶는 방법

| # | 대상 | 방법 1 | 방법 2 | 계약 영향 |
|---|---|---|---|---|
| 1 | 핸드오프 command | ① `read_activated` 경로에서 STREAM raw가 확실히 쓰지 않는 재분류 검사(`_transport_pair_id!=0` 분기 3개)를 pipe에 캐시된 불리언 한 번으로 접고, `process_activate_read`의 `_out_sync` 획득을 원자 플래그 교환으로 대체(알림 여부만 결정하므로 잠금 불필요한지 검증 필요). ② mailbox `_sync`를 비재귀로 | ypipe가 "잠들었다"고 표시되는 조건을 바꾸지 않은 채, **연속된 `activate_read`를 pipe 단위로 coalesce**(이미 큐에 있는 미처리 activate_read가 있으면 재전송 생략 — pipe에 pending 플래그) | 없음. 깨어나는 시점·순서 동일(같은 batch 안에서 한 번만 깨움). POLLIN level 조건은 `xhas_in`이 그대로 결정 |
| 2 | 재귀 mutex | `fast_mutex_t`를 `PTHREAD_MUTEX_DEFAULT`로 바꾸고, 재진입이 실제로 필요한 곳(`scoped_optional_fast_lock_t` 사용처)만 별도 타입으로 분리 | `_out_sync`처럼 재진입이 없음이 증명되는 것부터 하나씩 | 없음(잠금 구현 세부). **단 재진입 여부를 코드로 전수 확인해야 함** |
| 3 | 수신 버퍼 alloc | spare 슬롯을 1칸이 아니라 작은 free-list(예: 4칸)로 확장 — 반납/획득 모두 같은 I/O 스레드이므로 스레드 로컬 리스트로 두면 원자 연산도 제거 | `raw_decoder`의 `max_messages_`를 1이 아니라 read target/평균 프레임 크기로 두어 한 버퍼에서 여러 메시지를 잘라 쓰기(zero-copy 유지) | 없음(내부 메모리 재사용). 05-connection-memory의 보유 바이트 상한 서술은 재확인 필요 |
| 4 | gather 죽은 작업 | 엔진이 `build_gather_header`를 지원하는지 **연결 시점에 한 번** 판정해 `_gather_supported` 플래그로 두고, false면 `prepare_gather_output` 자체를 호출하지 않음 | `asio_raw_engine`에서 `_connection_fastpath_policy`의 gather 능력을 처음부터 false로 설정 | 없음. 관측 가능한 write 동작 동일(현재도 항상 encoder 경로) |
| 5 | blocking submit 진입 | `FLAGS_NONE` + 즉시 admission 성공이 지배적이므로, wait guard/컨텍스트 생성을 **첫 실패 이후로 지연**(현재는 루프 진입 전에 무조건 생성) | `enter_public_send`의 CAS와 API sync 획득을 하나의 상태 전이로 합침(이미 `take_sync_in_admission` 경로가 있음 — 그 경로가 항상 타는지 확인) | 없음. 실패 시 WRITABLE token 생성 시점·조건 불변이어야 함(08-stream §4 line 122-135) |
| 6 | route 조회 | `std::map` → open-addressing 해시(정수 키 4 B). 순서 의존이 없음(`xpipe_terminated`의 순회는 전체 스캔) | 최근 사용 RID→pipe 1엔트리 캐시를 shard에 두기(에코는 같은 RID가 연속으로 오지 않으므로 효과 제한적 → 해시 쪽이 우선) | 없음. 단 `xterm_peer_rid`/`~stream_t`의 순회 순서가 관측되지 않음을 확인 |
| 7 | poller wait 비용 | `get_events_internal`의 `process_commands(0,false,…)`를 `throttle_=true`(rdtsc 스킵)로 두는 것은 **금지**(레벨 조건이 바뀔 수 있음). 대신 command가 없을 때의 조기 return을 `process_submit_commands`처럼 두 원자 로드로 앞당기기 | `has_in()`의 `receive.sync` 획득을 receive lease CAS(A12와 동일 기법)로 대체 | 없음(둘 다 "관측되는 준비 상태"를 바꾸지 않음). 05-polling의 level 조건 문장을 문장 단위로 재확인 |
| 8 | auto-HWM 계측 | 카운터를 소켓 단일 원자 대신 **pipe 로컬 비원자 카운터 + 스냅샷 시 합산**으로 | 계측이 실제로 필요한 조건(auto-HWM 정책 활성)일 때만 증가 | 없음이면 채택, **집계값이 공개 API로 노출되면 D**. `06-auto-hwm`의 metric 정의 확인 필요 |
| 9 | credit 회계 | `account_inbound_frame`의 `_published_*_read` store를 credit 경계에서만 발행(단일 프레임은 이미 relaxed지만 매번 씀) | `can_commit_bytes_with_peer_snapshot_unlocked`가 1차 실패했을 때만 재로드하도록 이미 되어 있음 — 추가 개선 여지 낮음 | store 빈도를 줄이면 Auto-HWM 스냅샷의 최신성이 달라짐 → **06-auto-hwm 문장 확인 필요, 경계상 D 후보** |
| 10 | no-op 호출 | `stream_mark_raw_part_receive`처럼 옵션 비교 후 항상 0인 가상 호출을 비가상 인라인 검사로 | `xsend_routed`의 미사용 파라미터 6개를 시그니처에서 제거(내부 인터페이스) | 없음(전부 내부). Phase 3 R1 인벤토리 항목으로도 적합 |

**D(계약 변경 필요) 후보 — 구현 금지, 결정 대기**

| D | 아이디어 | 깨지는 조항 |
|---|---|---|
| D-a | 앱 send를 즉시 I/O 스레드에 알리지 않고 N개/T µs 묶어서 알림 | 08-stream §4 line 118-120·README part send: 제출 경계가 늦춰지면 peer 도달 지연이 관측된다. §4.1-3(순서·타이밍 보존) 위반 |
| D-b | `poller_wait`에서 command drain을 rdtsc로 스킵 | 05-polling의 POLLIN/POLLOUT level 조건(§4.1-3) |
| D-c | STREAM 앱 스레드가 I/O 스레드에서 직접 실행(핸드오프 제거) | 04-thread-safety의 스레드 소유 모델, 02-threading-model |
| D-d | credit published store를 경계로만 축소 | 06-auto-hwm의 스냅샷 정의(재확인 후 확정) |

## 3c. (c) 질문별 답

**wake 빈도 — 패킷마다인가 배치마다인가**
- `signaler_t::send()`(eventfd write)는 **mailbox cpipe가 "비어 있음 → 있음"으로 전이할 때만** 실행된다(`mailbox.cpp:69-80`). 따라서 부하가 높아 mailbox에 command가 쌓여 있으면 **배치당 1회**, 부하가 낮아 앱 스레드가 매번 다 비우면 **메시지당 1회**로 퇴화한다. primary signaler는 coalescing이 아니므로(F7) 이 전이마다 진짜 syscall이 난다.
- 반면 **`activate_read` command 자체는 pipe의 ypipe가 잠들 때마다 = 사실상 메시지마다** 만들어진다(`pipe.cpp:4088, 4109`). 에코는 앱이 매번 pipe를 비우므로 다음 도착이 반드시 잠든 pipe를 깨운다. **wake 비용의 본체는 eventfd가 아니라 이 command 왕복**이다.
- poller 쪽: 소켓 1개이므로 `rebuild()`가 primary notification을 잡고(`socket_poller.cpp:310-313`) 보조 signaler는 쓰지 않는다. `poll(2)`는 배치당 1회, eventfd read는 `mailbox::recv`의 `activate_if_command_pending` 경로에서 배치당 1회.
- 확인 방법: `perf stat -e syscalls:sys_enter_write,syscalls:sys_enter_read -p <server>` 로 메시지 수 대비 비율을 보거나, `zlink::signaler_t::send` / `zlink::mailbox_t::send` / `zlink::pipe_t::flush_unlocked` 심볼의 `perf record -g` 샘플 비율.

**fragment 조립 복사**: RAW 모드에는 없다(F1). 앱(벤치)이 `stream_echo::append_frame_bytes`로 조립하지만, 완전한 프레임이 한 chunk에 담기면 `is_complete_stream_frame` fast path로 **복사 0**이며 수신 msg를 그대로 에코한다(`test_scenario_stream_zlink.cpp:320-330`). 엔진 쪽 `_insize>0` partial prefix `memmove`(`asio_engine.cpp:493-497, 558-562`)만 남고, 이는 프레임이 chunk 경계에 걸릴 때만 발생.

**`packet_record_t` 이동**: RAW 셀에서 **미실행**(F1). PACKET 셀(`zlink_packet`)에서는 이동 생성/대입이 매번 `header.init/close` + `body.init/close` 4회를 하는 값비싼 구현이다(`stream.cpp:88-127`) — `std::deque` 재배치 시마다. R1 리팩토링 항목으로 남길 가치는 있으나 STREAM 격차의 원인은 아니다.

**64-chunk bounded pump 재wake**: RAW 셀에서 **미실행**(`stream.cpp:637-644`). PACKET 전용.

**recv의 API→command-owner→receive 잠금 순서**: 공개 recv 자체는 이 순서를 **타지 않는다**. `receive_once_guarded`가 lock-free lease(CAS)로 receive 소유권을 잡는다(`socket_base_msg.cpp:61`). 전체 3단 순서는 `process_commands`에서만 발생하고(`socket_base_lifecycle.cpp:419-421`), 이는 (i) `poller_wait`마다 1회, (ii) recv 1536회마다 1회, (iii) DONTWAIT recv가 EAGAIN을 만났을 때 1회(`socket_base_msg.cpp:1010-1013`) 뿐이다. **드레인 루프 끝의 (iii)이 배치당 1회**이므로 메시지당 비용은 아니다. 다만 그 안에서 처리되는 command 개수는 메시지 수에 비례하고, command 1개당 `receive.sync` 재귀 mutex(`:538`) + `_out_sync`(`pipe.cpp:2742`) + `receive.sync` 재진입(`socket_base_dispatch.cpp:1676`)이 든다.

**send의 route shard 잠금 + RID 조회**: 메시지마다 **정확히 1회**(`stream.cpp:948-950`). shard 64개, 1000 연결이면 shard당 ~16 엔트리 `std::map`. 추가로 `xsend_writable_target_ready`/`_known`도 같은 잠금을 쓰지만 이들은 **admission 실패 후에만** 호출되므로(`socket_send_complete.cpp:194, 220-225`) 정상 경로에는 없다.

**send의 credit 확인**: 메시지마다 1회, `_out_sync` 안에서 `can_commit_bytes_with_peer_snapshot_unlocked`(`pipe.cpp:2599`). 1차 판정이 성공하면 peer 스냅샷 재로드는 없다(`pipe.cpp:3670-3675`). 실패 시에만 `arm_hwm_credit_wait_unlocked`가 `seq_cst` fence를 친다(`pipe.cpp:1756-1764`). 정상 경로 비용은 원자 load 2–3.

---

## 4. (d) 후보 job 상위 5개

| 순위 | 제목 | 파일 | 예상 이득(거칠게) | 위험 | 재확인할 계약 조항 |
|---|---|---|---|---|---|
| S-1 | **activate_read 왕복 축소** — pipe별 "이미 큐에 있는 activate_read" 플래그로 중복 command 억제 + `process_activate_read`/`read_activated`의 STREAM raw 상수 분기 축약(재분류 3검사 → 캐시 불리언 1) | `core/src/runtime/core/pipe.cpp`(2738-2766, 4066-4112), `sockets/common/socket_base_dispatch.cpp`(1603-1678), `core/mailbox.cpp` | 메시지당 재귀 mutex 2–3쌍 + command 1개 제거 → **5–10%** | 중. "깨어남 조건"을 건드리므로 §4.1-3 위반 여지. drain 경계 회귀(D-099) 주의 | 05-polling(POLLIN level), README recv, 04-thread-safety, 08-stream §5 |
| S-2 | **`fast_mutex_t`/`mutex_t` 비재귀화** — 재진입이 없는 잠금부터 `PTHREAD_MUTEX_DEFAULT`로 | `core/src/runtime/utils/fast_mutex.hpp`, `utils/mutex.hpp`, 재진입 사용처 전수 조사 | 메시지당 7–10 lock/unlock의 상수 절감 → **3–7%**, 전 패턴 공통 이득 | 중~높음. 어딘가 재진입이 있으면 데드락. `scoped_optional_fast_lock_t` 사용처 전수 필요. ASan/TSan 필수 | 없음(구현 세부). 단 04-thread-safety의 잠금 순서 서술 확인 |
| S-3 | **decoder 버퍼 재사용 확대** — spare 1칸 → I/O 스레드 로컬 free-list(원자 제거), 또는 `max_messages_`를 늘려 한 버퍼에서 여러 메시지 절단 | `core/src/runtime/protocol/decoder_allocators.{hpp,cpp}`, `protocol/raw_decoder.cpp`, `engine/asio/asio_raw_engine.cpp:88-89` | 메시지당 malloc+free 1쌍 제거 → **3–8%**(64 B에서 가장 큼) | 중. 버퍼 수명이 앱 스레드까지 이어지므로 보유 바이트 상한이 커질 수 있음 | 05-connection-memory(보유 바이트), 06-auto-hwm, protocol/02-raw |
| S-4 | **STREAM raw의 gather 죽은 경로 제거** — 연결 시점에 gather 지원 여부를 확정해 `prepare_gather_output` 호출 자체를 없앰 | `core/src/runtime/engine/asio/asio_engine.cpp`(689, 757-802, 1350), `asio_raw_engine.cpp:114-124`, `asio_stream_fastpath_policy.hpp` | write turn마다 가상 호출 3 + 분기 6 제거 → **1–3%**. 구조 이득이 더 큼(R2) | 낮음. 동작은 이미 항상 encoder 경로 | 08-stream §371-407(런타임 기본값 문구), 03-io-thread §4 |
| S-5 | **blocking submit 진입 슬림화** — wait guard/컨텍스트를 첫 admission 실패 이후로 지연, `enter_public_send`의 sync 획득을 단일 상태 전이로 | `core/src/runtime/sockets/common/socket_send_submit.cpp`(351-511), `socket_lifecycle_runtime.cpp`(94-155, 416-444) | 메시지당 원자 3–5 + 재귀 mutex 1쌍 경로 단축 → **2–5%**, ROUTER/PAIR 공통 | 중. WRITABLE token 생성·순서 계약이 걸려 있음 | 08-stream §4(122-135행), README "Part send와 pending admission", 03-errors |

보조(작지만 위험이 낮아 묶어 처리할 것): route shard `std::map`→정수 해시(S-6), `_auto_hwm_send_attempts` 원자 완화(S-7), no-op 가상 호출 정리(S-8, Phase 3 R1과 통합).

---

## 5. 불확실한 항목과 확인 방법

| 항목 | 불확실한 이유 | 확인 방법 |
|---|---|---|
| eventfd write/read가 실제로 메시지당 몇 회인가 | mailbox cpipe의 sleep 전이 빈도는 부하 의존 | `perf stat -e 'syscalls:sys_enter_write,syscalls:sys_enter_read'` 를 서버 프로세스에 붙여 recv_msgs로 나눔. 또는 `zlink::signaler_t::send` 호출 카운트를 `perf probe` |
| spare 버퍼 재사용률(= malloc 빈도) | read drain 중 연속 read면 반드시 미스, 아니면 히트 | `perf record -g`에서 `zlink::allocate_buffer` / `_int_malloc` 비율. 또는 `ltrace -c` 대신 `perf probe -x libzlink.so allocate_buffer` |
| `_out_sync` / `receive.sync`의 실제 경합 여부 | 정적으로는 스레드가 다르므로 경합 가능 | `perf record -e 'syscalls:sys_enter_futex'` 또는 `perf lock` |
| `fq_t` round-robin이 1000 pipe에서 몇 번 헛읽는가 | `deactivate_current_after_read_miss`가 amortize하지만 활성 집합 크기에 의존 | `zlink::fq_t::recvpipe_internal` 안의 miss 분기에 `perf probe`, 또는 hotpath_gate `stream_tcp` 셀 명령 수 |
| S-2(비재귀 mutex)의 실제 이득 | glibc 재귀 mutex의 비경합 비용은 수 ns 수준일 수 있음 | `hotpath_gate` 명령 수(callgrind)로 결정적 판정 — 이 캠페인 Phase 0-4에서 추가하는 `stream_tcp` 셀이 정확히 이 용도 |
| `xsend_writable_target_ready`가 정상 경로에 없다는 판정 | 실패 경로에서만 호출된다고 읽었으나 완료 큐 경유가 있을 수 있음 | `zlink::stream_t::xsend_writable_target_ready` 심볼의 perf 샘플이 0에 가까운지 확인 |

## 6. §4.1 관점 요약

- 위 (b)의 1–7·10과 후보 job S-1~S-5는 **공개 헤더·완료/READY/POLLIN/POLLOUT/WRITABLE의 순서·조건을 바꾸지 않는 범위**로 설계했다. 다만 S-1과 S-5는 "언제 깨어나는가"와 "언제 token을 만드는가"에 인접하므로, 채택 전에 08-stream §4 line 122-135와 05-polling의 문장을 코드와 문장 단위로 대조해야 한다.
- D-a~D-d는 구현하지 않고 §7.5(D 목록)로 올린다.
