[English](./bindings-cpp-0.18.0.md) | [한국어](./bindings-cpp-0.18.0.ko.md)

# ZLink C++ binding 0.18.0 릴리스 노트

Core 0.18.0 위에서 동작하는 **breaking 릴리스**입니다. 전체 개요·마이그레이션은 [bindings 0.18.0 릴리스 노트](./bindings-0.18.0.ko.md)를 참조하십시오.

## 주요 변경 (breaking)

- 비동기 종결자 `async()`가 결과 객체를 돌려줍니다.
  - `send_submission_t { zlink_submit_result_t result; async_result_t<void> admitted; }`
  - `request_submission_t { result; admitted; async_result_t<std::vector<message_t>> reply; }`
- `result`는 제출 시점 `OK`|`BACKPRESSURED` 스냅샷입니다. `OK`면 `admitted`가 완료 상태, `BACKPRESSURED`면 WRITABLE 재제출로 완료됩니다. admission 실패는 `admitted`·`reply`에 같은 원인으로 전달됩니다(정확히 한 번).
- 동기 `submit()`·publish·reply는 바뀌지 않습니다.

## 마이그레이션

- send: `co_await op.async()` → `co_await op.async().admitted`
- request: `auto reply = co_await op.async()` → `auto sub = op.async(); auto reply = co_await sub.reply`

## 검증

- contract test 두 시나리오 통과, 전체 CTest green, perf(G6) criterion 2·4 PASS(FB-071).

릴리스 태그는 `cpp/v0.18.0`입니다.
