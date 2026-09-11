[English](./bindings-dotnet-0.18.0.md) | [한국어](./bindings-dotnet-0.18.0.ko.md)

# ZLink .NET binding 0.18.0 릴리스 노트

Core 0.18.0 위에서 동작하는 **breaking 릴리스**입니다. 전체 개요·마이그레이션은 [bindings 0.18.0 릴리스 노트](./bindings-0.18.0.ko.md)를 참조하십시오.

## 주요 변경 (breaking)

- 비동기 종결자 `Async()`가 결과 객체를 돌려줍니다.
  - `readonly struct SendSubmission { SubmitResult Result; Task Admitted; }`
  - `readonly struct RequestSubmission { Result; Admitted; Task<IReadOnlyList<Message>> Reply; }`
- **`TrySubmit()` 제거** — `Result == BACKPRESSURED`가 대체합니다. `ct`는 `Admitted`·`Reply` 둘 다에 적용됩니다.
- `Result`는 제출 시점 `OK`|`BACKPRESSURED` 스냅샷입니다. admission 실패는 `Admitted`·`Reply`에 같은 원인(정확히 한 번).
- 동기 `Submit()`·publish·reply는 바뀌지 않습니다.

## 마이그레이션

- send: `await op.Async()` → `await op.Async().Admitted`
- request: `await op.Async()` → `await op.Async().Reply`
- `TrySubmit()` 호출부 → `Async()` 결과의 `Result == BACKPRESSURED` 판정.

## 검증

- contract test 두 시나리오 ×5 통과, 전체 .NET 바인딩 249/249 green, perf(G6) criterion 2·4 PASS(FB-071).

릴리스 태그는 `dotnet/v0.18.0`(NuGet `Zlink`)입니다.
