# #4350 C++ L0 측정 진행

- 2026-11-13T10:21:08.327282+00:00 지정 브랜치·커밋·clean 확인. Kairos의 BUILD_DONE을 60초 간격으로 기다린다. 별도 빌드 미실행.
- 이전 도구 복사 후 JOB/raw 경로만 교체했다. 이후 이번 지시에 따라 만료된 cap을 BUILD_DONE+4시간으로, 최초 5회 셀을 request-serial 4K로 바꿨다. 제품 소스 변경 없음.
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T10:51:33.062427+00:00 BUILD_DONE rc=0. 변경 source 전체보다 바이너리 최신임을 확인했다([mtime](raw/changed-source-freshness.json)). 지정 run-group으로 exclusive 측정 lock 대기를 시작한다.
- 2026-11-13T11:59:32.408690+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:00:29.756531+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 592,021 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2960803.
- 2026-11-13T12:00:30.474790+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:01:50.094090+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 1024 · 549,184 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2746250.

# C++ L0 첫 측정

조건 B: source CPU 0–7, target CPU 8–15, warmup 30초, active 5초. 첫 유효 실행이며 중앙값은 후속 보고서에서 판정한다.

| L0 msg/s | Binding msg/s | L0÷binding |
|---:|---:|---:|
| [549,184](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/results.json) | [592,021](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json) | 0.9276 |

양쪽 모두 errors=0, submitted=completed=received이다.
- 2026-11-13T12:01:51.628057+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run2/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:03:29.127323+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 588,166 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run2/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2941287.
- 2026-11-13T12:03:29.965899+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run2/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:04:59.617995+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 1024 · 568,016 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run2/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2840361.
- 2026-11-13T12:05:01.266759+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run3/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:09:15.099109+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 569,204 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run3/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2846376.
- 2026-11-13T12:09:15.862099+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run3/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:14:23.952155+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 1024 · 532,980 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-1024/run3/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2675347.

