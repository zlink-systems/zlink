---
title: "오류, 결과 enum과 버전"
---

[English](03-errors.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Message](02-message.ko.md) | [다음: Result Enums](04-errno-map.ko.md)
<!-- zlink-nav:end -->

# 오류, 결과 enum과 버전

> **이 장이 정의하는 것** — 공개 result enum, errno와 version 조회의 공개 계약. 자세한
> result-errno 대응표는 [Result와 errno 대응](04-errno-map.ko.md)이 다룬다.

이 문서는 ZLink Core의 오류 ABI 계약을 정의한다. 대상 독자는 C API와
bindings 개발자다. 이 문서는 “공개 함수의 typed result와 thread-local errno가 어떤 값으로 대응하며
빌드 버전을 어떻게 판별하는가?”에 답한다.

## 1. 기본 규칙

공개 함수는 주된 제어 흐름을 `zlink_*_result_t`로 반환하고 같은 thread의 `zlink_errno()`에 세부 원인을
기록한다. caller는 result enum으로 분기하고 errno는 log와 더 세밀한 진단에 사용한다. 성공은 항상 숫자
0이며 성공 뒤 errno 값은 지정하지 않는다.

```c
#define ZLINK_HAUSNUMERO 156384712

#define EFSM            (ZLINK_HAUSNUMERO + 51)
#define ENOCOMPATPROTO  (ZLINK_HAUSNUMERO + 52)
#define ETERM           (ZLINK_HAUSNUMERO + 53)
#define EMTHREAD        (ZLINK_HAUSNUMERO + 54)

#ifndef ESTALE
#define ESTALE          (ZLINK_HAUSNUMERO + 19)
#endif
#ifndef EALREADY
#define EALREADY        (ZLINK_HAUSNUMERO + 20)
#endif
#ifndef EDEADLK
#define EDEADLK         (ZLINK_HAUSNUMERO + 21)
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN       (ZLINK_HAUSNUMERO + 22)
#endif
```

platform에 없는 POSIX errno는 `ZLINK_HAUSNUMERO` 기반 공개 값으로 정의한다. stale handle,
중복 operation, reentrant callback과 종료한 socket을 표현하는 `ESTALE`, `EALREADY`, `EDEADLK`와
`ESHUTDOWN`은 모든 지원 platform에서 위 값을 사용할 수 있다. 정확한 함수별 대응은
[errno map](04-errno-map.ko.md)이 소유한다.

## 2. Submit result

```c
typedef enum zlink_submit_result_t {
  ZLINK_SUBMIT_OK               = 0,
  ZLINK_SUBMIT_BACKPRESSURED    = 1,
  ZLINK_SUBMIT_NOT_CONNECTED    = 2,
  ZLINK_SUBMIT_NOT_FOUND        = 3,
  ZLINK_SUBMIT_TERMINATED       = 4,
  ZLINK_SUBMIT_INVALID_HANDLE   = 5,
  ZLINK_SUBMIT_INVALID_ARGUMENT = 6,
  ZLINK_SUBMIT_NOT_SUPPORTED    = 7,
  ZLINK_SUBMIT_INVALID_STATE    = 8,
  ZLINK_SUBMIT_THREAD_VIOLATION = 9,
  ZLINK_SUBMIT_OUT_OF_MEMORY    = 10,
  ZLINK_SUBMIT_SEQ_EXHAUSTED    = 11,
  ZLINK_SUBMIT_INTERNAL_ERROR   = 12,
  ZLINK_SUBMIT_NOT_ADMITTED     = 13
} zlink_submit_result_t;
```

`BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND`와 `NOT_ADMITTED`는 정상적인 runtime 제어 흐름이다.
`NOT_ADMITTED`는 target route는 식별했지만 raw socket의 현재 admission 상태가 신규 submit을
거부했다는 뜻이다. Handshake가 완료되지 않은 peer가 여기에 포함된다. 입력 message
ownership은 submit owner 문서가 따로 정하며 result 값만으로 추측하지 않는다.

## 3. Request completion result

```c
typedef enum zlink_request_result_t {
  ZLINK_REQUEST_OK               = 0,
  ZLINK_REQUEST_TIMED_OUT        = 101,
  ZLINK_REQUEST_NOT_FOUND        = 102,
  ZLINK_REQUEST_TERMINATED       = 103,
  ZLINK_REQUEST_PROTOCOL_ERROR   = 104,
  ZLINK_REQUEST_INTERNAL_ERROR   = 105,
  ZLINK_REQUEST_REJECTED         = 106,
  ZLINK_REQUEST_CONFLICT         = 107,
  ZLINK_REQUEST_BUSY             = 108,
  ZLINK_REQUEST_NOT_CONNECTED    = 109,
  ZLINK_REQUEST_INVALID_ARGUMENT = 110,
  ZLINK_REQUEST_INVALID_STATE    = 111,
  ZLINK_REQUEST_NOT_SUPPORTED    = 112,
  ZLINK_REQUEST_BACKPRESSURED    = 113
} zlink_request_result_t;
```

이 enum은 raw socket request operation의 terminal completion에 사용한다. timeout 결과는
`ZLINK_REQUEST_TIMED_OUT`으로 표현한다. `BACKPRESSURED`는 request가 outbound admission 전에
capacity 부족으로 실패했음을 뜻한다.

## 4. Receive와 handler result

