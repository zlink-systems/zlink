한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 core의 result-enum-family 표에 대응하는 이 레퍼런스의 대응
문서다 — 공유 exception 계층과, 모든 submit/request/recv/handler/close/
bind/connect/config 실패 API(Sockets/Messaging/Eventing/Core category)가
던지는 7개 typed exception을 문서화한다. 정확한 signature는
[`contracts/errors/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/errors/)가
소유한다.

---

## Typed exception family

각 API family는 단일 공유 exception 타입이 아니라 typed result enum을 담은
자신만의 typed exception을 던진다 — caller는 구체적 타입(또는 공유
`ZlinkException` 기반)을 잡아 `.getResult()`를 호출한다. 계층은 Java
`sealed` class다: `ZlinkException`(public, `abstract sealed`, `permits
TypedZlinkException`) → `TypedZlinkException`(**package-private**,
`abstract sealed`, application 코드에서 이름으로 직접 참조 불가) → 아래 각
concrete exception(`public final`).

| Exception | Result enum | 던지는 곳 | 값 |
|---|---|---|---|
| `ZlinkSubmitException` | `SubmitResult`(Sockets category) | send/publish/request-submit API | `BACKPRESSURED`(1, 정상 제어 흐름), `NOT_CONNECTED`(2), `NOT_FOUND`(3), `TERMINATED`(4), `INVALID_HANDLE`(5), `INVALID_ARGUMENT`(6), `NOT_SUPPORTED`(7), `INVALID_STATE`(8), `THREAD_VIOLATION`(9), `OUT_OF_MEMORY`(10), `SEQ_EXHAUSTED`(11), `INTERNAL_ERROR`(12), `NOT_ADMITTED`(13, 정상 제어 흐름) |
| `ZlinkRequestException` | `RequestResult`(Sockets category) | request/reply 완료 | `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `ZlinkRecvException` | `RecvResult`(Sockets category) | recv-family API | `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206) |
| `ZlinkHandlerException` | `HandlerResult` | handler 등록 API | `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `ZlinkCloseException` | `CloseResult` | `close()` 경로, `Context.shutdown()` | `BUSY`(401), `SHUTDOWN`(402), `INVALID_HANDLE`(403), `INTERNAL_ERROR`(404) |
| `ZlinkBindException` | `BindResult` | `Socket.bind(...)` | `INVALID_ARGUMENT`(501), `ADDR_IN_USE`(502), `NOT_SUPPORTED`(503), `INVALID_HANDLE`(504), `INTERNAL_ERROR`(505) |
| `ZlinkConnectException` | `ConnectResult` | `connect`/`unbind`/`disconnect`/`disconnectRid` | `INVALID_ARGUMENT`(601), `NOT_SUPPORTED`(602), `INVALID_HANDLE`(603), `INTERNAL_ERROR`(604), `NOT_FOUND`(605), `CONFLICT`(606), `BUSY`(607) |
| `ZlinkConfigException` | `ConfigResult` | 모든 socket/context option getter/setter | `INVALID_HANDLE`(701), `INVALID_ARGUMENT`(702), `NOT_SUPPORTED`(703), `INTERNAL_ERROR`(704), `INVALID_STATE`(705), `NOT_FOUND`(706) |

**언어간 비대칭.** 이 binding의 `ConfigResult`는 값이 6개뿐이며
`NOT_FOUND`(706)에서 멈춘다 — cpp의 `config_result_t`와 일치하지만,
dotnet의 `ZlinkConfigException.ErrorCode`는 추가로 `Conflict`(707),
`BufferTooSmall`(708), `Busy`(709)를 정의한다. 이 binding의 `ConfigResult`가
이 세 값을 가져야 하는지는 스펙 차원의 질문이며 이 레퍼런스의 범위 밖이다.

**각 값 family가 실제로 뜻하는 것.** `SubmitResult`의 `BACKPRESSURED`/
`NOT_CONNECTED`/`NOT_FOUND`/`NOT_ADMITTED`는 예외적 실패가 아니라 정상적인
실행 흐름이다 — non-`OK` submit 결과를 전부 같게 취급하는 caller는
"재시도가 합리적"과 "이대로 제출하면 절대 성공하지 않음"의 구분을 잃는다.
`INVALID_STATE`는 stale handle이나 닫힌 수신·연결 상태를 다룬다. 같은
handler의 콜백 안에서 그 handler를 교체·해제하면 실제로 deadlock에 빠지는
대신 `DEADLOCK`을 반환한다.

---

## `ZlinkException`

모든 typed exception이 (package-private 중간 계층 `TypedZlinkException`을
거쳐) 파생하는 public abstract 기반.

```java
try {
    dealer.send().message(part).submit();
} catch (ZlinkSubmitException ex) {
    if (ex.getResult() == SubmitResult.BACKPRESSURED) {
        // 정상 제어 흐름이지 실제 실패가 아니다
    }
}
```

**Options.** 생성자는 `protected`다 — 임의 typed exception을 만드는 public
진입점은 그 exception 자신의 result enum을 받는 public 생성자다
(`ZlinkSubmitException(SubmitResult)` 등), 또는 명시적 `nativeErrno`를
더한 같은 생성자다.

| Member | 의미 |
| --- | --- |
| `getCode()` | `int`, 실패를 분류하는 zlink result code |
| `getNativeErrno()` | `int`, 밑에 깔린 native errno, 없으면 `0` |
| `fromLastError(String operation)` / `fromLastError(ErrorCategory)` | static factory; 현재 native errno와 `ErrorCategory`(`CONFIG`/`BIND`/`CONNECT`/`CLOSE`/`HANDLER`/`RECV`/`REQUEST`/`SUBMIT`)로부터 올바른 타입의 exception을 만든다 — `String operation` overload는 operation 이름에서 category를 추론한다 |
| `fromErrno(String operation, int errno)` / `fromErrno(ErrorCategory, int errno)` | static factory; `fromLastError`와 같은 매핑이지만 현재 native errno를 읽는 대신 명시적 `errno`를 받는다 |

**Completion result.** 해당 없음 — 이건 exception 계층 자체다.
`TypedZlinkException`(중간 sealed class)은 package-private다 —
application 코드는 `ZlinkException`이나 구체적 subclass를 잡거나
참조할 수 있지만, `TypedZlinkException`을 이름으로 직접 참조할 수 없다.

**선택 기준.** 구체적 typed exception(`ZlinkSubmitException` 등)을 잡아
enum 타입의 `getResult()`를 호출하거나, exception 타입 전체에 걸쳐
`getCode()`/`getNativeErrno()`만 일반적으로 필요할 땐 공유
`ZlinkException` 기반을 잡는다. no-data와 일시적 back-pressure는 절대
일반 exception으로 보고되지 않는다 — 대신 Sockets/Messaging category의
`boolean` 반환 `recv`/`submit` 관례를 참고한다. raw errno로부터 올바른
타입의 exception을 만들어야 하는 custom native interop 경로를 구현할
때만 `ZlinkException.fromErrno(...)`/`fromLastError(...)`를 쓴다 — 일반
application 코드는 이걸 호출할 필요가 없다, 모든 내장 API가 이미 스스로
올바른 타입의 exception을 던지기 때문이다.

---

[`contracts/errors/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/errors/)와
[Java 바인딩 스펙](../../spec/java/README.ko.md)에서 전체 근거를 확인한다.
