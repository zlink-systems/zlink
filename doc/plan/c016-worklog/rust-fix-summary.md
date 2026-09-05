# zlink Rust typed submit 결과 보존 수정 요약

## 원인 (`file:line`)

- `bindings/rust/src/runtime/errors/native_errors.rs:168-173`의 기존 `check_submit_rc`는 nonzero native submit rc를 버리고 `last_errno()`로 `SubmitResult`를 다시 분류했다.
- async SEND/REQUEST도 `bindings/rust/src/runtime/messaging/operations/send_ops.rs:397-408`, `bindings/rust/src/runtime/messaging/operations/routed_async.rs:294-317`에서 typed rc보다 errno를 기준으로 최종 실패를 만들었다. 그 결과 `NOT_ADMITTED + EPROTOTYPE` 같은 조합이 `InternalError`로 손실됐다.
- 기존 errno fallback의 `bindings/rust/src/runtime/errors/native_errors.rs:11-29`는 `ENOBUFS`를 `OutOfMemory`로 분류했고, wait-token terminal은 일반 submit errno mapper를 재사용해 terminal에 허용되지 않는 분류도 만들 수 있었다.

## 소유 계층·spec 조항·교차언어·분류

- 소유 계층: Core가 `zlink_submit_result_t`와 wait-token terminal errno를 결정하고 Rust binding runtime이 이를 Rust typed error로 손실 없이 투영한다.
- spec 조항: `bindings/doc/spec/README.ko.md:4030-4050`, `:4210-4228`(모든 submit enum 값 1:1 typed 매핑); `core/doc/spec/core/socket/README.ko.md:1018-1055`(ROUTER→DEALER request `NOT_ADMITTED`, DONTWAIT backpressure/wait token/terminal); `core/doc/spec/core/03-errors.ko.md:334-352`, `:519-531`(result 우선, errno 보조, `ENOBUFS` backpressure).
- 교차언어 대조: C++ `operation_submit.hpp`는 native typed rc를 직접 `submit_result_t`로 보존하고 terminal을 `ENOENT→not_found`, `ETERM/ESHUTDOWN→terminated`로 정규화한다. .NET도 native result를 typed code로 보존하고 같은 terminal 규칙을 사용한다. Rust만 errno 재분류 경로가 있어 수정이 필요했다.
- 변경 분류: **B — 기존 결함**.

## 변경

- native submit rc 0–13 전 값을 기존 `SubmitResult` variant에 명시적으로 1:1 매핑했다. 알려진 typed rc는 errno와 무관하게 보존하며 errno는 `SubmitError.native_errno` 보조 정보로만 붙인다. `-1` legacy 실패만 errno fallback을 사용한다.
- sync send/publish/request/reply의 공통 `check_submit_rc`와 async SEND/REQUEST 최종 분기를 rc 우선으로 변경했다.
- errno fallback을 Core 표에 맞춰 보완하고 `ENOBUFS`를 `Backpressured`로 변경했다.
- wait-token `ZLINK_SEND_TERMINAL`을 `ENOENT→NotFound`, `ETERM/ESHUTDOWN→Terminated`, 그 외 `InternalError`로 좁혀 정규화했다.
- contract test를 raw errno 숫자에 의존하지 않도록 강화하고 ROUTER가 DEALER RID로 request할 때 `NotAdmitted`가 즉시 반환되는 회귀 테스트를 추가했다. 모든 enum 매핑, `ENOBUFS`, terminal 정규화 unit test도 추가했다.
- Clippy 1.97 전체-target gate가 지적한 테스트 전용 no-op waker와 byte slice 표현을 표준 형태로 정리했다.
- 공개 타입/variant 추가 없음. spec 수정 없음.

## 테스트·게이트 수치

- 신규 `router_request_to_a_dealer_is_immediately_not_admitted`: **5/5 통과**.
- 집중 검증: lib **13/13**, `send_failure_tests` **9/9**, `routed_async_tests` **18/18** 통과.
- 공식 `bindings/rust/tests/run_tests.sh`: **14/14 suite 통과**, samples 포함, `RESULT: PASS`.
- `cargo clippy --all-targets -j3 -- -D warnings`: 통과, 경고 0.
- `cargo fmt --check`: 통과.
- `git diff --check`: 통과.
- Cargo target: `/home/hep7hep7/project/zlink-work/c016/rust-target-fix`.
- Native runtime: 기존 local Core `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`, SHA-256 `c9c1cb987046534a693d28a6fa6880666eb057c179f7f323d0c65fd6da480beb`; Core 재빌드 없음.

## BLOCKERS

- 없음.
