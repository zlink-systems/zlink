한국어 | [English](18-errors.en.md)

[레퍼런스 목차](README.ko.md)

# 18. Errors, results, and version

이 category는 framework의 error-kind 대응표에 해당하는 이 레퍼런스의 대응 문서다 — 다른
모든 category의 "Return과 errno" 섹션이 기대는 공유 실패 모델을 설명하고, version·error
introspection 진입점 세 개를 다룬다. 공개 함수는 주요 제어 흐름을 `zlink_*_result_t`
enum으로 반환하고 같은 스레드의 `zlink_errno()`에 더 상세한 원인을 기록한다 — caller는
result enum으로 분기하고 errno는 로깅과 세밀한 진단에 쓴다. 성공은 항상 숫자
0이며, 성공 후에는 errno가 정의되지 않는다. 정확한 signature는
[Errors 스펙](../spec/core/03-errors.ko.md)과 [errno map](../spec/core/04-errno-map.ko.md)이
소유한다.

---

## Result enum family

각 API family는 단일 공유 error kind가 아니라 자신만의 typed result enum을 반환한다. 이
표는 다른 모든 category의 항목이 되짚어보는 색인이다.

| Enum | 사용처 | 공통 값 |
|---|---|---|
| `zlink_submit_result_t` | send/publish/request-submit API(모든 socket-type category) | `OK`(0), `BACKPRESSURED`(1, 정상 제어 흐름), `NOT_CONNECTED`(2), `NOT_FOUND`(3), `NOT_ADMITTED`(13, 정상 제어 흐름 — target은 확인됐지만 admission 정책이 거부), `TERMINATED`(4), `INVALID_HANDLE`(5), `INVALID_ARGUMENT`(6), `NOT_SUPPORTED`(7), `INVALID_STATE`(8), `THREAD_VIOLATION`(9), `OUT_OF_MEMORY`(10), `SEQ_EXHAUSTED`(11), `INTERNAL_ERROR`(12) |
| `zlink_request_result_t` | `zlink_reply_handler_fn` 완료(DEALER/ROUTER category) | `OK`(0), `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `zlink_recv_result_t` | recv family API(Raw receive·Socket monitor·Timers category) | `OK`(0), `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206), `BUFFER_TOO_SMALL`(207), `INVALID_STATE`(208) |
| `zlink_handler_result_t` | handler 등록 API(Raw receive·Socket lifecycle·Socket monitor·Timers category) | `OK`(0), `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `zlink_close_result_t` | `zlink_ctx_term`/`zlink_close`/`zlink_ctx_shutdown`/`zlink_timer_destroy`/`zlink_monitor_close` | `OK`(0), `BUSY`(401), `SHUTDOWN`(402), `INVALID_HANDLE`(403), `INTERNAL_ERROR`(404) |
| `zlink_bind_result_t` | `zlink_bind`(Socket lifecycle category) | `OK`(0), `INVALID_ARGUMENT`(501), `ADDR_IN_USE`(502), `NOT_SUPPORTED`(503), `INVALID_HANDLE`(504), `INTERNAL_ERROR`(505) |
| `zlink_connect_result_t` | `zlink_connect`/`zlink_unbind`/`zlink_disconnect`/`zlink_disconnect_rid`(Socket lifecycle category) | `OK`(0), `INVALID_ARGUMENT`(601), `NOT_SUPPORTED`(602), `INVALID_HANDLE`(603), `INTERNAL_ERROR`(604), `NOT_FOUND`(605), `CONFLICT`(606), `BUSY`(607), `AUTH_FAILED`(608) |
| `zlink_config_result_t` | 모든 `zlink_set_*`/`zlink_get_*` 옵션 API, 기타 control-path 호출 | `OK`(0), `INVALID_HANDLE`(701), `INVALID_ARGUMENT`(702), `NOT_SUPPORTED`(703), `INTERNAL_ERROR`(704), `INVALID_STATE`(705), `NOT_FOUND`(706), `CONFLICT`(707), `BUFFER_TOO_SMALL`(708), `BUSY`(709) |

**각 값 family가 실제로 뜻하는 것**(함수별 정확한 `errno` 매핑은
[errno map](../spec/core/04-errno-map.ko.md) 참고): submit의 `BACKPRESSURED`/
`NOT_CONNECTED`/`NOT_FOUND`/`NOT_ADMITTED`는 예외적 실패가 아니라 정상적인 실행 흐름이다
— 모든 non-`OK` submit 결과를 똑같이 취급하는 caller는 "재시도가 합리적"과 "이대로
제출하면 절대 성공하지 않음"의 구분을 잃는다. Receive/config의 `BUFFER_TOO_SMALL`은
caller가 제공한 buffer가 첫 완결된 값을 담을 수 없다는(또는 SUB/XSUB의 경우 topic
buffer 용량이 너무 작다는) 뜻이다 — 호출은 아무것도 소비하지 않고 필요한 크기를
보고하므로 더 큰 buffer로 재시도해도 안전하다. `INVALID_STATE`는 stale handle이나 닫힌
수신·연결 상태를 다룬다. 같은 handler의 callback 안에서 그 handler를 해제·교체하면
실제로 deadlock에 빠지는 대신 `DEADLOCK`을 반환한다.

---

## `zlink_errno` / `zlink_strerror`

호출한 스레드의 상세 error code를 읽거나, error code를 사람이 읽을 수 있는 문자열로
변환한다.

```c
int code = zlink_errno();
const char *message = zlink_strerror(code);
```

**Parameters.** `zlink_errno`는 인자가 없다. `zlink_strerror`는 `errnum`(보통 이전
`zlink_errno()` 호출에서 온 error code)을 받는다.

**Return과 errno.** `zlink_errno`는 호출한 스레드의 현재 errno 값을 반환한다 — 실패한
operation이 반환된 직후, 같은 스레드의 다른 Core 호출 전에 부른다. `zlink_strerror`는
library 소유 static storage에 대한 pointer를 반환하며 caller가 해제하거나 수정하면
안 된다.

**선택 기준.** `zlink_*_result_t` 값이 이미 실패가 어느 result-enum 버킷에 속하는지
알려준 뒤, 진단·로깅을 위해 이 둘을 함께 쓴다 — `errno`는 그 위에 플랫폼·상세
맥락을 더할 뿐 대체 분류가 아니다. 특정 플랫폼에 없는 POSIX errno 값은
`ZLINK_HAUSNUMERO` 기반 공개 값을 쓴다 — `ESTALE`/`EALREADY`/`EDEADLK`/`ESHUTDOWN`은
각각 stale handle, 중복 operation, 재진입 callback, 닫힌 socket을 위해 지원하는 모든
플랫폼에서 사용 가능함이 보장된다.

---

## `zlink_version`

Library의 빌드 버전을 읽는다.

```c
int major, minor, patch;
zlink_version(&major, &minor, &patch);
```

**Parameters.** 세 개의 출력 `int *` pointer.

**Return과 errno.** 없음(`void`). `ZLINK_VERSION_MAJOR`/`_MINOR`/`_PATCH`와 결합된
`ZLINK_VERSION`/`ZLINK_MAKE_VERSION(major, minor, patch)` macro도 런타임 호출이
필요 없는 버전 확인을 위해 컴파일 시점에 쓸 수 있다. Core는
`ZLINK_VERSION_MAJOR`와 일치하는 SOVERSION을 쓴다.

**선택 기준.** Core를 빌드 시점에 링크하지 않고 동적으로 로드할 때 특히, 링크된
library 버전이 application이 빌드된 버전과 일치하는지 런타임에 확인할 때 쓴다.

---

전체 근거는 [Errors 스펙](../spec/core/03-errors.ko.md)과
[errno map](../spec/core/04-errno-map.ko.md)을 참고한다.
