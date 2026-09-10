[English](./bindings-0.18.0.md) | [한국어](./bindings-0.18.0.ko.md)

# ZLink bindings 0.18.0 릴리스 노트

Core 0.18.0 위에서 동작하며, **공개 API가 바뀌는 breaking 릴리스**입니다. 7언어 binding의 비동기
submit 종결자가 Core 제출 결과를 호출자에게 돌려줍니다. 1.0 이전이므로 호환성을 유지하지 않습니다.

## 주요 변경 (breaking)

- **비동기 submit 종결자가 결과 객체를 돌려줍니다.** 이전에는 `submit()`/`async()`/`Async()`가 완료
  하나(reply 또는 void)만 반환했으나, 이제 제출 시점 결과와 완료를 분리한 결과 객체를 돌려줍니다.
  - `SendSubmission`: `result`(`OK`|`BACKPRESSURED`, 제출 시점 스냅샷)와 `admitted`(admission 완료 stage).
  - `RequestSubmission`: 여기에 `reply`(응답 stage)를 더합니다 — 이전 `submit()`이 돌려주던 값입니다.
  - `result == OK`면 `admitted`는 이미 완료 상태이고, `BACKPRESSURED`면 binding이 입력을 보관·재제출해
    WRITABLE에서 `admitted`를 완료합니다. admission 실패는 `admitted`와 `reply`에 같은 원인으로 전달됩니다
    (정확히 한 번). `OK`|`BACKPRESSURED` 외 제출 실패는 지금처럼 예외/에러입니다.
- **동기 종결자(`submit_sync()`, .NET·C++ `Submit()`/`submit()`)·publish·reply는 바뀌지 않습니다.**
- 언어별 반환형: Java `SendSubmission`/`RequestSubmission`(`result()`·`admitted()`·`reply()`), .NET
  `readonly struct`(`Result`·`Admitted`·`Reply`) — **`TrySubmit()` 제거**(`Result == BACKPRESSURED`가 대체),
  C++ `send_submission_t`/`request_submission_t`, Node `SendSubmission`/`RequestSubmission`(동기 `result` 필드 +
  `admitted`/`reply` Promise), Go `Submit(ctx)`가 즉시 결과 객체 반환 + `Result()`/`Admitted(ctx)`/`Reply(ctx)`,
  Rust `Result<SendSubmission>`/`Result<RequestSubmission>`(boxed future), Python `SendSubmission`/`RequestSubmission`
  (`result()`·`admitted()`·`reply()`).
- **Framework**(F1·F2·F2-a): 공개 terminal은 backpressure를 노출하지 않고(내부 구현만 결과 객체 소비),
  동기 blocking 종결자를 추가하며(runtime 실행 문맥에서 호출 시 `InvalidOperation`), messaging call 계약은 유지됩니다.

## 왜

request `submit()`이 admission과 reply를 한 stage로 합쳐, producer가 backpressure를 볼 수 없었습니다.
socket 하나로는 깊이가 1에 묶여 multi perf의 `ROUTER_ROUTER_REQREP` clients=1이 ~8.5k/s에 머물렀습니다.
결과 객체로 admission과 reply를 분리하니 `OK`에서 연속 제출해 HWM 깊이까지 파이프라인됩니다 — clients=1이
~300k/s로 약 38배 개선되었고(C reference 동급), clients=100의 회귀는 없습니다(설계·측정: `doc/draft/bindings-submit-result-terminal.ko.md`,
`doc/plan/fw-bench-worklog/decisions.ko.md` FB-071).

## 마이그레이션

- send: `await op.submit()` → `await op.submit().admitted`(또는 Go `sub, _ := op.Submit(ctx); sub.Admitted(ctx)`).
  backpressure를 의식하지 않는 단순 전송은 `admitted`만 기다리면 됩니다.
- request: `reply = await op.submit()` → `sub = op.submit(); reply = await sub.reply`(Go `sub.Reply(ctx)`).
- .NET `TrySubmit()` 사용처는 `Async()`의 `Result == BACKPRESSURED` 판정으로 바꿉니다.
- 동기 `submit_sync()`·`Submit()`·publish·reply는 변경이 필요 없습니다.

스펙: `bindings/doc/spec/async-coroutine-policy.*` §6, `bindings/doc/spec/README.*`(Submit 결과 투영), 언어별 `bindings/doc/spec/<lang>/README.*`.

## 검증

- 7언어 contract test에 두 시나리오(즉시 admission→`result==OK`·완료된 `admitted`; HWM→`BACKPRESSURED`·WRITABLE 뒤
  `admitted`·request는 `reply`) 추가·통과.
- bindings spec·guide에 옛 종결자 시그니처 0건(grep 확인).
- perf(G6, Core 0.18.0 고정): criterion 2(clients=1 깊이 탈출) PASS, criterion 4(clients=100 회귀 없음) PASS
  — 과거 0.17.5 결과와 size·버전 교차 확인(FB-071).

릴리스 태그는 `<language>/v0.18.0`(4언어), go·rust·python은 매니페스트 동기화입니다.
