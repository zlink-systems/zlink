한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 core의 result-enum-family 표에 대응하는 이 레퍼런스의 대응
문서다 — 공유 error enum과, 모든 submit/request/recv/handler/close/
bind/connect/config 실패 API가 `Result::Err`로 반환하는 7개 typed error
struct를 문서화한다. 정확한 signature는
[`contracts/errors/`](../../../../bindings/rust/src/contracts/errors/)가
소유한다.

---

## Typed error family

각 API family는 모든 필드에 대해 단일 공유 error 타입이 아니라 typed
result enum을 담은 자신만의 typed error struct를 갖는다 — caller는
구체적 struct의 `.code` 필드를 match하거나, 공유 `ZlinkError` enum으로
변환(또는 match)한다. 8개 전부 하나의 내부 macro(`define_error_type!`)로
생성되며, 각각 동일한 형태를 가진다: `code: TResult` 필드,
`native_errno: i32` 필드, `new(code, native_errno)`,
`code()`/`native_errno()` accessor, `Display`, `std::error::Error`,
`From<Self> for ZlinkError`.

| Error struct | Result enum | 반환하는 곳 | 값 |
|---|---|---|---|
| `SubmitError` | `SubmitResult`(Messaging category) | send/publish/request-submit API | `Backpressured`(1, 정상 제어 흐름), `NotConnected`(2), `NotFound`(3), `Terminated`(4), `InvalidHandle`(5), `InvalidArgument`(6), `NotSupported`(7), `InvalidState`(8), `ThreadViolation`(9), `OutOfMemory`(10), `SeqExhausted`(11), `InternalError`(12), `NotAdmitted`(13, 정상 제어 흐름) |
| `RequestError` | `RequestResult` | request/reply 완료(`RequestOp` 콜백에 전달) | `TimedOut`(101), `NotFound`(102), `Terminated`(103), `ProtocolError`(104), `InternalError`(105), `Rejected`(106), `Conflict`(107), `Busy`(108), `NotConnected`(109), `InvalidArgument`(110), `InvalidState`(111), `NotSupported`(112) — **여기엔 `Backpressured` variant가 없다**, 113에 이를 정의하는 다른 모든 언어의 `RequestResult`/대응물과 다르다 |
| `RecvError` | `RecvResult` | recv-family API | `NoData`(201), `Busy`(202), `Terminated`(203), `InvalidHandle`(204), `NotSupported`(205), `InternalError`(206) — 6개 값 집합(`BufferTooSmall`/`InvalidState` 없음, dotnet/cpp/java와 일치, node의 8개 값 집합과 다름) |
| `HandlerError` | `HandlerResult` | handler 등록 API | `InvalidArgument`(301), `Busy`(302), `NotSupported`(303), `Deadlock`(304), `InvalidHandle`(305), `InternalError`(306) |
| `CloseError` | `CloseResult` | `close()` 경로, `Context::shutdown()` | `Busy`(401), `Shutdown`(402), `InvalidHandle`(403), `InternalError`(404) |
| `BindError` | `BindResult` | `bind(...)` | `InvalidArgument`(501), `AddrInUse`(502), `NotSupported`(503), `InvalidHandle`(504), `InternalError`(505) |
| `ConnectError` | `ConnectResult` | `connect`/`unbind`/`disconnect`/`disconnect_rid` | `InvalidArgument`(601), `NotSupported`(602), `InvalidHandle`(603), `InternalError`(604), `NotFound`(605), `Conflict`(606), `Busy`(607) — 7개 값(`AuthFailed` 없음, dotnet/cpp/java와 일치, node의 8개 값 집합과 다름) |
| `ConfigError` | `ConfigResult` | 모든 socket/context option getter/setter | `InvalidHandle`(701), `InvalidArgument`(702), `NotSupported`(703), `InternalError`(704), `InvalidState`(705), `NotFound`(706) — 6개 값 집합(cpp/java와 일치, dotnet/node의 9개 값 집합과 다름) |

