[English](./bindings-java-0.18.0.md) | [한국어](./bindings-java-0.18.0.ko.md)

# ZLink Java binding 0.18.0 릴리스 노트

Core 0.18.0 위에서 동작하는 **breaking 릴리스**입니다. 전체 개요·마이그레이션은 [bindings 0.18.0 릴리스 노트](./bindings-0.18.0.ko.md)를 참조하십시오.

## 주요 변경 (breaking)

- 비동기 종결자 `submit()`이 결과 객체를 돌려줍니다.
  - `SendSubmission { SubmitResult result(); CompletionStage<Void> admitted(); }`
  - `RequestSubmission { result(); admitted(); CompletionStage<List<Message>> reply(); }`
- `result()`는 제출 시점 `OK`|`BACKPRESSURED` 스냅샷입니다. `OK`면 `admitted()`가 완료, `BACKPRESSURED`면 WRITABLE 재제출로 완료됩니다. admission 실패는 `admitted()`·`reply()`에 같은 원인(정확히 한 번). Kotlin은 `admitted()`·`reply()`를 `await()`합니다.
- 동기 `submit_sync()`·publish·reply는 바뀌지 않습니다.

## 마이그레이션

- send: `op.submit()` → `op.submit().admitted()`
- request: `op.submit()` (reply stage) → `op.submit().reply()`

## 검증

- contract test 두 시나리오 ×5 통과, Java unit/integration/Netty/Kotlin·sample green, perf(G6) criterion 2·4 PASS(FB-071).

릴리스 태그는 `java/v0.18.0`(Maven Central `systems.zlink:zlink`)입니다.
