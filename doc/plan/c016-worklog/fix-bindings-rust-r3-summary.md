# Rust binding R3 결함 수정 결과

감독자가 F-R3-1과 F-R3-13의 원인별 diff와 검증 결과를 검토하기 위한 기록이다.
두 원인을 수정했으며 최종 Rust gate는 **14/14, 실패 0**이다. 공개 API 변경과 commit은 없다.
작업 branch는 `main`, gate 시점 HEAD는 `26603263defee5c3f2c6dfe3e125574fb3608f1c`이다.

## F-R3-1 — WRITABLE RID 재검증 제거

- **소유 계층:** Core가 submit RID echo를 보장하고, Rust completion owner가 socket-local
  context·token으로 찾은 waiter에 결과를 전달한다.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md:986–994`의 part send WRITABLE
  token·context·RID echo, `:1147–1164`의 completion pull ownership과 §6 completion 결과 표.
  `bindings/doc/spec/async-execution-model.ko.md` §4·§5의 단일 owner와 publish/capture 합류,
  `bindings/doc/spec/async-coroutine-policy.ko.md` §3·§5의 완료 합류와 별도 ReplyToken owner
  검증을 유지한다. D-109의 binding 공통 README 반영 문장은 “socket-local context·token으로
  찾은 WRITABLE을 해당 waiter에 전달하며 submit RID echo를 다시 판정하지 않는다”이다.
- **원인:** 수정 전 `bindings/rust/src/internal/completion_owner.rs:715–724`의
  `target_matches`가 Core의 RID를 다시 비교하고 불일치를 `InternalError/EPROTO`로 바꾼다.
- **수정:** 같은 파일 `:700`의 `writable_outcome`에서 RID 비교를 제거했다.
  비교에만 쓰던 `CompletionEntry.expected_target`, `ParkedWritable.peer_rid`와 전달 인자도
  제거했다. 실제 재제출 target은 기존 operation이 계속 소유한다. Kind·token 검사,
  native completion close, context registry 조회와 ReplyToken owner 검증은 유지한다.
- **교차언어 대조:** C의 `bindings/c/include/zlink/socket/api.h:337–349`는 raw completion을
  그대로 노출한다. C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:321–347`과
  Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:529–544`에도 같은 RID
  재검증이 있다. Rust만의 구조적 차이가 아니라 D-109의 공통 원인이며, 이번 diff는 Rust만
  처리한다. 계약에 맞는 Core record에서 결과와 재제출 대상은 같다.
- **변경 분류:** B — 기존의 하위 계층 재검증 제거.
- **수정 전/후 규칙 수:** Rust 범위 2 → 1. Core RID echo 보장과 Rust의 재판정에서
  Core 보장만 남는다. 캠페인 전체의 8 → 1 목표를 모두 완료했다는 뜻은 아니다.
- **대안 판단:** 비교만 지우고 RID 저장·인자를 남기는 안과 비교 전용 상태까지 제거하는
  안을 비교했다. 후자를 선택해 동일 target의 중복 보관도 제거했다.
- **회귀 테스트:** `bindings/rust/src/internal/completion_owner.rs:954`의
  `writable_for_waiter_token_is_delivered_without_peer_rid_reverification`.
  SEND·REQUEST 각각에서 publish 전/후 도착을 검사한다. 비교 대상이 될 수 있는 RID를
  주입해도 일치 token은 정확히 한 번 `Ok(())`를 전달한다. Core가 잘못된 record를
  발행해도 된다는 public 계약이 아니라 binding의 책임 경계를 확인하는 내부 테스트다.
  수정 전에는 새 성공 단언에서 실패했고 수정 후에는 통과했다.
- **기존 단언 처리:** 제거 대상 규칙을 강제하던 `writable_target_must_match`는 `:983`의
  `writable_entry_rejects_non_writable_completion_kind`로 교체했다. 유지해야 하는 kind
  검사를 대상으로 기존 `InternalError` 및 `EPROTO` 단언을 그대로 보존했다.
  Token 불일치, parked replay, terminal errno 테스트의 결과 단언도 유지했다.
- **Gate 결과:** lib 17/17, 신규 RID 테스트와 kind 검사 포함 WRITABLE 테스트 8개를
  5회 실행해 40/40. 최종 전체 gate·samples·clippy·fmt는 아래 공통 결과와 같다.
- **BLOCKERS:** 없음.

## F-R3-13 — Token 없는 BACKPRESSURED 보존

- **소유 계층:** Core가 submit result·errno·completion ID를 결정하고 Rust가 기존 typed
  error로 전달한다. Nonzero token이 있는 BACKPRESSURED만 WRITABLE 대기를 시작한다.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md:977–984`의 unified reservation,
  `:1058–1059`의 REQUEST slot 포화 `BACKPRESSURED/EAGAIN/ID 0/completion 없음`,
  `:1351–1353`의 SEND·REQUEST 공유 slot 검증 조건. 같은 문서 `:964–966`의 blocking
  SEND timeout도 token 없는 EAGAIN을 허용한다.
  `bindings/doc/spec/async-execution-model.ko.md` §5·§7과
  `bindings/doc/spec/async-coroutine-policy.ko.md` §3의 exact submit error 전달 계약을 따른다.