**언어간 비대칭, 여기서 다시 명시.** 모든 wrapper binding의 result
enum은 core의 `zlink_*_result_t` family(core Errors category에
문서화됨)를 정확히 반영해야 하지만, 서로 일치하지 않는다: 이 binding의
`RecvResult`/`ConnectResult`/`ConfigResult`는 (node의 더 완전한 집합이
아니라) *더 작은* 값 집합(cpp/java)과 일치한다. `RequestResult`엔
`Backpressured`가 아예 없는데, 지금까지 다룬 다른 모든 언어는 이를
정의한다. 이 binding이 빠진 값을 가져야 하는지, 아니면 다른 곳의 더
완전한 값을 줄여야 하는지는 스펙 차원의 질문이며 이 레퍼런스의 범위
밖이다 — 이 항목은 그 사실이 묻히지 않도록 명시해둔다.

**각 값 family가 실제로 뜻하는 것.** `SubmitResult`의
`Backpressured`/`NotConnected`/`NotFound`/`NotAdmitted`는 예외적 실패가
아니라 정상적인 실행 흐름이다 — non-`Ok` submit 결과를 전부 같게
취급하는 코드는 "재시도가 합리적"과 "이대로 제출하면 절대 성공하지
않음"의 구분을 잃는다. `InvalidState`는 stale handle이나 닫힌
수신·연결 상태를 다룬다. 같은 handler의 콜백 안에서 그 handler를
교체·해제하면 실제로 deadlock에 빠지는 대신 `Deadlock`을 반환한다.

---

## `ZlinkError`

Rust다운 공유 error 타입 — 상속 기반 class가 아니라, typed error
struct마다 variant 하나씩을 가진 `enum`.

```rust
match dealer.send().message(part)?.submit() {
    Ok(true) => { /* queued */ }
    Ok(false) => { /* SendFlags::DONT_WAIT이고 block됐을 상황 */ }
    Err(err) => {
        let zlink_err: ZlinkError = err.into();
        if zlink_err.code() == SubmitResult::Backpressured as i32 {
            // 정상 제어 흐름이지 실제 실패가 아니다
        }
    }
}
```

**Options.** 각 typed error struct가 `From<Self> for ZlinkError`를
구현하므로, `.into()`/`?`(error 타입이 `ZlinkError`일 때)가 자동으로
변환한다.

| Member | 의미 |
| --- | --- |
| `Submit(SubmitError)` / `Request(RequestError)` / `Recv(RecvError)` / `Handler(HandlerError)` / `Close(CloseError)` / `Bind(BindError)` / `Connect(ConnectError)` / `Config(ConfigError)` | typed error struct마다 variant 하나씩 |
| `code(&self) -> i32` | variant를 match해서 내부 `code` 필드를 `i32`로 캐스팅해 반환 |
| `native_errno(&self) -> i32` | variant를 match해서 내부 `native_errno` 필드를 반환 |

**Completion result.** 해당 없음 — `Display`와 `std::error::Error`를
구현하는 순수 sum-type wrapper.

**선택 기준.** 특정 result enum의 variant가 중요할 땐 주어진 API가
반환하는 구체적 typed error struct를 match한다. 함수 본문 전체에서 8개
서로 다른 struct 타입을 match하는 것보다 단일 error 타입이 더 편할 땐
`ZlinkError`로 변환한다(`.into()`, 또는 `Result<_, ZlinkError>`를
반환하는 함수에서 `?`가 대신 하게 둠). no-data와 일시적
back-pressure는 절대 error로 보고되지 않는다 — 대신 Sockets/Messaging
category의 `bool`/`Option` 반환 `recv`/`submit` 관례를 참고한다.

---

[`contracts/errors/`](../../../../bindings/rust/src/contracts/errors/)와
[Rust 바인딩 스펙](../../spec/rust/README.ko.md)에서 전체 근거를 확인한다.
