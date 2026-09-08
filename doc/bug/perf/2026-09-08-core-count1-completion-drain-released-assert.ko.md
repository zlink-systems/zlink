# Core: 1,025-part reply를 받는 request future에서 `release_count1_completion_drain` assertion (SIGABRT)

- 보고: 머신 A(bindings 성능 캠페인), 2026-09-08 14:15
- Core: `core/v0.17.3-alpha`(= `core/v0.17.3` 코드) 고정 prefix, Linux x64 WSL2, Release+LTO
- 재현 주체: Rust binding ownership 테스트 `request_future_preserves_more_than_1024_reply_parts`
  (`bindings/rust/tests/ownership_tests.rs:188`), codex rust-cost-map job이 발견, `--test-threads=1` 직렬 재실행에서도 재현

## 현상

```
test request_future_preserves_more_than_1024_reply_parts ...
Assertion failed: released (core/src/runtime/sockets/common/socket_base_api.cpp:1641)
process didn't exit successfully: ownership_tests (signal: 6, SIGABRT)
```

`socket_base_api.cpp:1630-1645`(0.17.3):
```cpp
if (result == socket_reqrep_internal::completion_pipe_public_head) {
    if (count1_application) {
        const bool released = release_count1_completion_drain (completion_pipe_);
        zlink_assert (released);          // ← 1641
```

## 테스트가 하는 일
ROUTER bind + DEALER connect(inproc). server thread가 `router.recv`로 request를 받고 **1,025개 part**로 reply를 보낸다.
client는 DEALER의 request future로 reply를 기다린다. 앞선 ownership 테스트(1024 이하 part)는 통과한다 — 1,025 = 1,024를 넘는
첫 값이므로 reply part 수 상한(1,024) 경계에서 completion pipe의 count1 drain 해제가 실패하는 것으로 보인다.

## 확인하지 못한 것
- `core/v0.17.2`에서도 나는지(캠페인 prefix 규칙상 이 테스트를 0.17.2로 돌리지 않았다).
- 다른 언어 binding(C++·.NET·Java)에 같은 1,025-part 테스트가 있는지.

## 요청
0.17.4 범위에서 (1) 1,025-part reply의 count1 completion drain 경로 확인, (2) Core 통합 테스트에 part 수 경계(1,024/1,025)
request/reply 회귀 추가. 성능 캠페인 쪽 영향: perf 러너는 2-part만 쓰므로 측정에는 영향 없음; Rust binding 전체 테스트가 이
1건으로 SIGABRT라 러너 수정 검증에서 이 테스트만 제외하고 있다.
