한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 core의 result-enum-family 표에 대응하는 이 레퍼런스의 대응
문서다 — 공유 exception 기반과, 모든 submit/request/recv/handler/close/
bind/connect/config 실패 API(Sockets/Messaging/Eventing/Core category)가
던지는 7개 typed exception을 문서화한다. 정확한 signature는
[`contracts/errors/`](../../../../bindings/python/src/zlink/contracts/errors/)가
소유한다.

---

## Typed exception family

각 API family는 단일 공유 exception 타입이 아니라 typed result enum을
담은 자신만의 typed exception을 던진다 — caller는 구체적 타입(또는
공유 `ZlinkError` 기반)을 잡아 `.result`를 읽는다. 8개 전부
concrete class다(이 binding의 다른 대부분의 contract와 달리
`Protocol`이 아님) — 내부 `_TypedZlinkError` 기반에서 파생하며, 그
자신은 `ZlinkError(RuntimeError)`에서 파생한다.

| Exception | Result enum | 던지는 곳 | 값 |
|---|---|---|---|
| `SubmitError` | `SubmitResult`(Sockets category) | send/publish/request-submit API | `BACKPRESSURED`(1, 정상 제어 흐름), `NOT_CONNECTED`(2), `NOT_FOUND`(3), `TERMINATED`(4), `INVALID_HANDLE`(5), `INVALID_ARGUMENT`(6), `NOT_SUPPORTED`(7), `INVALID_STATE`(8), `THREAD_VIOLATION`(9), `OUT_OF_MEMORY`(10), `SEQ_EXHAUSTED`(11), `INTERNAL_ERROR`(12), `NOT_ADMITTED`(13, 정상 제어 흐름) |
| `RequestError` | `RequestResult`(Sockets category) | request/reply 완료 | `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `RecvError` | `RecvResult`(Sockets category) | recv-family API | `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206), `BUFFER_TOO_SMALL`(207), `INVALID_STATE`(208) — 더 완전한 8개 값 집합(node와 일치) |
| `HandlerError` | `HandlerResult`(Sockets category) | handler 등록 API | `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `CloseError` | `CloseResult` | `close()` 경로, `Context.shutdown()` | `BUSY`(401), `SHUTDOWN`(402), `INVALID_HANDLE`(403), `INTERNAL_ERROR`(404) |
| `BindError` | `BindResult` | `Socket.bind(...)` | `INVALID_ARGUMENT`(501), `ADDR_IN_USE`(502), `NOT_SUPPORTED`(503), `INVALID_HANDLE`(504), `INTERNAL_ERROR`(505) |
| `ConnectError` | `ConnectResult` | `connect`/`disconnect`/`disconnect_rid` | `INVALID_ARGUMENT`(601), `NOT_SUPPORTED`(602), `INVALID_HANDLE`(603), `INTERNAL_ERROR`(604), `NOT_FOUND`(605), `CONFLICT`(606), `BUSY`(607), `AUTH_FAILED`(608) — 더 완전한 8개 값 집합(node와 일치) |
| `ConfigError` | `ConfigResult` | 모든 socket/context option getter/setter | `INVALID_HANDLE`(701), `INVALID_ARGUMENT`(702), `NOT_SUPPORTED`(703), `INTERNAL_ERROR`(704), `INVALID_STATE`(705), `NOT_FOUND`(706), `CONFLICT`(707), `BUFFER_TOO_SMALL`(708), `BUSY`(709) — 9개 값 전체 집합(dotnet/node와 일치) |

**언어간 비대칭, 여기서 다시 명시.** 이 binding의
`RecvResult`/`ConnectResult`/`ConfigResult`는 dotnet/cpp/java/rust가
이 family 중 하나 이상에서 쓰는 더 작은 집합이 아니라 node의 더
완전한 값 집합과 일치한다 — 다른 모든 언어의 Errors category에 이미
문서화된 것과 같은 비대칭이다. 더 작은 집합이 빠진 값을 가져야
하는지, 이 binding의 더 완전한 집합을 줄여야 하는지는 스펙 차원의
질문이며 이 레퍼런스의 범위 밖이다.

**각 값 family가 실제로 뜻하는 것.** `SubmitResult`의
`BACKPRESSURED`/`NOT_CONNECTED`/`NOT_FOUND`/`NOT_ADMITTED`는 예외적
실패가 아니라 정상적인 실행 흐름이다 — non-`OK` submit 결과를 전부
같게 취급하는 코드는 "재시도가 합리적"과 "이대로 제출하면 절대
성공하지 않음"의 구분을 잃는다. `BUFFER_TOO_SMALL`은 caller가
제공한 output 용량이 첫 완결된 값을 담을 수 없다는 뜻이다 — 호출은
아무것도 소비하지 않으므로 더 큰 buffer로 재시도해도 안전하다.
`INVALID_STATE`는 stale handle이나 닫힌 수신·연결 상태를 다룬다.
같은 handler의 콜백 안에서 그 handler를 교체·해제하면 실제로
deadlock에 빠지는 대신 `DEADLOCK`을 반환한다.

---

## `ZlinkError`

모든 typed exception이 (내부 `_TypedZlinkError` 중간 계층을 거쳐)
파생하는 public 기반.

```python
try:
    dealer.send().message(part).submit()
except SubmitError as ex:
    if ex.result == SubmitResult.BACKPRESSURED:
        pass  # 정상 제어 흐름이지 실제 실패가 아니다
```

**Options.**

| Member | 의미 |
| --- | --- |
| `ZlinkError(code: int, native_errno: int = 0)` | 순수 public 생성자다(기반이 protected/abstract이고 subclass를 통해서만 도달 가능한 다른 언어와 다름) |
| `code` | property, 실패를 분류하는 zlink result code |
| `native_errno` | property, 밑에 깔린 native errno, 없으면 `0` |
| `_TypedZlinkError.result` | property, typed result enum; 모든 typed exception이 실제로 파생하는 non-public 중간 계층에 선언됨 — 이 `__init__`은 **raw code가 알려진 enum member와 맞지 않을 때 `ValueError`를 잡아서 예외를 던지거나 무관한 값으로 조용히 매핑하는 대신 raw 정수를 그대로 보존한다**, 이는 특히 더 새로운 Core가 이 binding이 아직 모르는 result를 보고할 때를 견디기 위함이다 |

**Completion result.** 해당 없음 — 이건 내장 `RuntimeError`를
확장하는 exception 계층 자체다.

**선택 기준.** 구체적 typed exception(`SubmitError` 등)을 잡아 typed
`.result`를 읽거나, exception 타입 전체에 걸쳐 `.code`/
`.native_errno`만 일반적으로 필요할 땐 공유 `ZlinkError`를 잡는다.
no-data와 일시적 back-pressure는 절대 일반 exception으로 보고되지
않는다 — 대신 Sockets/Messaging category의 `bool`/`None` 반환
`recv_into`/`submit` 관례를 참고한다. 더 새로운 Core와 통신할 때
`.result`가 enum 대신 raw `int`를 가질 수 있으므로, `isinstance
(ex.result, SubmitResult)`가 항상 성립한다고 가정하는 대신 enum
member와 `==`로 비교한다.

---

[`contracts/errors/`](../../../../bindings/python/src/zlink/contracts/errors/)와
[Python 바인딩 스펙](../../spec/python/README.ko.md)에서 전체 근거를 확인한다.
