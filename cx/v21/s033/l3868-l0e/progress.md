# #4350 C++ L0 수신·source 비용 일치 실험

시작: 2026-11-13 05:16 UTC. 시간 한도: 2026-11-13 10:16 UTC. Branch `exp/3868-cpp-ladder`, 시작 HEAD `666edd8dc2`, clean. 이전 l3868 raw와 이번 brief.md는 보존한다.

| 결정 | 근거·현재 위치 | 이번 실험 소유자 |
|---|---|---|
| target receive 대기·회전 | raw `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:39–45,82–88`; L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:545–565` | 실제 owner socket의 직접 blocking recv; raw와 동일한 loop |
| source native submit·completion | raw `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:618–647,584–590`; L0 `framework/bench/grpc/cpp/ladder/ladder_driver.inc:31–51,109–164` | binding/Core; source는 같은 raw submit 구현과 POLLCOMPLETION wait 사용 |
| source payload·Message 수명 | raw `bench_cpp_client.cpp:620–625`; L0 `ladder_wire.hpp:101–111` | 기존 raw submit을 한 구현으로 옮겨 재사용 |
| native output buffer 복사 | `core/src/runtime/protocol/encoder.hpp:92` → `core/src/runtime/engine/asio/asio_engine.cpp:1314` | 양쪽 공통 Core 경로; Framework 추가 복사 아님 |

대안: L0 encoder 반환 객체만 줄이면 source encode·submit 판정 위치가 둘로 남는다. 기존 raw submit을 공통 위치로 옮겨 raw/L0가 같은 코드를 실행하도록 한다. Source 제출 알고리즘의 판정 위치 수 2→1. Target의 poller→DONTWAIT 이중 수신 회전을 직접 recv 한 곳으로 바꾼다.

소유 계층은 Core/binding의 물리 receive·admission·completion이며 근거는 messaging hot path §1·§3·§4다. C++ 단독 실험으로 타 언어 제품 runtime과 spec은 바꾸지 않는다. 분류 C: 사용자가 명시적으로 승인한 L0 실험 경로다. 이번 사용자 지시는 수신·source parity 구현과 local commit을 승인했다.

초기 공통 규칙을 읽기 전에 기본 AGENTS 지침에 따라 internal-docs.sh가 한 번 실행됐다. 이번 공통 규칙의 금지를 확인한 뒤 중단했으며, 이후 해당 명령·원격 GitHub·push·PR·sub-agent는 실행하지 않는다.

수신 일치: direct blocking recv 1회/record, 별도 readiness wait·DONTWAIT probe 없음. recv 오류만 raw와 같은 50µs sleep.

| binding `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` |
|---|---|
| `39: zlink::received_t received;` | `548: zlink::received_t received;` |
| `40: while (!g_stop.load ()) {` | `549: while (!stop.load ()) {` |
| `41: const int rc = router.recv (received, zlink::recv_flags_t::none);` | `550: const int rc = _router->recv (received, zlink::recv_flags_t::none);` |
| `42: if (rc != 0) {` | `551: if (rc != 0) {` |
| `43: std::this_thread::sleep_for (std::chrono::microseconds (50));` | `552: std::this_thread::sleep_for (std::chrono::microseconds (50));` |
| `44: continue;` | `553: continue;` |
| `45: }` | `554: }` |

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
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `112: if (_level == 0) {` |
|  | `113: submit_raw (*_socket, _target, std::move (stream), seq);` |
|  | `114: return;` |
|  | `115: }` |

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
| `620: zlink::poll_event_t event{};` | `34: zlink::poll_event_t event{};` |
| `621: while (!_stop)` | `35: while (!_stop)` |
| `622: _poller.wait (&event, 1, std::chrono::milliseconds (50));` | `36: _owner->ablation_poller ().wait (&event, 1, std::chrono::milliseconds (50));` |

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


- 2026-11-13 05:29 UTC: build-slot이 다른 작업의 exclusive 측정 lock을 대기 중이다. 새 성능 측정 0회. [lock 상태](raw/build-lock-wait.txt).

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
| `653: submit_raw (_socket, _target, std::move (stream), seq);` | `112: if (_level == 0) {` |
|  | `113: submit_raw (*_socket, _target, std::move (stream), seq);` |
|  | `114: return;` |
|  | `115: }` |

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
| `620: zlink::poll_event_t event{};` | `34: zlink::poll_event_t event{};` |
| `621: while (!_stop)` | `35: while (!_stop)` |
| `622: _poller.wait (&event, 1, std::chrono::milliseconds (50));` | `36: _owner->ablation_poller ().wait (&event, 1, std::chrono::milliseconds (50));` |

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


- Source 공통화 뒤 호출되지 않는 owner 실험 submit helper 2개와 그 분기를 제거했다. 최종 변경은 4개 파일이며 [patch](raw/source-change.patch)와 [대조](raw/parity-final/reply-parity.md)를 보존했다. 새 per-message 상태·queue·lock은 없다.

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


- L0 source의 미사용 raw-adapter context·poller와 per-stream encoder 생성을 제거했다. 실제 owner의 context·poller는 유지한다. [normal runtime 대조](raw/verify-normal-runtime.txt)는 실험 블록 밖의 runtime이 기존 기준과 같은지 확인했다.

- 2026-11-13T05:36:46.627620+00:00: local commit `73285401ca18c376320a03e06350a4d9b1a60bb5`; clean. [commit](raw/commit.txt). 정적 parity 대조는 통과했으며 빌드·실행 검증은 lock 대기 중이다.

- 2026-11-13T06:16:05.210982+00:00: 시작 뒤 약 1시간이 지났다. build-slot은 계속 다른 작업의 exclusive 측정 lock을 기다리고 있으며 configure·compile·성능 측정은 아직 시작하지 않았다. [lock 상태](raw/lock-wait-061605.txt). 종료 한도는 10:16 UTC다.

- 2026-11-13 06:18 UTC: configure가 시작되지 않은 대기 요청을 종료하고, configure+compile을 단일 cpp-build-slot 호출로 묶어 다시 대기한다. 두 단계 사이의 lock 재획득 대기를 없앴다. 최초 대기 script와 빈 configure log는 queued-attempt 파일로 보존했다. 소스·바이너리·측정 결과는 바뀌지 않았다.

- 2026-11-13T07:16:28.211914+00:00: 시작 뒤 약 2시간. 다른 작업의 exclusive 측정과 대기 중인 writer 때문에 build-slot의 measure gate를 계속 대기 중이다. 소스 commit·정적 parity 검사는 완료, configure·compile·실행 검증·fresh 성능 측정은 아직 시작하지 않았다. [lock 상태](raw/lock-wait-071628.txt). 시간 한도는 10:16 UTC다.
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


- 2026-11-13T08:14:03.593601+00:00: 시작 뒤 약 3시간. build-slot의 measure gate 대기가 이어져 configure·compile·실행 검증·fresh 측정은 아직 시작하지 않았다. 소스 commit `73285401ca`와 정적 parity 검사는 완료됐다. [lock 상태](raw/lock-wait-081403.txt). 10:16 UTC 한도는 유지한다.

- 2026-11-13T08:48:20.640883+00:00: writer가 잠시 없는 구간에도 gate 대기가 이어져 keeper를 읽었다. [관찰한 keeper](raw/measure-gate-keeper-observed.sh)는 writer가 없는 상태를 기본 180초 유지한 뒤 gate를 연다. 그 전에 다음 측정이 시작되면 idle을 0으로 되돌린다. 공용 설정은 바꾸지 않았다.

- 2026-11-13T09:15:53.700168+00:00: 시작 뒤 약 4시간, 10:16 UTC까지 약 1시간 남았다. 공용 writer와 180초 grace 때문에 build-slot gate 대기가 이어진다. 소스 commit·정적 parity 검사는 완료됐으며 configure·compile·wire/bench 실행 검증·fresh 측정은 아직 시작하지 않았다. [lock 상태](raw/lock-wait-091553.txt).

- 2026-11-13T09:45:22.572337+00:00: 한도까지 약 30분. build-slot gate 대기가 이어져 아직 configure·compile·fresh 측정은 없다. [lock 상태](raw/lock-wait-094522.txt). 10:16 UTC에 대기/측정을 종료하고 실제 완료 범위만 보고한다.

- 2026-11-13T10:17:30.845222+00:00: 5시간 한도 종료. 이 작업의 build 대기를 종료했다. configure·compile·wire/bench 실행 검증·fresh matrix/cross 측정은 미실행이다. [최종 상태](raw/final-status.json), [보고서](report.md), [정리 결과](raw/cleanup.txt).