- 2026-11-13T12:14:24.589977+00:00 send-saturation 1024: L0 [549,184](raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/results.json) · 3회 · G5 pass (3.43%); binding [588,166](raw/L0/send-saturation-1024/run2/zlink-cpp-send-saturation-1024/results.json) · 3회 · G5 pass (3.22%); 비율 [0.9337](raw/aggregates/send-saturation-1024.json) · 미달.
- 2026-11-13T12:14:25.596335+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run1/zlink-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:17:48.884391+00:00 fresh 유효 실행: [zlink-cpp send-saturation 4096 · 237,526 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run1/zlink-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1187782.
- 2026-11-13T12:17:49.649051+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run1/zlink-framework-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:19:14.642566+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 4096 · 238,244 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run1/zlink-framework-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1191360.
- 2026-11-13T12:19:15.499171+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run2/zlink-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:20:54.811579+00:00 fresh 유효 실행: [zlink-cpp send-saturation 4096 · 355,151 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run2/zlink-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1777792.
- 2026-11-13T12:20:55.771093+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run2/zlink-framework-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:22:54.826753+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 4096 · 237,739 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run2/zlink-framework-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1188822.
- 2026-11-13T12:22:55.551534+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run3/zlink-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:24:44.620180+00:00 fresh 유효 실행: [zlink-cpp send-saturation 4096 · 238,377 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run3/zlink-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1191968.
- 2026-11-13T12:24:45.378025+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run3/zlink-framework-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:26:24.394940+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 4096 · 237,511 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run3/zlink-framework-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1187667.

- 2026-11-13T12:26:24Z send-saturation-4096: G5 failed at 3 runs; add runs 4/5, preserve all 3 originals.
- 2026-11-13T12:26:25.371459+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run4/zlink-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:28:28.519991+00:00 fresh 유효 실행: [zlink-cpp send-saturation 4096 · 257,300 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run4/zlink-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1286579.
- 2026-11-13T12:28:29.276719+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run4/zlink-framework-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:30:34.416443+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 4096 · 255,402 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run4/zlink-framework-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1277217.
- 2026-11-13T12:30:35.195376+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run5/zlink-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:31:54.239997+00:00 fresh 유효 실행: [zlink-cpp send-saturation 4096 · 247,916 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run5/zlink-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1239641.
- 2026-11-13T12:31:55.008443+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run5/zlink-framework-cpp-send-saturation-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:33:34.196462+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 4096 · 220,187 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/send-saturation-4096/run5/zlink-framework-cpp-send-saturation-4096/results.json); errors=0, submitted=completed=received=1101109.

- 2026-11-13T12:33:35.316317+00:00 send-saturation 4096: L0 [237,739](raw/L0/send-saturation-4096/run2/zlink-framework-cpp-send-saturation-4096/results.json) · 5회 · G5 pass (7.43%); binding [247,916](raw/L0/send-saturation-4096/run5/zlink-cpp-send-saturation-4096/results.json) · 5회 · G5 fail (43.25%); 비율 [0.9590](raw/aggregates/send-saturation-4096.json) · 0.95 통과.
- 2026-11-13T12:33:35.996500+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run1/zlink-cpp-request-serial-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:35:08.704722+00:00 fresh 유효 실행: [zlink-cpp request-serial 1024 · 12,432 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run1/zlink-cpp-request-serial-1024/results.json); errors=0, submitted=completed=received=62161.
- 2026-11-13T12:35:09.573320+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run1/zlink-framework-cpp-request-serial-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:36:08.308443+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 1024 · 12,379 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run1/zlink-framework-cpp-request-serial-1024/results.json); errors=0, submitted=completed=received=61899.
- 2026-11-13T12:36:09.359826+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run2/zlink-cpp-request-serial-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:36:58.152986+00:00 fresh 유효 실행: [zlink-cpp request-serial 1024 · 12,594 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run2/zlink-cpp-request-serial-1024/results.json); errors=0, submitted=completed=received=62973.
- 2026-11-13T12:36:58.927024+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run2/zlink-framework-cpp-request-serial-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:37:43.661851+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 1024 · 12,484 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run2/zlink-framework-cpp-request-serial-1024/results.json); errors=0, submitted=completed=received=62421.
- 2026-11-13T12:37:44.596951+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run3/zlink-cpp-request-serial-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:38:23.394311+00:00 fresh 유효 실행: [zlink-cpp request-serial 1024 · 12,678 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run3/zlink-cpp-request-serial-1024/results.json); errors=0, submitted=completed=received=63393.
- 2026-11-13T12:38:24.144895+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run3/zlink-framework-cpp-request-serial-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:39:18.901391+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 1024 · 12,416 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-1024/run3/zlink-framework-cpp-request-serial-1024/results.json); errors=0, submitted=completed=received=62084.

- 2026-11-13T12:39:19.431495+00:00 request-serial 1024: L0 [12,416](raw/L0/request-serial-1024/run3/zlink-framework-cpp-request-serial-1024/results.json) · 3회 · G5 pass (0.54%); binding [12,594](raw/L0/request-serial-1024/run2/zlink-cpp-request-serial-1024/results.json) · 3회 · G5 pass (1.29%); 비율 [0.9859](raw/aggregates/request-serial-1024.json) · 0.95 통과.
- 2026-11-13T12:39:20.112400+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run1/zlink-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:39:58.884369+00:00 fresh 유효 실행: [zlink-cpp request-serial 4096 · 11,694 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run1/zlink-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58470.
- 2026-11-13T12:39:59.729064+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run1/zlink-framework-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:40:48.532757+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 4096 · 7,725 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run1/zlink-framework-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=38626.
- 2026-11-13T12:40:49.547062+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run2/zlink-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:41:48.312225+00:00 fresh 유효 실행: [zlink-cpp request-serial 4096 · 11,651 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run2/zlink-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58256.
- 2026-11-13T12:41:49.192695+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run2/zlink-framework-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:42:39.887614+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 4096 · 11,676 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run2/zlink-framework-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58381.
- 2026-11-13T12:42:40.969441+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run3/zlink-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:43:59.827584+00:00 fresh 유효 실행: [zlink-cpp request-serial 4096 · 11,740 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run3/zlink-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58701.
- 2026-11-13T12:44:00.705344+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run3/zlink-framework-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:44:53.489010+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 4096 · 11,639 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run3/zlink-framework-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58197.
- 2026-11-13T12:44:54.566262+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run4/zlink-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:45:31.344307+00:00 fresh 유효 실행: [zlink-cpp request-serial 4096 · 11,538 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run4/zlink-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=57693.
- 2026-11-13T12:45:32.117386+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run4/zlink-framework-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:46:28.779882+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 4096 · 11,737 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run4/zlink-framework-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58685.
- 2026-11-13T12:46:29.843685+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run5/zlink-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:47:28.644125+00:00 fresh 유효 실행: [zlink-cpp request-serial 4096 · 11,600 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run5/zlink-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=58002.
- 2026-11-13T12:47:29.381017+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run5/zlink-framework-cpp-request-serial-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:48:10.130721+00:00 fresh 유효 실행: [zlink-framework-cpp request-serial 4096 · 11,584 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-serial-4096/run5/zlink-framework-cpp-request-serial-4096/results.json); errors=0, submitted=completed=received=57921.

- 2026-11-13T12:48:10.643567+00:00 request-serial 4096: L0 [11,639](raw/L0/request-serial-4096/run3/zlink-framework-cpp-request-serial-4096/results.json) · 5회 · G5 fail (33.63%); binding [11,651](raw/L0/request-serial-4096/run2/zlink-cpp-request-serial-4096/results.json) · 5회 · G5 pass (0.97%); 비율 [0.9990](raw/aggregates/request-serial-4096.json) · 0.95 통과.
- 2026-11-13T12:48:11.313782+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run1/zlink-cpp-request-backpressure-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:49:14.323220+00:00 fresh 유효 실행: [zlink-cpp request-backpressure 1024 · 153,480 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run1/zlink-cpp-request-backpressure-1024/results.json); errors=0, submitted=completed=received=780532.
- 2026-11-13T12:49:15.205547+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run1/zlink-framework-cpp-request-backpressure-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:51:54.185594+00:00 fresh 유효 실행: [zlink-framework-cpp request-backpressure 1024 · 155,643 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run1/zlink-framework-cpp-request-backpressure-1024/results.json); errors=0, submitted=completed=received=792110.
- 2026-11-13T12:51:55.068346+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run2/zlink-cpp-request-backpressure-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:54:24.123014+00:00 fresh 유효 실행: [zlink-cpp request-backpressure 1024 · 155,278 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run2/zlink-cpp-request-backpressure-1024/results.json); errors=0, submitted=completed=received=790496.
- 2026-11-13T12:54:24.997120+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run2/zlink-framework-cpp-request-backpressure-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:57:00.062375+00:00 fresh 유효 실행: [zlink-framework-cpp request-backpressure 1024 · 156,377 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run2/zlink-framework-cpp-request-backpressure-1024/results.json); errors=0, submitted=completed=received=795652.
- 2026-11-13T12:57:00.929841+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run3/zlink-cpp-request-backpressure-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T12:59:29.986308+00:00 fresh 유효 실행: [zlink-cpp request-backpressure 1024 · 149,369 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run3/zlink-cpp-request-backpressure-1024/results.json); errors=0, submitted=completed=received=760836.
- 2026-11-13T12:59:30.859788+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run3/zlink-framework-cpp-request-backpressure-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:01:49.853386+00:00 fresh 유효 실행: [zlink-framework-cpp request-backpressure 1024 · 158,392 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-1024/run3/zlink-framework-cpp-request-backpressure-1024/results.json); errors=0, submitted=completed=received=806099.

- 2026-11-13T13:01:50.171086+00:00 request-backpressure 1024: L0 [156,377](raw/L0/request-backpressure-1024/run2/zlink-framework-cpp-request-backpressure-1024/results.json) · 3회 · G5 pass (1.29%); binding [153,480](raw/L0/request-backpressure-1024/run1/zlink-cpp-request-backpressure-1024/results.json) · 3회 · G5 pass (2.68%); 비율 [1.0189](raw/aggregates/request-backpressure-1024.json) · 0.95 통과.
- 2026-11-13T13:01:50.826207+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run1/zlink-cpp-request-backpressure-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:04:23.663453+00:00 fresh 유효 실행: [zlink-cpp request-backpressure 4096 · 129,600 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run1/zlink-cpp-request-backpressure-4096/results.json); errors=0, submitted=completed=received=648421.
- 2026-11-13T13:04:24.503119+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run1/zlink-framework-cpp-request-backpressure-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:07:09.562754+00:00 fresh 유효 실행: [zlink-framework-cpp request-backpressure 4096 · 131,626 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run1/zlink-framework-cpp-request-backpressure-4096/results.json); errors=0, submitted=completed=received=658359.
- 2026-11-13T13:07:10.399851+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run2/zlink-cpp-request-backpressure-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:09:39.381867+00:00 fresh 유효 실행: [zlink-cpp request-backpressure 4096 · 138,697 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run2/zlink-cpp-request-backpressure-4096/results.json); errors=0, submitted=completed=received=693750.
- 2026-11-13T13:09:40.151314+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run2/zlink-framework-cpp-request-backpressure-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:11:45.087752+00:00 fresh 유효 실행: [zlink-framework-cpp request-backpressure 4096 · 136,617 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run2/zlink-framework-cpp-request-backpressure-4096/results.json); errors=0, submitted=completed=received=683322.
- 2026-11-13T13:11:45.963420+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run3/zlink-cpp-request-backpressure-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:13:44.904058+00:00 fresh 유효 실행: [zlink-cpp request-backpressure 4096 · 124,930 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run3/zlink-cpp-request-backpressure-4096/results.json); errors=0, submitted=completed=received=624872.
- 2026-11-13T13:13:45.657608+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run3/zlink-framework-cpp-request-backpressure-4096 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:15:54.703029+00:00 fresh 유효 실행: [zlink-framework-cpp request-backpressure 4096 · 129,788 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/L0/request-backpressure-4096/run3/zlink-framework-cpp-request-backpressure-4096/results.json); errors=0, submitted=completed=received=649356.

- 2026-11-13T13:15:54.953559+00:00 request-backpressure 4096: L0 [131,626](raw/L0/request-backpressure-4096/run1/zlink-framework-cpp-request-backpressure-4096/results.json) · 3회 · G5 pass (3.79%); binding [129,600](raw/L0/request-backpressure-4096/run1/zlink-cpp-request-backpressure-4096/results.json) · 3회 · G5 pass (7.02%); 비율 [1.0156](raw/aggregates/request-backpressure-4096.json) · 0.95 통과.
- 2026-11-13T13:15:55.724581+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/raw-raw/run1/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:17:58.938117+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 566,423 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/raw-raw/run1/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2832396.
- 2026-11-13T13:17:59.785610+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-raw/run1/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:19:59.012173+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 575,716 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-raw/run1/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2879398.
- 2026-11-13T13:20:00.597244+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-framework/run1/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:22:19.788115+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 1024 · 568,252 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-framework/run1/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2841296.
- 2026-11-13T13:22:21.474057+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/raw-raw/run2/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:24:24.775570+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 562,800 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/raw-raw/run2/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2814378.
- 2026-11-13T13:24:25.552602+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-raw/run2/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:26:24.821980+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 586,527 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-raw/run2/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2933398.
- 2026-11-13T13:26:26.411973+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-framework/run2/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:28:39.657906+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 1024 · 581,952 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-framework/run2/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2910468.
- 2026-11-13T13:28:41.247067+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/raw-raw/run3/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:31:00.472921+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 568,635 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/raw-raw/run3/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2843444.
- 2026-11-13T13:31:01.439324+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-raw/run3/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:33:00.758086+00:00 fresh 유효 실행: [zlink-cpp send-saturation 1024 · 581,040 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-raw/run3/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2905837.
- 2026-11-13T13:33:02.343408+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-framework/run3/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:35:19.515068+00:00 fresh 유효 실행: [zlink-framework-cpp send-saturation 1024 · 579,705 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/cross/L0/framework-framework/run3/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2898938.
- 2026-11-13T13:35:21.495401+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/profiles/send-saturation-1024/run1/zlink-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:37:45.798008+00:00 진단 profile: [zlink-cpp send-saturation 1024 · 559,165 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/profiles/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2796213.
- 2026-11-13T13:37:46.555780+00:00 | /root/jobs/fx/cx/v21/s033/l3868-l0f/raw/profiles/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024 | built commit=73285401ca18c376320a03e06350a4d9b1a60bb5 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=67c98550037bc6a096872af7150dd3230a9406d35e9669c93a89b8254d871c6b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=617d7e8e99813e680d35d61613fdd6c88da94c4867d3bb6cafdaa453c3cdd89b ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=9a545b2ac6a421547ffbdcbbf2a21c6436b1b72886c27931aeddcf822d45638f ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ab8f3c49434513c23f80cf345fd0661ebd358e5ae7c535bb002040702bd6528b
RCVTIMEO도 일치한다: raw server는 recv timeout을 설정하지 않고, L0 owner_options도 receive_timeout을 설정하지 않는다. raw_mesh_node_options_t의 receive_timeout은 기본 null optional이며 native 기본 blocking 대기를 유지한다. 근거: raw_mesh_node_owner.hpp:81 및 owner_options.


수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `529: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `530: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `531: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `532: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `533: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `534: continue;` |
| `45: }` | `535: }` |

응답 일치: 새 envelope·새 protobuf reply body·encode·새 body Message·2-part submit.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `54: if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ())))` | `228: } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `56: metrics.record (payload.body ().data (), payload.body ().size ());` | `231: metrics.record (payload.body ().data (), payload.body ().size ());` |

수신 decode·counter·reply submit 대조

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

source 일치: 같은 submit_raw 구현을 호출하며 header→payload encode→body 수명과 callback 등록도 동일.

| binding `framework/bench/grpc/cpp/client/bench_cpp_client.cpp` | L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc` |
|---|---|
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `114: if (_level == 0) {` |
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


- 2026-11-13T13:39:50.836001+00:00 진단 profile: [zlink-framework-cpp send-saturation 1024 · 539,259 msg/s](/root/jobs/fx/cx/v21/s033/l3868-l0f/raw/profiles/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/results.json); errors=0, submitted=completed=received=2696562.

- 2026-11-13T13:46:24.471922+00:00 전체 측정·target frame 대조·보고서 작성 완료. [최종 raw 검사](raw/final-audit.json), [보고서 수치·링크 양성/음성 검사](raw/report-validation.json), [build directory 정리·clean branch](raw/cleanup.json).
