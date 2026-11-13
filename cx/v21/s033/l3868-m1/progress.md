# #4350/#4351 C++ quick measurement 진행

- 새 common-rules.md의 MEASUREMENT METHOD 적용: 3초 warmup, 5초 active, 기본 1회. 경계 셀만 3회, 1회 셀 G5 없음.
- 시작 branch exp/3868-cpp-ladder, clean HEAD 608513f260. 기존 build 요청 PID 3824279/3824280을 확인했으며 build-l3868은 아직 없다.
- GATE-WAIT 100.5 (기존 L1 build 요청, 13:51 UTC 시작). 대기를 유지한다.
- call-shape.md를 HEAD 소스와 직접 대조했다. binding/L0 receive와 fresh reply는 일치하고 L1 permit만 추가된다.
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `531: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `532: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `533: std::optional<application_job_queue_t::permit_t> permit;` |
| `42: if (rc != 0) {` | `534: if (application_jobs) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `535: permit = application_jobs->wait_for_supply_blocking (` |
| `44: continue;` | `536: application_job_queue_t::origin_t::remote);` |
| `45: }` | `537: if (!permit)` |
|  | `538: return;` |
|  | `539: }` |
|  | `540: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
|  | `541: if (rc != 0) {` |
|  | `542: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
|  | `543: continue;` |
|  | `544: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `243: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `244: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `245: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `246: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `247: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `248: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `249: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `250: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `229: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `232: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `252: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0 || _level == 1) {` |
|  | `115: submit_raw (*_socket, _target, std::move (stream), seq);` |
|  | `116: return;` |
|  | `117: }` |

공유 source 구현 `framework/bench/grpc/cpp/client/bench_cpp_client.cpp`:
```cpp
458: const char *envelope = request_envelope ();
459: auto header = zlink::message_t::from (std::as_bytes (
460: std::span<const char> (envelope, std::strlen (envelope))));
461: const auto encoded = encode_bench_payload (make_payload (stream->size, stream->phase, seq));
462: auto body = zlink::message_t::from (std::as_bytes (
463: std::span<const char> (encoded.data (), encoded.size ())));
464: if (_command) {
```

source 완료 대기 일치: POLLCOMPLETION poller.wait(50ms), recv probe 없음.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `620: zlink::poll_event_t event{};` | `36: zlink::poll_event_t event{};` |
| `621: while (!_stop)` | `37: while (!_stop)` |
| `622: _poller.wait (&event, 1, std::chrono::milliseconds (50));` | `38: _owner->ablation_poller ().wait (&event, 1, std::chrono::milliseconds (50));` |

| record당 작업 · 정상 경로 | binding | L0 |
|---|---|---|
| target wait | blocking recv 1; 별도 poll 0 | 동일 |
| source 완료 wait | POLLCOMPLETION 50ms wait; binding이 callback drain | 동일 |
| source payload 생성 | BenchPayload body 문자열 객체·buffer, protobuf encoded 문자열: 생성 3; Message buffer 2; protobuf encode 1 | 같은 submit_raw; 추가 생성 0 |
| source 명시 복사·쓰기 | body assign 1, stamp 1, protobuf encode write 1, header/body Message::from memcpy 2 | 동일; 빈 Message init·array move 없음 |
| target protobuf decode | body 문자열 객체·buffer 생성 2, decode copy 1 | 동일 |
| request reply 생성 | reply body 객체·buffer 생성 2, encoded 문자열 1, envelope/body Message buffer 2; encode 1 | 동일 |
| request reply 복사 | reply.set_body 1, protobuf encode write 1, Message::from memcpy 2 | 동일; request part 재사용 0 |
| target metrics | server_metrics_t::record 1; mutex 1; counter atomic 0; latency vector의 공통 성장 비용 | 같은 class·호출 1 |
| target stop atomic | stop.load 1/record | 동일 |
| source counters·callback | driver_t::run/stream_t와 observe_binding 공통 구현; sequence.fetch_add 1; callback의 공통 refcount·counter atomic | 동일; L0 추가 atomic 0 |
| binding/Core | 동일한 router send/request/reply·native Message alloc·completion; native encoder buffer copy | 동일 코드·동일 library; 위 표는 binding 내부의 조건부 allocation·atomic을 별도 총계로 추정하지 않음 |

생성 수는 bench가 요청하는 객체·buffer의 수다. allocator cache·protobuf 내부의 세부 malloc 횟수와 binding의 조건부 완료 상태 생성은 총계로 추정하지 않는다. 양쪽이 같은 구현을 호출한다. 근거: message.cpp:108–115, bench_common.hpp:159–165, bench_stats_server.hpp:45–63. L0 fixed_handler는 직접 호출되며 legacy view의 빈 vector는 heap을 할당하지 않는다. Canonical wire adapter는 측정하는 legacy 메시지에 실행되지 않는다.

L1 추가 작업: receive 직전 실제 wait_for_supply_blocking(remote), fixed_handler.handle 직전 release_for_handler_entry. Capacity 여유·waiter 없음·pressure 전이 없음에서는 queue mutex 1회, credits load 2회·fetch_add 1회·fetch_sub 1회다(application_job_queue.hpp:454,903,922,984). Permit 정상 fast path의 새 heap allocation 0·payload copy 0; 추가 counter/metrics atomic 0; 실제 queue의 mutex·credits atomic·shared/weak refcount만 복원한다. Queue가 만드는 비용은 allocation 총계로 추정하지 않는다.

GATE-WAIT 104.6
