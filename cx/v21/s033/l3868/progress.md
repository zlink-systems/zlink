# #4350 C++ L0 진행

PM 2026-11-12 08:30 UTC 결정 적용: main/control 재측정과 main 교차 시험은 사용하지 않는다. main L8 참조는 r4190w(d3fd955131)의 결과와 raw만 사용한다. 앞선 control 관련 raw 파일은 보존하되 측정 근거에서 제외한다.

L0/binding 6셀과 L0 send-saturation 1K 교차 시험을 진행한다. 모든 측정 직전에 빌드 출처·commit·base·binary SHA-256을 기록한다.

09:00 UTC 정정 반영: 이전 request 측정이 있다면 무효다. 기존 main/control raw는 판정에서 제외한다. 이번에는 L0 source encode·target decode·reply decode도 binding의 protobuf 경로를 사용한다. 수정 commit 7a5b049a9d.

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |

2026-11-12T09:20:53.815966+00:00 — 수정 commit은 준비됐다. cpp-build-slot.sh가 외부 측정의 exclusive lock·공통 gate를 기다려 아직 빌드에 진입하지 못했다. 현재까지 L0 측정값은 없다. 대기 상태: [raw/build-wait-corrected.txt](raw/build-wait-corrected.txt).

2026-11-12T09:44:20.228926+00:00 — cpp-build-slot.sh는 계속 외부 측정 lock·gate 대기 중이다. 아직 컴파일과 측정에 진입하지 못했다. 상태: [raw/build-wait-094420.txt](raw/build-wait-094420.txt).

2026-11-12T10:16:05.942330+00:00 — 첫 빌드의 기존 CLI default 인자 타입 오류 두 곳을 수정해 a58f69592d에 로컬 커밋했다. build→verify→test→seal→measure 연쇄 명령이 외부 측정 lock·gate를 기다린다. 아직 L0 측정값은 없다. [대기 상태](raw/build-wait-101605.txt), [첫 빌드 로그](raw/build-l0-corrected.log).

2026-11-12T10:44:47.598121+00:00 — a58f69592d의 재빌드가 계속 측정 lock·gate를 기다린다. 측정 전 검증과 L0/binding 실행은 빌드 성공 뒤에만 이어진다. 아직 유효 수치는 없다. [대기 상태](raw/build-wait-104447.txt).

2026-11-12T11:14:09.712714+00:00 — a58f69592d 재빌드는 외부 측정 lock·gate 때문에 대기 중이다. 아직 성공한 새 빌드와 유효 L0 측정값이 없다. [대기 상태](raw/build-wait-111409.txt).

2026-11-12T11:45:35.873953+00:00 — 공통 gate가 열려 a58f69592d의 Release 재빌드가 진행 중이다. L0 owner 파일도 컴파일을 통과했다. 아직 측정 전이다. [빌드 로그](raw/build-l0-corrected-final.log).

2026-11-12T11:50:30.442578+00:00 — a58f69592d의 빌드는 성공했다. wire 검사가 raw bench envelope에도 Framework format marker를 요구한 불일치를 발견했다. raw wire의 binding byte 일치 검사와 Framework wire의 실제 codec 검사를 분리하고 손상 입력 거부도 양쪽에 적용해 313a7ccccf에 로컬 커밋했다. 새 빌드·검증 뒤에만 측정한다. [성공 빌드](raw/build-l0-corrected-final.log), [검증 실패 원본](raw/verify-build-corrected.txt).

2026-11-12T12:15:59.150059+00:00 — 313a7ccccf의 wire 검사 수정 빌드가 외부 측정 lock·gate 대기 중이다. 앞선 a58f69592d 빌드는 성공했지만 wire 검사를 통과하지 않아 측정하지 않았다. 아직 유효 L0 수치는 없다. [대기 상태](raw/build-wait-121559.txt).

2026-11-12T12:45:27.479121+00:00 — 313a7ccccf의 재빌드는 계속 외부 측정 lock·gate 대기 중이다. 아직 성공한 수정 빌드·유효 측정값이 없다. 이전 원시 파일은 모두 보존했다. [대기 상태](raw/build-wait-124527.txt).

2026-11-12T13:14:57.414512+00:00 — 313a7ccccf 재빌드가 외부 측정 lock·gate 대기 중이다. 보고서에는 역사 main 기준선과 코드 위치를 먼저 정리했고, 새 L0/binding·교차 시험은 미측정으로 표시했다. 유효 수치는 아직 없다. [대기 상태](raw/build-wait-131457.txt).

