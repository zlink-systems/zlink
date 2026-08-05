한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 이 레퍼런스에서 core의 result-code-family 표에 대응하는
부분이다 — 공유 `ZlinkError` interface와, 모든
submit/request/recv/handler/close/bind/connect/config 실패 API가
반환하는 8개 typed error struct를 다룬다. 정확한 signature는
[`internal/native/error.go`](../../../../bindings/go/internal/native/error.go)와
[`result_codes.go`](../../../../bindings/go/internal/native/result_codes.go)가
소유하며,
[`contracts/errors.go`](../../../../bindings/go/contracts/errors.go)를
통해 alias로 re-export된다.

---

## Typed error family

각 API 계열은 모든 field에 공유되는 하나의 error 타입 대신 typed
result code를 담은 자신만의 typed error struct를 가진다 — caller는
`errors.As`로 특정 struct(또는 공유 `ZlinkError` interface)에
type-assert한다. 8개 전부 동일한 형태를 공유한다: `Result`
field(자신만의 명명된 `int` result 타입), export되지 않은
`nativeErrno int`, 그리고 4개 메서드 — `Error() string`(native
errno가 있으면 `"<kind> error (<code>): <native strerror text>"`,
없으면 `"<kind> error (<code>)"` 형태로 포맷), `Code() int`,
`InternalErrno() int`, `Unwrap() error`(native errno를
`syscall.Errno`로 변환, 표준 라이브러리 `errors.Is`/`errors.As` 연동
지점 — native errno가 캡처되지 않았으면 `nil` 반환).

| Error struct | Result 타입 | 반환하는 곳 | 값 |
|---|---|---|---|
| `SubmitError` | `SubmitResult`(Messaging category) | send/publish/request-submit API | `SubmitOK`(0), `SubmitBackpressured`(1, 일반적인 제어 흐름), `SubmitNotConnected`(2), `SubmitNotFound`(3), `SubmitTerminated`(4), `SubmitInvalidHandle`(5), `SubmitInvalidArgument`(6), `SubmitNotSupported`(7), `SubmitInvalidState`(8), `SubmitThreadViolation`(9), `SubmitOutOfMemory`(10), `SubmitSeqExhausted`(11), `SubmitInternalError`(12), `SubmitNotAdmitted`(13, 일반적인 제어 흐름) — 전체 13-value 집합 |
| `RequestError` | `RequestResult` | request/reply completion(`RequestReplyCallback`/`RequestReplyCompletion`으로 전달) | `RequestOK`(0), `RequestTimedOut`(101), `RequestNotFound`(102), `RequestTerminated`(103), `RequestProtocolError`(104), `RequestInternalError`(105), `RequestRejected`(106), `RequestConflict`(107), `RequestBusy`(108), `RequestNotConnected`(109), `RequestInvalidArgument`(110), `RequestInvalidState`(111), `RequestNotSupported`(112), `RequestBackpressured`(113) — **이 binding은 `Backpressured`를 정의한다**, 대응물이 없는 rust의 `RequestResult`와 다르다 |
| `RecvError` | `RecvResult` | recv 계열 API | `RecvOK`(0), `RecvNoData`(201), `RecvBusy`(202), `RecvTerminated`(203), `RecvInvalidHandle`(204), `RecvNotSupported`(205), `RecvInternalError`(206), `RecvBufferTooSmall`(207), `RecvInvalidState`(208) — 전체 8-value 집합, node와 일치하며 dotnet/cpp/java/rust/python이 공유하는 6-value 집합과 다르다 |
| `HandlerError` | `HandlerResult` | handler 등록 API(`OnEvent`/`OnFire`/`OnPacket`/`OnCompletionControl` 등) | `HandlerOK`(0), `HandlerInvalidArgument`(301), `HandlerBusy`(302), `HandlerNotSupported`(303), `HandlerDeadlock`(304), `HandlerInvalidHandle`(305), `HandlerInternalError`(306) |
| `CloseError` | `CloseResult` | `Close()` 경로, `Context.Shutdown()` | `CloseOK`(0), `CloseBusy`(401), `CloseShutdown`(402), `CloseInvalidHandle`(403), `CloseInternalError`(404) |
| `BindError` | `BindResult` | `Bind(...)` | `BindOK`(0), `BindInvalidArgument`(501), `BindAddrInUse`(502), `BindNotSupported`(503), `BindInvalidHandle`(504), `BindInternalError`(505) |
| `ConnectError` | `ConnectResult` | `Connect`/`Unbind`/`Disconnect`/`DisconnectRID` | `ConnectOK`(0), `ConnectInvalidArgument`(601), `ConnectNotSupported`(602), `ConnectInvalidHandle`(603), `ConnectInternalError`(604), `ConnectNotFound`(605), `ConnectConflict`(606), `ConnectBusy`(607), `ConnectAuthFailed`(608) — `AuthFailed`를 포함한 전체 8-value 집합, node와 일치하며 dotnet/cpp/java/rust/python이 공유하는 7-value 집합과 다르다 |
| `ConfigError` | `ConfigResult` | 모든 socket/context option getter/setter | `ConfigOK`(0), `ConfigInvalidHandle`(701), `ConfigInvalidArgument`(702), `ConfigNotSupported`(703), `ConfigInternalError`(704), `ConfigInvalidState`(705), `ConfigNotFound`(706), `ConfigConflict`(707), `ConfigBufferTooSmall`(708), `ConfigBusy`(709) — 전체 9-value 집합, dotnet/node와 일치하며 cpp/java가 공유하는 6-value 집합과 다르다 |

