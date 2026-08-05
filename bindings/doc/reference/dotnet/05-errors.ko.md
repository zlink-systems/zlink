한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 core의 result-enum-family 표에 대응하는 이 레퍼런스의 대응 문서다 —
공유 exception 기반과, 모든 submit/request/recv/handler/close/bind/connect/config
실패 API(Sockets/Messaging/Eventing/Core category)가 던지는 7개 typed exception을
문서화한다. 정확한 signature는
[`Contracts/Errors/`](../../../../bindings/dotnet/src/Zlink/Contracts/Errors/)가
소유한다.

---

## Typed exception family

각 API family는 단일 공유 exception 타입이 아니라 nested `ErrorCode` enum을 담은
자신만의 typed exception을 던진다 — caller는 구체적 타입(또는 공유
`ZlinkException` 기반)을 잡아 `.Result`로 분기한다.

| Exception | 던지는 곳 | `ErrorCode` 값 |
|---|---|---|
| `ZlinkSubmitException` | send/publish/request-submit API(모든 socket-type category) | `Backpressured`(1, 정상 제어 흐름), `NotConnected`(2), `NotFound`(3), `Terminated`(4), `InvalidHandle`(5), `InvalidArgument`(6), `NotSupported`(7), `InvalidState`(8), `ThreadViolation`(9), `OutOfMemory`(10), `SeqExhausted`(11), `InternalError`(12), `NotAdmitted`(13, 정상 제어 흐름 — target은 도달 가능했지만 admission 정책이 거부) |
| `ZlinkRequestException` | request/reply 완료 | `TimedOut`(101), `NotFound`(102), `Terminated`(103), `ProtocolError`(104), `InternalError`(105), `Rejected`(106), `Conflict`(107), `Busy`(108), `NotConnected`(109), `InvalidArgument`(110), `InvalidState`(111), `NotSupported`(112) |
| `ZlinkRecvException` | recv-family API(Sockets/Eventing category) | `NoData`(201), `Busy`(202), `Terminated`(203), `InvalidHandle`(204), `NotSupported`(205), `InternalError`(206) |
| `ZlinkHandlerException` | handler 등록 API(Sockets/Eventing category) | `InvalidArgument`(301), `Busy`(302), `NotSupported`(303), `Deadlock`(304, handler를 자신의 콜백 안에서 교체·해제), `InvalidHandle`(305), `InternalError`(306) |
| `ZlinkCloseException` | `Close()`/`Dispose` 경로(Sockets/Eventing category), `IContext.Shutdown()`(Core category) | `Busy`(401), `Shutdown`(402), `InvalidHandle`(403), `InternalError`(404) |
| `ZlinkBindException` | `ISocket.Bind(...)`(Sockets category) | `InvalidArgument`(501), `AddrInUse`(502), `NotSupported`(503), `InvalidHandle`(504), `InternalError`(505) |
| `ZlinkConnectException` | `IConnectableSocket.Connect`/`Unbind`/`Disconnect`/`DisconnectRid`(Sockets category) | `InvalidArgument`(601), `NotSupported`(602), `InvalidHandle`(603), `InternalError`(604), `NotFound`(605), `Conflict`(606), `Busy`(607) |
| `ZlinkConfigException` | 모든 socket/context option getter/setter(Sockets/Core category) | `InvalidHandle`(701), `InvalidArgument`(702), `NotSupported`(703), `InternalError`(704), `InvalidState`(705), `NotFound`(706), `Conflict`(707), `BufferTooSmall`(708), `Busy`(709) |

**각 값 family가 실제로 뜻하는 것.** `ZlinkSubmitException`의 `Backpressured`/
`NotConnected`/`NotFound`/`NotAdmitted`는 예외적 실패가 아니라 정상적인 실행
흐름이다 — non-`Ok` submit 결과를 전부 같게 취급하는 caller는 "재시도가 합리적"과
"이대로 제출하면 절대 성공하지 않음"의 구분을 잃는다. `ZlinkConfigException`의
`BufferTooSmall`은 caller가 제공한 output 용량이 첫 완결된 값을 담을 수 없다는
뜻이다 — 호출은 아무것도 소비하지 않으므로 더 큰 buffer로 재시도해도 안전하다.
`InvalidState`는 stale handle이나 닫힌 수신·연결 상태를 다룬다. 같은 handler의
콜백 안에서 그 handler를 교체·해제하면 실제로 deadlock에 빠지는 대신
`Deadlock`을 반환한다.

---

## `ZlinkException`

위 모든 typed exception이 상속하는 abstract 기반.

```csharp
try
{
    dealer.Send().Message(Message.From("payload")).Submit();
}
catch (ZlinkSubmitException ex) when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
{
    // 정상 제어 흐름이지 실제 실패가 아니다
}
```

**Options.**

| Member | 의미 |
| --- | --- |
| `ZlinkException(int code)` / `ZlinkException(int code, int nativeErrno)` | protected 생성자만 존재 — public 진입점은 각 typed exception 자신의 `ErrorCode`를 받는 생성자다(`Ok`는 절대 안 됨, 아래 참고) |
| `Code` | `int`, 실패를 분류하는 zlink result code |
| `NativeErrno` | `int`, 밑에 깔린 native errno, 없으면 `0` |

**Completion result.** 해당 없음 — 이건 exception 계층 자체다. 모든 typed
exception의 public 생성자는 `ValidatePublicErrorCode<TErrorCode>`를 통해 성공값
`Ok`를 거부하며, caller가 `Ok`로 생성하려 하면 `ArgumentOutOfRangeException`을
던진다. native errno도 함께 받는 생성자 overload는 internal runtime 변환용일
뿐 public 표면이 아니다.

**선택 기준.** 구체적 typed exception(`ZlinkSubmitException` 등)을 잡아
`ErrorCode` 타입의 `.Result`로 분기하거나, exception 타입 전체에 걸쳐 `.Code`/
`.NativeErrno`만 일반적으로 필요할 땐 공유 `ZlinkException` 기반을 잡는다.
no-data와 일시적 back-pressure는 절대 일반 exception으로 보고되지 않는다 —
대신 Sockets/Messaging category의 `bool` 반환 `Recv`/`Submit` 관례를 참고한다.

---

## `SubmitResult`

`ZlinkSubmitException.ErrorCode`와 같은 값을 갖는 public enum으로, native result
code가 typed exception이나 `bool` 반환으로 바뀌기 전 내부적으로 매핑하는 데
쓰인다.

**Options.** `ZlinkSubmitException.ErrorCode`(위)와 같은 값 집합.

**Completion result.** 해당 없음 — `Contracts/`의 어떤 public API도 `SubmitResult`를
직접 반환·수신하지 않는다. 모든 public submit 표면(Messaging/Sockets category)은
실패를 이 enum이 아니라 `bool` 또는 `ZlinkSubmitException`으로 보고한다.

**선택 기준.** 오늘 시점 application 코드에서는 해당 사항 없음 — public 타입으로
존재하지만 어떤 public contract member로도 도달하지 않는다. 이걸
`ZlinkSubmitException.ErrorCode`로 병합해야 할지 아니면 실제로 도달 가능하게
만들어야 할지는 스펙 차원의 질문이며 이 레퍼런스의 범위 밖이다.

---

[`Contracts/Errors/`](../../../../bindings/dotnet/src/Zlink/Contracts/Errors/)와
[.NET 바인딩 스펙](../../spec/dotnet/README.ko.md)에서 전체 근거를 확인한다.
