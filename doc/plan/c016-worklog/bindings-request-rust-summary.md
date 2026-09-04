# Rust REQUEST 계약 통일 결과

## 변경 파일

- `bindings/rust/src/runtime/messaging/operations/routed_async.rs`
  - async REQUEST를 `BACKPRESSURED + wait token -> WRITABLE -> 동일 request 재제출 -> REQUEST completion` 상태기로 변경했다.
  - request payload는 admission 전까지 Future가 소유한다. 재시도용 `Vec` 또는 payload byte snapshot을 만들지 않고, native 시도에는 stack-local `zlink_msg_copy` shared descriptor를 사용한다.
  - 반복 backpressure는 새 token으로 재무장하고, TERMINAL WRITABLE은 `SubmitError`의 `NotFound`/`Terminated`로 전달한다.
  - blocking REQUEST의 admission/reply 의미는 유지했다.
- `bindings/rust/src/internal/completion_owner.rs`
  - REQUEST entry가 WRITABLE 단계와 REQUEST completion 단계를 순서대로 처리하도록 확장했다.
  - public poller drain이 REQUEST의 WRITABLE을 읽은 뒤 entry를 조기 제거하지 않도록 수정했다.
  - REQUEST의 routed target 일치, token 재무장, close shutdown typed 실패를 처리하고 단위 테스트를 추가했다.
- `bindings/rust/src/runtime/messaging/operations/send_ops.rs`
  - 검증된 SEND shared-part submit helper를 REQUEST가 재사용할 수 있도록 module visibility만 확장했다.
- `bindings/rust/src/contracts/messaging/operations.rs`
- `bindings/rust/src/contracts/messaging/operation_contracts.rs`
- `bindings/rust/src/contracts/eventing/poller.rs`
- `bindings/rust/README.rustdoc.md`
  - REQUEST의 WRITABLE 재시도와 admission 이후 completion 계약을 public API 주석에 반영했다.
  - `PENDING_MAX_MSGS/BYTES`는 ABI enum·set/get storage만 유지되고 SEND/REQUEST 모두 무시됨을 명시했다.
- `bindings/rust/src/runtime/native/ffi.rs`
  - `PENDING_MAX_MSGS/BYTES` 주석을 ABI 유지·무시됨으로 정정했다.
- `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs`
  - binding 밖의 pre-admission retry deque와 `Backpressured` 재제출 가정을 제거했다.
  - REQUEST Future가 token/동일 payload 재제출을 소유하게 했고, WRITABLE/REQUEST completion을 같은 public poller lane에서 구동한다.
  - payload를 `Message`에 직접 생성해 기존 `Vec<u8> -> Message` 추가 복사를 제거했으며, idle turn은 poller event를 기다린다.
- `bindings/rust/tests/routed_async_tests.rs`
  - sleep-free public 회귀: HWM REQUEST 재제출/reply, private runtime owner wake, connect-before-bind, close token cleanup, SEND/REQUEST token 혼재, ROUTER target removal typed terminal을 추가했다.
- `bindings/rust/tests/test_support/mod.rs`
  - REQUEST도 첫 Future poll에서 admission을 시작한다는 설명을 반영했다.
- `bindings/rust/tests/contract_tests.rs`
- `bindings/rust/samples/request_reply_future_sample.rs`
  - reply submit 직후 ROUTER를 drop/close해 TCP 전달과 경합하던 기존 수명 race를 제거했다.

## API 전/후

- Public Rust 함수/타입 signature: 변경 없음.
- 이전 async REQUEST: 첫 DONTWAIT 결과의 nonzero completion ID를 admission 성공으로 간주하고 REQUEST completion만 기다렸다. Core가 `BACKPRESSURED + token`을 반환하면 payload가 이미 소비되어 재제출할 수 없었다.
- 이후 async REQUEST: Future가 multipart request를 보유하고 자신의 WRITABLE token에서만 동일 request를 재제출한다. admission 성공 뒤에는 기존 reply/timeout REQUEST completion으로 완료한다.
- blocking `RequestOp::submit_sync()`: 기존 blocking admission과 reply wait 유지.
- `PENDING_MAX_MSGS/BYTES`: ABI 값과 Core set/get storage만 유지하며 동작에는 영향 없음.

## 테스트

- `cargo test --lib`: 10/10 PASS.
- `routed_async_tests`: 18/18 PASS, 최종 5회 반복 PASS.
- `contract_tests`: 26/26 PASS, 수명 race 수정 뒤 5회 반복 PASS.
- `bash bindings/rust/tests/run_tests.sh`: 14/14 PASS(13 test suites + samples 7/7).
- `cargo fmt --all -- --check`: PASS.
- `git diff --check -- bindings/rust`: PASS.
- Core runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`, sha256 `a98cc793457dae04fc58aaafc9cf6fcbe70b021e59cba61b8846aab623061025`; Core build/clean 미실행.

## 스모크 수치

Single: tcp, 1024 B, duration 2, runs 1, status complete 15/15.

| Pattern | Throughput | Bandwidth | Mean / p95 / p99 |
|---|---:|---:|---:|
| DEALER_ROUTER_REQREP | 3,649.5 ops/s | 7.474 MB/s | 0.273 / 0.544 / 0.723 ms |
| ROUTER_ROUTER_REQREP | 2,801.5 ops/s | 5.737 MB/s | 0.356 / 0.573 / 0.647 ms |
| DEALER_ROUTER | 518,243 msg/s | 530.681 MB/s | 0.947 / 2.719 / 4.378 ms |

Report: `/tmp/zlink-rust-perf-req/single/report/perf_rust_single_linux_20260905_011828_req-rust-single.txt`.

Multi: tcp, clients 8, duration 2, runs 1, status complete 30/30, success 6/fail 0.

| Pattern | 1024 B throughput | 65536 B throughput |
|---|---:|---:|
| MULTI_DEALER_ROUTER_REQREP | 121,194.5 ops/s | 9,117 ops/s |
| MULTI_ROUTER_ROUTER_REQREP | 96,696 ops/s | 8,859.5 ops/s |
| MULTI_DEALER_DEALER | 568,473 msg/s | 116,434.5 msg/s |

Report: `/tmp/zlink-rust-perf-req/multi/report/perf_rust_multi_linux_20260905_011855_req-rust-multi.txt`.

## BLOCKERS

- 없음.
- `core/build`, `core/build-dev`는 기존 untracked symlink이며 수정하지 않았다.
