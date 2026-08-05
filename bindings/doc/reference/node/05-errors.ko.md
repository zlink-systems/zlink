한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 core의 result-enum-family 표에 대응하는 이 레퍼런스의 대응
문서다 — 공유 error 기반과, 모든 submit/request/recv/handler/close/
bind/connect/config 실패 API(Sockets/Messaging/Eventing/Core category)가
던지는 7개 typed error를 문서화한다. 정확한 signature는
[`contracts/errors/`](../../../../bindings/node/src/zlink/contracts/errors/)가
소유한다.

---

## Typed error family

각 API family는 단일 공유 error 타입이 아니라 typed result 상수를 담은
자신만의 typed error를 던진다 — caller는 구체적 타입(또는 공유
`ZlinkError` 기반)을 잡아 `.result`를 읽는다. 모든 typed error는 내부의,
**export되지 않은** `ResultError<TResult>` class(java의 package-private
`TypedZlinkException`에 대응)를 확장하며, 이는 다시 public
`ZlinkError`를 확장한다.

| Error | Result 상수 | 던지는 곳 | 값 |
|---|---|---|---|
| `SubmitError` | `SubmitResult`(Sockets category) | send/publish/request-submit API | `Backpressured`(1, 정상 제어 흐름), `NotConnected`(2), `NotFound`(3), `Terminated`(4), `InvalidHandle`(5), `InvalidArgument`(6), `NotSupported`(7), `InvalidState`(8), `ThreadViolation`(9), `OutOfMemory`(10), `SeqExhausted`(11), `InternalError`(12), `NotAdmitted`(13, 정상 제어 흐름) |
| `RequestError` | `RequestResult` | request/reply 완료 | `TimedOut`(101), `NotFound`(102), `Terminated`(103), `ProtocolError`(104), `InternalError`(105), `Rejected`(106), `Conflict`(107), `Busy`(108), `NotConnected`(109), `InvalidArgument`(110), `InvalidState`(111), `NotSupported`(112), `Backpressured`(113) |
| `RecvError` | `RecvResult` | recv-family API | `NoData`(201), `Busy`(202), `Terminated`(203), `InvalidHandle`(204), `NotSupported`(205), `InternalError`(206), `BufferTooSmall`(207), `InvalidState`(208) — **이 binding의 `RecvResult`는 `BufferTooSmall`/`InvalidState`를 포함한다, go와 일치**; 다른 모든 언어는 이 값들이 없는 6개 값 집합을 공유한다 |
| `HandlerError` | `HandlerResult` | handler 등록 API | `InvalidArgument`(301), `Busy`(302), `NotSupported`(303), `Deadlock`(304), `InvalidHandle`(305), `InternalError`(306) |
| `CloseError` | `CloseResult` | `close()` 경로, `Context.shutdown()` | `Busy`(401), `Shutdown`(402), `InvalidHandle`(403), `InternalError`(404) |
| `BindError` | `BindResult` | `Socket.bind(...)` | `InvalidArgument`(501), `AddrInUse`(502), `NotSupported`(503), `InvalidHandle`(504), `InternalError`(505) |
| `ConnectError` | `ConnectResult` | `connect`/`unbind`/`disconnect`/`disconnectRid` | `InvalidArgument`(601), `NotSupported`(602), `InvalidHandle`(603), `InternalError`(604), `NotFound`(605), `Conflict`(606), `Busy`(607), `AuthFailed`(608) — **이 binding은 `AuthFailed`가 있다, go와 일치**; 다른 모든 언어는 이 값이 없는 7개 값 집합을 공유한다 |
| `ConfigError` | `ConfigResult` | 모든 socket/context option getter/setter | `InvalidHandle`(701), `InvalidArgument`(702), `NotSupported`(703), `InternalError`(704), `InvalidState`(705), `NotFound`(706), `Conflict`(707), `BufferTooSmall`(708), `Busy`(709) — 9개 값 전체 집합(dotnet과 일치, cpp/java의 6개 값 집합과 다름) |

