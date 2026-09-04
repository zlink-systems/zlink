# Rust DONTWAIT backpressure 계약 정합 결과

## 결과

- 작업 트리: `/home/hep7hep7/project/zlink-wt-rust` (detached `70a999899813`)
- 변경 범위: `bindings/rust/**`만 변경
- Core: `ZLINK_CORE_SOURCE=local`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`
- Core SHA-256: `3bfbf1a191178836ec11d37e452fe793ee24477d5c87556552b7683ab41db3fb`
- commit/push/checkout, `--core-version`, `scripts/local-package/**`, core build/clean은 실행하지 않음

## API 계약 전/후

| API/영역 | 이전 | 변경 후 |
|---|---|---|
| `SendOp::submit()` | 0.16 SEND completion을 기다리고 Core가 pending payload를 보유한다는 전제 | 최초 DONTWAIT 단일 시도. 즉시 admission은 ID 0/무 completion으로 완료. `BACKPRESSURED + EAGAIN + nonzero token`이면 바인딩이 multipart packet을 보존하고 `POLLOUT`에서 queue를 NO_DATA까지 pull하여 같은 token/context/RID의 WRITABLE만 받아 같은 packet을 재전송 |
| 반복 backpressure | 기존 SEND completion terminal 전제 | 재시도도 backpressured이면 새 token으로 재무장; WRITABLE 하나는 재시도 한 번만 허용 |
| `SendOp::submit_sync()` | Core blocking admission | 동일하게 Core blocking admission 유지; close와 native submit 수명 경계만 직렬화 |
| publish DONTWAIT | one-shot 오류 반환 | `SubmitResult::Backpressured` + `EAGAIN` 반환 유지; SEND/WRITABLE completion 없음 |
| completion kind | SEND=1, REQUEST=2 내부 값 | public `CompletionKind::{Send=1, Request=2, Writable=3}`. SEND=1은 ABI 예약이며 일반 SEND 성공에서 미발행 |
| public `Poller` | `POLLCOMPLETION`에서 queue drain | completion owner인 poller가 `POLLOUT` 또는 `POLLCOMPLETION` ready 시 NO_DATA까지 drain. SEND 구동 시 두 mask를 함께 등록하도록 API 문서화 |
| ROUTER/STREAM 없는 RID | 오류 종류를 테스트가 고정하지 않음 | 첫 poll에서 즉시 `SubmitResult::NotConnected` + `EHOSTUNREACH`; Pending/token 없음 |
| `ZLINK_OPT_PENDING_MAX_*` | SEND pending으로 오해 가능한 서술 | 값과 ABI는 유지하고 REQUEST pending-admission 전용으로 명시; typed Rust SEND option으로 노출하지 않음 |
| REQUEST/reply | REQUEST completion queue 처리 | 기존 REQUEST completion 의미 유지. SEND owner 전환/취소 tombstone이 REQUEST progress를 막지 않도록 owner handshake 보강 |

## 구현 요점

- Core가 모든 SEND 결과에서 native message를 소비하므로, logical `MessageParts`를 바인딩이 보존하고 매 시도마다 native clone을 제출한다.
- completion entry는 stable `user_context`를 유지하며 token/context/RID를 상관한다. 다른 token은 해당 waiter를 깨우지 않고, terminal WRITABLE은 errno 기반 typed `SubmitError`로 변환한다.
- SEND reactor는 별도 OS worker thread, sleep, timer 없이 Future executor turn의 timeout-0 native poller probe로 구동된다. public poller가 completion queue를 소유하면 public `Poller::wait()`가 구동한다.
- socket별 completion drain mutex, public/private owner handoff barrier와 wake, submit/close mutex를 추가해 concurrent drain, lost wake, close 중 raw-handle submit, token-context ABA를 막았다.
- drop/cancel 또는 reactor 오류 뒤에도 살아 있는 Core token의 entry는 payload-free tombstone으로 남기고 정확한 completion drain 또는 socket shutdown 때 제거한다.
- perf one-way helper도 Pending Future를 버리고 새 token을 누적하지 않고 같은 Future/packet을 WRITABLE 재시도 완료까지 유지한다.

## 변경 파일

- 문서/API 계약: `bindings/rust/README.rustdoc.md`, `src/contracts/errors/results.rs`, `src/contracts/eventing/poller.rs`, `src/contracts/messaging/operation_contracts.rs`, `src/contracts/messaging/operations.rs`, `src/contracts/sockets/socket_options.rs`, `src/lib.rs`
- runtime/FFI: `src/internal.rs`, `src/internal/completion_owner.rs`, `src/internal/routed_handle.rs`, `src/runtime/errors/native_errors.rs`, `src/runtime/eventing/poller.rs`, `src/runtime/messaging/operations/mod.rs`, `src/runtime/messaging/operations/routed_async.rs`, `src/runtime/messaging/operations/send_ops.rs`, `src/runtime/native/ffi.rs`, `src/runtime/sockets/socket/socket_inner_runtime.rs`, `src/runtime/sockets/socket/socket_parts_runtime.rs`
- 테스트: `tests/behavior_tests.rs`, `tests/contract_tests.rs`, `tests/routed_async_tests.rs`, `tests/send_failure_tests.rs`, `tests/surface_tests.rs`, `tests/test_support/mod.rs`
- sample/perf: `samples/request_reply_future_sample.rs`, `perf/single/src/common.rs`
- `bindings/rust/include/**`는 변경하지 않음

## 테스트와 gate

- 신규 public 계약 케이스: HWM fill → BACKPRESSURED/token → peer drain → `POLLOUT`/WRITABLE → 같은 multipart 재전송 성공 및 거절 payload 미보관 확인: 최종 코드에서 5/5 green
- completion owner 단위 테스트 6개 green: capture-before-publish, token mismatch 격리, RID 검증, 반복 token 재무장, terminal WRITABLE, detached cleanup
- 추가 회귀: missing ROUTER RID 즉시 `NotConnected/EHOSTUNREACH`, close `Terminated/ESHUTDOWN`, detached SEND token과 동시 REQUEST progress, public REQUEST completion green
- 표준 `bindings/rust/tests/run_tests.sh`: 14/14 PASS (`lib_tests`, 12 integration suites, samples)
- `cargo fmt --all -- --check`: PASS
- `cargo doc --no-deps`: PASS (변경하지 않은 `pubsub_socket_contracts.rs`의 기존 broken intra-doc link warning 2건만 출력)
- `git diff --check -- bindings/rust`: PASS
- raw header mirror: `bindings/rust/include`의 8개 파일을 대응 `core/include`와 `cmp`, 모두 byte-identical
- 모든 test/gate에 `ulimit -v 16777216`, `CARGO_BUILD_JOBS=2`; 표준 runner에 `ZLINK_BUILD_JOBS=3` 적용
- 남은 실패: 없음