- **원인:** 수정 전 `bindings/rust/src/runtime/messaging/operations/routed_async.rs:286–319`,
  특히 `:309`에서 `Ok`와 `Backpressured`의 ID 0을 모두 `InternalError/EPROTO`로 바꾼다.
- **수정:** 같은 파일 `:307`에서 ID 없는 성공만 기존 protocol error로 처리한다.
  Token 없는 BACKPRESSURED는 기존 `submit_error_from_rc(rc, errno)`로 전달한다.
  최초 제출과 WRITABLE 이후 재제출은 같은 `submit_request_attempt`를 사용한다.
  Nonzero token 대기와 ID·수명 검사는 유지하며 새 오류 매핑이나 재시도 규칙은 추가하지 않았다.
- **교차언어 대조:** C++ `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:225–231`,
  Go `bindings/go/internal/native/dealer_router_request.go:368–404`,
  Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:336–348`은 ID 0일 때
  원래 BACKPRESSURED 오류를 보존한다. Rust의 별도 오분류를 이 동작에 맞췄다.
- **변경 분류:** B — 기존 Core submit 계약과의 불일치 수정.
- **수정 전/후 규칙 수:** 2 → 1. Core 결과 분류와 Rust의 token 없는 실패 재분류에서
  Core 결과 분류만 남는다.
- **대안 판단:** BACKPRESSURED 전용 ID 0 반환 분기를 추가하는 안보다 기존 공통 오류
  전달로 합류하는 안의 규칙이 적어 후자를 선택했다.
- **회귀 테스트:** `bindings/rust/tests/routed_async_tests.rs:183`의
  `request_completion_reservation_exhaustion_preserves_tokenless_backpressure`.
  공개 Rust API로 아직 bind되지 않은 inproc endpoint에 REQUEST를 65,536개 제출해 실제
  Core slot을 채운다. 다음 Future의 첫 poll이 `ZlinkError::Submit`의
  `SubmitResult::Backpressured`, `EAGAIN`으로 끝나고 completion progress가 없음을 단언한다.
  Socket close 후 기존 65,536개 waiter가 모두 `Terminated/ESHUTDOWN`으로 끝나는 것도 확인한다.
  Mock·Core 변경·추가 timeout 없이 수정 전 `InternalError` 오분류를 재현했다.
- **Gate 결과:** 공개 회귀 테스트 5/5, routed async suite 19/19.
  최종 전체 gate·samples·clippy·fmt는 아래 공통 결과와 같다.
- **BLOCKERS:** 없음.

## Diff 분리

| 원인 | 독립 적용할 파일·hunk |
|---|---|
| F-R3-1 | `bindings/rust/src/internal/completion_owner.rs`의 모든 hunk, `bindings/rust/src/runtime/messaging/operations/send_ops.rs`의 entry 생성 hunk, `bindings/rust/src/runtime/messaging/operations/routed_async.rs`의 `register_request` 호출 두 hunk |
| F-R3-13 | `bindings/rust/src/runtime/messaging/operations/routed_async.rs`의 `RequestFuture` 주석 hunk와 `submit_request_attempt`의 ID 0 분류 hunk, `bindings/rust/tests/routed_async_tests.rs`의 새 회귀 테스트 hunk |

두 수정은 서로 독립적으로 적용할 수 있다. 이 보고서는 두 원인의 공통 검증 기록이다.
수정 파일은 위 Rust 파일 4개와 이 보고서뿐이다. 기존 다른 작업의 변경은 보존했다.

## 공통 검증 결과

- 최종 `bindings/rust/tests/run_tests.sh`: **14/14 PASS, 실패 0**.
  Lib와 integration suite 13개 및 samples 항목을 포함한다.
  같은 script가 호출한 `samples/run_samples.sh`의 sample 7개가 모두 통과했다.
- `cargo clippy --all-targets -- -D warnings`: PASS.
- `cargo fmt --check`: PASS. `git diff --check -- bindings/rust`: PASS.
- Core는 `core/build-dev/lib/libzlink.so.0.17.0`이며 gate 전후 SHA-256은
  `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`로 동일하다.
  `ldd`로 routed async 실행 파일이 `core/build-dev/lib/libzlink.so.0`을 로드함을 확인했다.
- Sample 실행 전체에서 `flock /tmp/zlink-samples-gate.lock`을 유지했다.
  Perf benchmark는 실행하지 않았다.

Gate 실행 환경은 다음과 같다. 공통 local Core helper가 `ZLINK_CORE_LIB_DIR`을
`core/build/lib`로 덮어쓰므로 `/tmp/zlink-rust-r3-review/bin/cargo`가 실제 cargo 실행 직전에
`ZLINK_CORE_LIB_DIR="$ZLINK_RUST_NATIVE_DIR"`을 export하고
`/home/hep7/.cargo/bin/cargo`를 실행한다. Repository script는 수정하지 않았다.

```bash
export ZLINK_CORE_SOURCE=local
export ZLINK_CORE_INCLUDE_DIR=/home/hep7/project/zlink/core/include
export ZLINK_RUST_NATIVE_DIR=/home/hep7/project/zlink/core/build-dev/lib
export LD_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib
export CARGO_TARGET_DIR=/tmp/zlink-rust-r3-review/target
export PATH="/tmp/zlink-rust-r3-review/bin:$PATH"
flock /tmp/zlink-samples-gate.lock bash bindings/rust/tests/run_tests.sh
```

첫 gate는 suite 13개가 통과했으나 sample의 `timeout cargo`가 shell 함수로 지정한 경로를
사용하지 못해 존재하지 않는 `core/build/lib`에서 빌드를 시도했다. Sample 실행 전의
검증 환경 실패이며, 외부 executable wrapper로 경로를 전달한 최종 gate에서 해소됐다.
제품 동작이나 테스트 단언을 변경해 이 실패를 우회하지 않았다.

로그는 `/tmp/zlink-rust-r3-review/`에 보존했다:
`f1-before.log`, `f13-before.log`는 수정 전 실제 회귀 실패,
`lib-after.log`, `routed-async-after.log`는 관련 suite,
`f1-repeat-{1..5}.log`, `f13-repeat-{1..5}.log`는 반복 검증,
`gate-attempt-1.log`는 최초 경로 실패, `gate.log`는 최종 gate,
`clippy.log`, `fmt-check.log`는 정적 검사 결과다.

**BLOCKERS: 없음. 남은 테스트 실패: 0.**