**언어간 비대칭, 여기서 바로잡음.** 모든 wrapper binding의 result 상수
집합은 core의 `zlink_*_result_t` family(core Errors category에
문서화됨)를 정확히 반영해야 하지만, 서로 일치하지 않는다: dotnet의
`RecvResult`/`ConnectResult`는 각각 `BufferTooSmall`/`InvalidState`와
`AuthFailed`가 빠져 있다. cpp와 java의 `ConfigResult`는
`NotFound`(706)에서 멈춰 `Conflict`/`BufferTooSmall`/`Busy`가 빠져
있다. 여기 문서화된 node의 세 result 집합은 core의 완전한 정의와
일치한다 — **go도 마찬가지다**, 그러니 이건 node 단독 특성이 아니라
두 binding의 짝이다. 다른 binding이 이 빠진 값을 가져야 하는지,
아니면 node/go를 줄여서 맞춰야 하는지는 스펙 차원의 질문이며 이
레퍼런스의 범위 밖이다 — 이 항목은 그 사실이 묻히지 않도록
명시해둔다.

**각 값 family가 실제로 뜻하는 것.** `SubmitResult`의 `Backpressured`/
`NotConnected`/`NotFound`/`NotAdmitted`는 예외적 실패가 아니라 정상적인
실행 흐름이다 — non-`Ok` submit 결과를 전부 같게 취급하는 caller는
"재시도가 합리적"과 "이대로 제출하면 절대 성공하지 않음"의 구분을
잃는다. `RecvResult`/`ConfigResult`의 `BufferTooSmall`은 caller가
제공한 output 용량이 첫 완결된 값을 담을 수 없다는 뜻이다 — 호출은
아무것도 소비하지 않으므로 더 큰 buffer로 재시도해도 안전하다.
`InvalidState`는 stale handle이나 닫힌 수신·연결 상태를 다룬다. 같은
handler의 콜백 안에서 그 handler를 교체·해제하면 실제로 deadlock에
빠지는 대신 `Deadlock`을 반환한다.

---

## `ZlinkError`

모든 typed error가 (export되지 않은 중간 계층
`ResultError<TResult>`를 거쳐) 파생하는 public 기반.

```ts
try {
  dealer.send().message(part).submit();
} catch (err) {
  if (err instanceof SubmitError && err.result === SubmitResult.Backpressured) {
    // 정상 제어 흐름이지 실제 실패가 아니다
  }
}
```

**Options.** public 생성자 `ZlinkError(code: number, nativeErrno =
0)` — 하지만 application 코드는 이걸 직접 생성하지 않는다, 모든 내장
API가 대신 올바른 타입의 subclass를 던진다.

| Member | 의미 |
| --- | --- |
| `code` | `number`, 실패를 분류하는 zlink result code |
| `nativeErrno` | `number`, 밑에 깔린 native errno, 기본값 `0` |
| `name` | `string`, `ResultError`의 생성자가 구체적 error class 이름(예: `'SubmitError'`)으로 설정 |

**Completion result.** 해당 없음 — 이건 내장 `Error`를 확장하는 error
계층 자체다. 내부 `ResultError<TResult>` 계층은 export되지 않는다 —
application 코드는 `ZlinkError`나 구체적 subclass(`SubmitError` 등)를
잡거나 참조할 수 있지만, `ResultError`를 직접 import할 수 없다.

**선택 기준.** 구체적 typed error(`SubmitError` 등)를 잡아 typed
`.result`를 읽거나, error 타입 전체에 걸쳐 `.code`/`.nativeErrno`만
일반적으로 필요할 땐 공유 `ZlinkError`(또는 순수 `Error`)를 잡는다.
no-data와 일시적 back-pressure는 절대 일반 throw된 error로 보고되지
않는다 — 대신 Sockets/Messaging category의 `boolean`/`null` 반환
`recv`/`submit` 관례를 참고한다.

---

[`contracts/errors/`](../../../../bindings/node/src/zlink/contracts/errors/)와
[Node 바인딩 스펙](../../spec/node/README.ko.md)에서 전체 근거를 확인한다.
