한국어 | [English](05-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Errors

이 category는 core의 result-enum-family 표에 대응하는 이 레퍼런스의 대응 문서다 —
공유 exception 기반과, 모든 submit/request/recv/handler/close/bind/connect/config
실패 API(Sockets/Messaging/Eventing/Core category)가 던지는 7개 typed exception을
문서화한다. 정확한 signature는
[`Contracts/Errors/`](../../../../bindings/cpp/include/zlink/Contracts/Errors/)가
소유한다.

---

## Typed exception family

각 API family는 단일 공유 exception 타입이 아니라 자신만의 result enum을 담은
자신만의 typed exception을 던진다(`binding_error_t`에서 파생) — caller는
구체적 타입(또는 공유 `binding_error_t` 기반)을 잡아 `.result()`를 호출한다.

| Exception | Result enum | 던지는 곳 | 값 |
|---|---|---|---|
| `submit_error_t` | `submit_result_t`(Sockets category) | send/publish/request-submit API | `backpressured`(1, 정상 제어 흐름), `not_connected`(2), `not_found`(3), `terminated`(4), `invalid_handle`(5), `invalid_argument`(6), `not_supported`(7), `invalid_state`(8), `thread_violation`(9), `out_of_memory`(10), `seq_exhausted`(11), `internal_error`(12), `not_admitted`(13, 정상 제어 흐름) |
| `request_error_t` | `request_result_t`(Messaging category) | request/reply 완료 | `timed_out`(101), `not_found`(102), `terminated`(103), `protocol_error`(104), `internal_error`(105), `rejected`(106), `conflict`(107), `busy`(108), `not_connected`(109), `invalid_argument`(110), `invalid_state`(111), `not_supported`(112) |
| `recv_error_t` | `recv_result_t`(Sockets category) | recv-family API | `no_data`(201), `busy`(202), `terminated`(203), `invalid_handle`(204), `not_supported`(205), `internal_error`(206) |
| `handler_error_t` | `handler_result_t` | handler 등록 API | `invalid_argument`(301), `busy`(302), `not_supported`(303), `deadlock`(304), `invalid_handle`(305), `internal_error`(306) |
| `close_error_t` | `close_result_t` | `close()` 경로, `context_t::shutdown()` | `busy`(401), `shutdown`(402), `invalid_handle`(403), `internal_error`(404) |
| `bind_error_t` | `bind_result_t` | `socket_t::bind(...)` | `invalid_argument`(501), `addr_in_use`(502), `not_supported`(503), `invalid_handle`(504), `internal_error`(505) |
| `connect_error_t` | `connect_result_t` | `connect`/`unbind`/`disconnect`/`disconnect_rid` | `invalid_argument`(601), `not_supported`(602), `invalid_handle`(603), `internal_error`(604), `not_found`(605), `conflict`(606), `busy`(607) |
| `config_error_t` | `config_result_t` | 모든 socket/context option getter/setter | `invalid_handle`(701), `invalid_argument`(702), `not_supported`(703), `internal_error`(704), `invalid_state`(705), `not_found`(706) |

**언어간 비대칭.** 이 투영의 `config_result_t`는 값이 6개뿐이며
`not_found`(706)에서 멈춘다 — dotnet의 `ZlinkConfigException.ErrorCode`는
추가로 `Conflict`(707), `BufferTooSmall`(708), `Busy`(709)를 정의한다. 이
투영의 `config_result_t`가 이 세 값을 가져야 하는지는 스펙 차원의 질문이며 이
레퍼런스의 범위 밖이다 — 이 문서가 해결하는 게 아니다.

**각 값 family가 실제로 뜻하는 것.** `submit_error_t`의 `backpressured`/
`not_connected`/`not_found`/`not_admitted`는 예외적 실패가 아니라 정상적인 실행
흐름이다 — non-zero submit 결과를 전부 같게 취급하는 caller는 "재시도가
합리적"과 "이대로 제출하면 절대 성공하지 않음"의 구분을 잃는다.
`invalid_state`는 stale handle이나 닫힌 수신·연결 상태를 다룬다. 같은
handler의 콜백 안에서 그 handler를 교체·해제하면 실제로 deadlock에 빠지는
대신 `deadlock`을 반환한다.

---

## `binding_error_t`

위 모든 typed exception이 상속하는 abstract 기반(그 자신은
`std::runtime_error`에서 파생).

```cpp
try {
    std::move (dealer.send ()).message (part).submit ();
} catch (const zlink::submit_error_t &ex) {
    if (ex.result () == zlink::submit_result_t::backpressured) {
        // 정상 제어 흐름이지 실제 실패가 아니다
    }
}
```

**Options.**

| Member | 의미 |
| --- | --- |
| `binding_error_t(int code_, int internal_errno_)` | protected 생성자만 존재 — public 진입점은 각 typed exception 자신의 result enum을 받는 생성자다(예: `submit_error_t(submit_result_t)`), 또는 native result에서 변환할 때 내부적으로 쓰이는 명시적 `internal_errno_`를 더한 같은 생성자 |
| `code()` | `int`, 실패를 분류하는 zlink result code |
| `internal_errno()` | `int`, 밑에 깔린 native errno, 또는 1-인자 형태로 생성됐으면 `code()`와 같은 값 |
| `what()` | 재정의된 `std::runtime_error::what()`, 포맷된 메시지 텍스트 반환 |

**Completion result.** 해당 없음 — 이건 exception 계층 자체다. `error_t`(family에
속하지 않는 일반 exception)는 `error_t(int code_)` 또는 `error_t(int code_, int
internal_errno_)`로 raw code에서 직접 생성할 수 있다.

**선택 기준.** 구체적 typed exception(`submit_error_t` 등)을 잡아 enum 타입의
`.result()`를 호출하거나, exception 타입 전체에 걸쳐 `.code()`/`.what()`만
일반적으로 필요할 땐 공유 `binding_error_t` 기반(또는 순수 `std::exception`)을
잡는다. no-data와 일시적 back-pressure는 절대 일반 exception으로 보고되지
않는다 — 대신 Sockets/Messaging category의 `int`/`bool` 반환 `recv`/`submit`
관례를 참고한다.

---

[`Contracts/Errors/`](../../../../bindings/cpp/include/zlink/Contracts/Errors/)와
[C++ 바인딩 스펙](../../spec/cpp/README.ko.md)에서 전체 근거를 확인한다.