2026-11-12T13:41:34.457995+00:00 — 측정 순서를 send-saturation 1K 3쌍 → L0 send 1K 교차 시험 → 나머지 5셀로 배치했다. 동일 raw를 교차 표에 재사용하며 전체 셀·반복·조건은 유지한다. 아직 빌드 슬롯 대기 중이다.

2026-11-12T13:48:24.207962+00:00 — 313a7ccccf의 wire 수정 재빌드는 계속 외부 측정 lock·gate 대기 중이다. 새 바이너리 검증 전 실행은 하지 않았다. L0/binding 및 교차 시험의 유효 수치는 아직 없다. [대기 상태](raw/build-wait-134824.txt).
- 2026-11-12T14:10:23.128555+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024 | built commit=313a7ccccf509864335db3a827d3bc5e663b3976 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=8d8fdb10197dcf612a8240b80db5231c04749710830d9a496c2160b5d609b911 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=1831af8f792f68f4433c352d62f472665b7e54cae4bd60eb2fdd77162f251dca ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `241: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `242: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `243: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `244: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `245: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `246: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `247: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `248: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `250: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `227: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:11:01.094117+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024 | built commit=313a7ccccf509864335db3a827d3bc5e663b3976 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=8d8fdb10197dcf612a8240b80db5231c04749710830d9a496c2160b5d609b911 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=1831af8f792f68f4433c352d62f472665b7e54cae4bd60eb2fdd77162f251dca ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `241: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `242: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `243: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `244: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `245: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `246: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `247: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `248: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `250: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `227: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |

L0 첫 시도는 source startup의 immutable descriptor 검사에서 중단됐다. raw/L0는 보존하고 판정에서 제외한다. 정상 owner 계약대로 preparing → bind 실제 endpoint → serving 순서로 고쳐 새 raw/L0-owner 및 raw/cross/L0-owner에서 재측정한다.
- 2026-11-12T14:15:01.979645+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run1/zlink-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:15:39.919725+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |

# C++ L0 첫 측정

조건 B: source CPU 0–7, target CPU 8–15, warmup 30초, active 5초. 첫 유효 실행이며 중앙값은 후속 보고서에서 판정한다.

| L0 msg/s | Binding msg/s | L0÷binding |
|---:|---:|---:|
| [587,509](/root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/results.json) | [582,017](/root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json) | 1.0094 |

양쪽 모두 errors=0, submitted=completed=received이다.
- 2026-11-12T14:16:53.814424+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run2/zlink-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:18:09.907138+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run2/zlink-framework-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:19:33.805015+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run3/zlink-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:21:09.910378+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-1024/run3/zlink-framework-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:22:53.837777+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/cross/L0-owner/framework-raw/run1/zlink-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:24:13.919396+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/cross/L0-owner/framework-raw/run2/zlink-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:25:49.872198+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/cross/L0-owner/framework-raw/run3/zlink-cpp-send-saturation-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:27:23.990235+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-4096/run1/zlink-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:29:03.840638+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-4096/run1/zlink-framework-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:30:09.587802+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-4096/run2/zlink-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:31:39.490525+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-4096/run2/zlink-framework-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:32:59.291627+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-4096/run3/zlink-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |

2026-11-12T14:33:26.711944+00:00 — 현재 666edd8dc2의 owner 초기화 수정 재빌드는 외부 측정 lock·gate 대기 중이다. source startup 실패 원인은 raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/source.log에 보존했다. 정상 factory(mesh_node_runtime.cpp:727)의 preparing 상태와 service_topology_registry.cpp:179의 endpoint 게시 계약에 맞췄다. 이전 driver·manifest·source hash는 *-313 파일로 별도 보존했고 새 시도는 raw/L0-owner에 둔다. 아직 유효 L0 수치는 없다. [대기 상태](raw/build-wait-143326.txt).
- 2026-11-12T14:34:25.189013+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/send-saturation-4096/run3/zlink-framework-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:35:39.157263+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/profiles-owner/send-saturation-666edd8dc2/run1/zlink-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:36:55.965607+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/profiles-owner/send-saturation-666edd8dc2/run1/zlink-framework-cpp-send-saturation-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:38:04.181741+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-1024/run1/zlink-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:39:13.834645+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-1024/run1/zlink-framework-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:39:53.428756+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-1024/run2/zlink-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:40:30.963713+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-1024/run2/zlink-framework-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:41:08.564382+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-1024/run3/zlink-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:41:46.055465+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-1024/run3/zlink-framework-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:42:23.754111+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/profiles-owner/request-serial-666edd8dc2/run1/zlink-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:43:02.104809+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/profiles-owner/request-serial-666edd8dc2/run1/zlink-framework-cpp-request-serial-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:43:43.875320+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-4096/run1/zlink-cpp-request-serial-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:44:21.371487+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-4096/run1/zlink-framework-cpp-request-serial-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:44:58.887663+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-4096/run2/zlink-cpp-request-serial-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:45:38.383653+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-4096/run2/zlink-framework-cpp-request-serial-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:46:15.861174+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-4096/run3/zlink-cpp-request-serial-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:46:59.416102+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-serial-4096/run3/zlink-framework-cpp-request-serial-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:47:37.029937+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-1024/run1/zlink-cpp-request-backpressure-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:48:14.746048+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-1024/run1/zlink-framework-cpp-request-backpressure-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:50:04.507194+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-1024/run2/zlink-cpp-request-backpressure-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:52:04.409960+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-1024/run2/zlink-framework-cpp-request-backpressure-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:53:40.182990+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-1024/run3/zlink-cpp-request-backpressure-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:55:33.951567+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-1024/run3/zlink-framework-cpp-request-backpressure-1024 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |
- 2026-11-12T14:57:19.725605+00:00 | /root/jobs/fx/cx/v21/s033/l3868/raw/L0-owner/request-backpressure-4096/run1/zlink-cpp-request-backpressure-4096 | built commit=666edd8dc2f780ad5ef5524c053d3e062dfe1c23 dirty=0 | base=0ca8e19bbbba47fb9f4959557e5f8614ed19b341 | THIS worktree=/root/worktree/zlink-v21-s033/l3868 | /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client sha256=afa952ad3e7149d85e9043a2b2eb38638401c037eac5699b938d233de100e318 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_framework_server sha256=c6f717c67b8bef704c66fdd327b67bef700436f8b6394b500228d542ef768bee ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server sha256=7ef7d00517be7557c6461d6fa6f37ccdba6d5ce8a17d526082b4722634a52e84 ; /root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_grpc_server sha256=ecfc225e7c8ad7bf4b242cadd31fd7ab9edde73baf87bc835652848cb1188907

Binding/L0 응답 작업 일치 확인: 새 envelope Message 1개, reply body 문자열 할당·복사 1회, protobuf encode 1회와 encoded 문자열 할당, 새 body Message 할당·복사 1회, 2 parts를 같은 binding reply submit에 전달한다. 수신도 양쪽 ParseFromArray로 새 BenchPayload body를 decode한다. 요청 parts 재사용은 없다.

| binding `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp` | L0 `/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp` |
|---|---|
| `58: const char *envelope = zlink_cpp_bench::response_envelope ();` | `242: const char *envelope = response_envelope ();` |
| `59: zlink::message_t header = zlink::message_t::from (std::as_bytes (` | `243: zlink::message_t header = zlink::message_t::from (std::as_bytes (` |
| `60: std::span<const char> (envelope, std::strlen (envelope))));` | `244: std::span<const char> (envelope, std::strlen (envelope))));` |
| `61: zlink::framework::bench::withgrpc::BenchPayload reply;` | `245: payload_t reply;` |
| `62: reply.set_body (payload.body ());` | `246: reply.set_body (payload.body ());` |
| `63: const std::string encoded = zlink_cpp_bench::encode_bench_payload (reply);` | `247: const std::string encoded = encode_bench_payload (reply);` |
| `64: zlink::message_t echo = zlink::message_t::from (` | `248: zlink::message_t echo = zlink::message_t::from (` |
| `65: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` | `249: std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));` |
| `67: received.reply ().message (header).message (echo).submit ();` | `251: received.reply ().message (header).message (echo).submit ();` |
| `54: payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))` | `228: payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))` |

2026-11-12T15:01:04.647881+00:00 — 측정 중단·정산: 양쪽 3회 완료 5/6셀, 수치상 통과 3/6셀, 교차 3종 완료. send 4K 및 request-serial 1K 기준 미달, request-backpressure 4K 미완료. 자체 측정 lock 오분류와 시간 한도 초과를 정정한다.