**여기서도 재확인하는 언어 간 비대칭.** 모든 wrapper binding의
result 집합은 core의 `zlink_*_result_t` 계열(core의 Errors
category에 문서화)을 정확히 미러링해야 하지만, 서로 일치하지 않는다.
이 binding의 `RecvResult`와 `ConnectResult`는 이전엔 node만 가진
것으로 문서화됐던 더 완전한 값 집합을 갖는다 — **go도 node에
합류한다**, `RecvResult`의 `BufferTooSmall`/`InvalidState`와
`ConnectResult`의 `AuthFailed`를 정의하는 점에서 — 그러니 node가 그
값들을 가진 "유일한" binding이라는 이전의 주장은 이제 낡았으며
"node와 go"로 읽어야 한다. 이 binding의 `RequestResult`는
완전하다(`Backpressured` 있음), 그게 없는 rust와 다르다. 값이 부족한
언어가 이를 얻어야 하는지, 아니면 더 완전한 쪽이 축소돼야 하는지는
이 레퍼런스 범위 밖의 spec 레벨 질문이다 — 이 항목은 그게 묻히지
않도록 사실을 적어둔다.

**Completion result.** 모든 error는 이 8개 concrete pointer 타입 중
하나를 담은 평범한 Go `error` interface 값으로 반환된다; caller는
`errors.As(err, &target)`로 typed 값을 복원한다.

**선택 기준.** 호출하는 코드가 result code로 분기해야 할 땐 특정
error family의 `.Result`를 매치한다; POSIX errno 값을 이미 일반적으로
다루는 코드와 통합할 땐 `Unwrap()`으로 노출되는 `syscall.Errno`에
`errors.Is`/`errors.As`를 쓴다.

---

## `ZlinkError`

8개 typed error struct 전부가 구현하는 공유 error interface — 어떤
구체적 struct인지 몰라도 zlink에서 발생한 error를 균일하게 다루려는
코드를 위한 것.

```go
var zerr contracts.ZlinkError
if errors.As(err, &zerr) {
    log.Printf("zlink error %d (errno %d): %v", zerr.Code(), zerr.InternalErrno(), zerr)
}
```

**Options.** `ZlinkError`는 Go `interface`다(struct나 enum이 아니라).
**`Unwrap() error`는 `ZlinkError` interface 자체의 일부가 아니다** —
각 concrete struct가 이를 구현하지만, `ZlinkError` 타입 값만 가진
caller는 그 interface를 통해 직접 `Unwrap()`을 호출할 수 없다;
`errors.Is`/`errors.As`는 표준 라이브러리 자신의 관례에 따라 밑바탕
concrete 값에 대한 reflection을 통해 여전히 그것을 찾아낸다.

| Member | 의미 |
| --- | --- |
| `error` | 표준 error interface를 embed, 즉 `Error() string`을 요구 |
| `Code() int` | 실패를 분류하는 zlink result code |
| `InternalErrno() int` | 밑에 깔린 native errno |

**Completion result.** N/A — 순수 interface 타입, 직접 생성되는 일은
없다.

**선택 기준.** 어떤 8개 concrete error struct가 만들었는지 신경 쓰지
않고 `.Code()`/`.InternalErrno()`만 필요할 땐 `errors.As(err,
&zerr)`(`zerr`을 `contracts.ZlinkError`로 선언)를 쓴다. result
code의 의미가 family별로 다를 땐 대신 특정 struct 타입(예:
`*contracts.RecvError`)을 쓴다.

---

## `Strerror` 함수 없음

standalone errno-to-text 조회를 노출하는 다른 모든 언어와 달리,
**이 binding엔 공개 `Strerror`/`strerror` 함수가 전혀 없다**(Core
category에서 이미 언급했지만 이 category의 주제에 속하므로 다시
적는다). native `zlink_strerror` 호출은 각 typed error의
`Error()` 메서드 안에서만 내부적으로 쓰인다 — 모든 error는 caller가
독립적으로 호출할 수 있는 공유 export 조회를 거치는 대신 자신의
메시지를 직접 포맷한다.

**선택 기준.** 포맷된 메시지를 얻으려면 typed error에 `.Error()`를
호출한다(또는 `fmt`/`log`가 암묵적으로 그렇게 하도록 둔다); 이
binding에서 native strerror 텍스트에 닿는 다른 공개 경로는 없다.

---

[`internal/native/error.go`](../../../../bindings/go/internal/native/error.go),
[`result_codes.go`](../../../../bindings/go/internal/native/result_codes.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