```c
typedef enum zlink_recv_result_t {
  ZLINK_RECV_OK               = 0,
  ZLINK_RECV_NO_DATA          = 201,
  ZLINK_RECV_BUSY             = 202,
  ZLINK_RECV_TERMINATED       = 203,
  ZLINK_RECV_INVALID_HANDLE   = 204,
  ZLINK_RECV_NOT_SUPPORTED    = 205,
  ZLINK_RECV_INTERNAL_ERROR   = 206,
  ZLINK_RECV_BUFFER_TOO_SMALL = 207,
  ZLINK_RECV_INVALID_STATE    = 208
} zlink_recv_result_t;

typedef enum zlink_handler_result_t {
  ZLINK_HANDLER_OK               = 0,
  ZLINK_HANDLER_INVALID_ARGUMENT = 301,
  ZLINK_HANDLER_BUSY             = 302,
  ZLINK_HANDLER_NOT_SUPPORTED    = 303,
  ZLINK_HANDLER_DEADLOCK         = 304,
  ZLINK_HANDLER_INVALID_HANDLE   = 305,
  ZLINK_HANDLER_INTERNAL_ERROR   = 306
} zlink_handler_result_t;
```

`BUFFER_TOO_SMALL`은 caller가 제공한 batch가 첫 complete message를 담지 못하거나 raw SUB/XSUB의
topic buffer capacity가 0 또는 필요한 길이보다 작은 경우다. raw subscription receive는 필요한 topic
길이를 반환하고 queue의 topic과 payload를 소비하지 않으므로 caller가 충분한 buffer로 재시도할 수 있다.
`INVALID_STATE`는 stale handle 또는 종료한 receive state에 사용한다. Handler unregister나
replace를 같은 callback에서 호출하면 `DEADLOCK`이다.

## 5. Close, bind와 connect result

```c
typedef enum zlink_close_result_t {
  ZLINK_CLOSE_OK             = 0,
  ZLINK_CLOSE_BUSY           = 401,
  ZLINK_CLOSE_SHUTDOWN       = 402,
  ZLINK_CLOSE_INVALID_HANDLE = 403,
  ZLINK_CLOSE_INTERNAL_ERROR = 404
} zlink_close_result_t;

typedef enum zlink_bind_result_t {
  ZLINK_BIND_OK               = 0,
  ZLINK_BIND_INVALID_ARGUMENT = 501,
  ZLINK_BIND_ADDR_IN_USE      = 502,
  ZLINK_BIND_NOT_SUPPORTED    = 503,
  ZLINK_BIND_INVALID_HANDLE   = 504,
  ZLINK_BIND_INTERNAL_ERROR   = 505
} zlink_bind_result_t;

typedef enum zlink_connect_result_t {
  ZLINK_CONNECT_OK               = 0,
  ZLINK_CONNECT_INVALID_ARGUMENT = 601,
  ZLINK_CONNECT_NOT_SUPPORTED    = 602,
  ZLINK_CONNECT_INVALID_HANDLE   = 603,
  ZLINK_CONNECT_INTERNAL_ERROR   = 604,
  ZLINK_CONNECT_NOT_FOUND        = 605,
  ZLINK_CONNECT_CONFLICT         = 606,
  ZLINK_CONNECT_BUSY             = 607,
  ZLINK_CONNECT_AUTH_FAILED      = 608
} zlink_connect_result_t;
```

Raw connect intent 또는 routing ID 충돌은 `CONFLICT`, transport peer authentication 불일치는
`AUTH_FAILED`다.

## 6. Configuration result

```c
typedef enum zlink_config_result_t {
  ZLINK_CONFIG_OK               = 0,
  ZLINK_CONFIG_INVALID_HANDLE   = 701,
  ZLINK_CONFIG_INVALID_ARGUMENT = 702,
  ZLINK_CONFIG_NOT_SUPPORTED    = 703,
  ZLINK_CONFIG_INTERNAL_ERROR   = 704,
  ZLINK_CONFIG_INVALID_STATE    = 705,
  ZLINK_CONFIG_NOT_FOUND        = 706,
  ZLINK_CONFIG_CONFLICT         = 707,
  ZLINK_CONFIG_BUFFER_TOO_SMALL = 708,
  ZLINK_CONFIG_BUSY             = 709
} zlink_config_result_t;
```

`CONFLICT`는 중복 이름, 중복 binding과 process-local identity 충돌이다. `BUFFER_TOO_SMALL`은 query 또는
retain output capacity가 작으며 caller-owned output을 일부 기록하지 않았음을 뜻한다. `BUSY`는 같은 mutable
batch나 configuration object를 동시에 사용한 경우다.

## 7. Version

```c
#define ZLINK_VERSION_MAJOR 11
#define ZLINK_VERSION_MINOR 0
#define ZLINK_VERSION_PATCH 0

#define ZLINK_MAKE_VERSION(major, minor, patch) \
  ((major) * 10000 + (minor) * 100 + (patch))

#define ZLINK_VERSION \
  ZLINK_MAKE_VERSION(ZLINK_VERSION_MAJOR, ZLINK_VERSION_MINOR, ZLINK_VERSION_PATCH)

ZLINK_EXPORT int zlink_errno(void);
ZLINK_EXPORT const char *zlink_strerror(int errnum);
ZLINK_EXPORT void zlink_version(int *major, int *minor, int *patch);
```

Core는 SOVERSION 11을 사용한다. `zlink_strerror()`가 반환한
pointer는 library-owned static storage이며 해제하거나 수정하지 않는다. 세 함수는 thread-safe이고
`zlink_errno()`는 호출 thread의 값만 반환한다.
