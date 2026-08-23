---
title: "오류, 결과 enum과 버전"
---

[English](https://zlink-systems.github.io/zlink/spec/core/03-errors/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Message](02-message.ko.md) | [다음: Events](04-events.ko.md)
<!-- zlink-nav:end -->

# 오류, 결과 enum과 버전

> **이 장이 정의하는 것** — 공개 result enum, errno와 version 조회의 공개 계약. 자세한
> result-errno 대응표는 이 문서의 [Result와 errno 대응](#result와-errno-대응) 절이 다룬다.

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
[Result와 errno 대응](#result와-errno-대응)이 소유한다.

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

### 6.1 Receive flow state

`zlink_socket_set_receive_flow_state()`의 결과는 다음과 같다. 함수 선언과 state enum은
[Socket 공통](socket/README.ko.md)이 소유하고, 결과로 나타나는 동작은
[DEALER](socket/06-dealer.ko.md)와 [ROUTER](socket/07-router.ko.md)가 소유한다.

| 조건 | 결과 | errno |
|---|---|---|
| DEALER 또는 ROUTER handle과 `zlink_receive_flow_state_t` 범위 안의 state. Socket이 이미 유지하는 state를 다시 설정한 경우를 포함한다 | `ZLINK_CONFIG_OK` | 정의하지 않음 |
| Handle이 NULL이거나 socket이 아니거나, close의 teardown이 이미 끝난 handle | `ZLINK_CONFIG_INVALID_HANDLE` | 정의하지 않음 |
| `zlink_receive_flow_state_t` 범위 밖의 state 값 | `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL` |
| Completion lane이 없는 socket 유형: PAIR, PUB, SUB, XPUB, XSUB, STREAM | `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` |
| 동시에 진행한 close가 이 호출보다 먼저 socket admission을 얻음 | `ZLINK_CONFIG_INVALID_STATE` | `ESHUTDOWN` |
| 소유 context가 종료 중 | `ZLINK_CONFIG_INTERNAL_ERROR` | `ETERM` |

현재 상태를 다시 설정하는 호출은 오류가 아니라 성공하는 no-op다. 이 state는 counter가 아니라
절대값이다.

동시에 진행하는 close와 이 호출은 같은 socket admission을 두고 경합하며, 먼저 수락된 쪽만
관측된다. 경합의 두 결과가 모두 정의되어 있다. `INVALID_STATE`는 handle이 아직 등록된 상태에서
close가 먼저 수락된 경우다. `INVALID_HANDLE`은 close의 teardown이 이미 끝나 handle이 더 이상
socket으로 확인되지 않는 경우다. 어느 쪽도 state를 일부만 적용하지 않는다.

## 7. Version

```c
#define ZLINK_VERSION_MAJOR 0
#define ZLINK_VERSION_MINOR 12
#define ZLINK_VERSION_PATCH 0

#define ZLINK_MAKE_VERSION(major, minor, patch) \
  ((major) * 10000 + (minor) * 100 + (patch))

#define ZLINK_VERSION \
  ZLINK_MAKE_VERSION(ZLINK_VERSION_MAJOR, ZLINK_VERSION_MINOR, ZLINK_VERSION_PATCH)

ZLINK_EXPORT int zlink_errno(void);
ZLINK_EXPORT const char *zlink_strerror(int errnum);
ZLINK_EXPORT void zlink_version(int *major, int *minor, int *patch);
```

Core는 SOVERSION 0을 사용한다. `zlink_strerror()`가 반환한
pointer는 library-owned static storage이며 해제하거나 수정하지 않는다. 세 함수는 thread-safe이고
`zlink_errno()`는 호출 thread의 값만 반환한다.

## Result와 errno 대응

이 절은 ZLink Core raw public API의 result enum과 thread-local errno 대응을 정의한다. Result는
제어 흐름의 기준이고 errno는 같은 실패를 더 세밀하게 설명한다.

### 1. 공통 우선순위

한 호출에서 여러 실패 조건이 겹치면 argument, handle·lifecycle, target·connection 조회, capacity,
transport·internal failure 순서로 하나를 반환한다. 성공한 함수의 errno는 정의하지 않는다.

### 2. Submit result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_SUBMIT_OK` | - | 함수가 정한 ownership 전이가 완료됨 |
| `ZLINK_SUBMIT_BACKPRESSURED` | `EAGAIN`, `ETIMEDOUT`, `ENOBUFS` | socket queue 또는 reservation capacity 부족 |
| `ZLINK_SUBMIT_NOT_CONNECTED` | `ENOTCONN` | target connection 없음 |
| `ZLINK_SUBMIT_NOT_FOUND` | `ENOENT` | raw target 없음 |
| `ZLINK_SUBMIT_NOT_ADMITTED` | `EACCES` | handshake 또는 raw routing admission 거부 |
| `ZLINK_SUBMIT_TERMINATED` | `ETERM`, `ESHUTDOWN` | Context 또는 socket lifecycle 종료 |
| `ZLINK_SUBMIT_INVALID_HANDLE` | `EFAULT` | handle이 `NULL`이거나 종류가 다름 |
| `ZLINK_SUBMIT_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | 잘못된 pointer, count, metadata 또는 flags |
| `ZLINK_SUBMIT_NOT_SUPPORTED` | `ENOTSUP` | handle에서 지원하지 않는 operation |
| `ZLINK_SUBMIT_INVALID_STATE` | `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN` | socket lifecycle 또는 request state 오류 |
| `ZLINK_SUBMIT_THREAD_VIOLATION` | `EDEADLK`, `EPERM` | 금지한 callback 재진입 또는 thread 사용 |
| `ZLINK_SUBMIT_OUT_OF_MEMORY` | `ENOMEM` | 필요한 storage 확보 실패 |
| `ZLINK_SUBMIT_SEQ_EXHAUSTED` | `EOVERFLOW` | operation sequence 공간 소진 |
| `ZLINK_SUBMIT_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |

각 socket 문서가 input ownership과 socket별 세부 조건을 정의한다.

### 3. Request completion result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_REQUEST_OK` | - | terminal success |
| `ZLINK_REQUEST_TIMED_OUT` | `ETIMEDOUT` | operation deadline 만료 |
| `ZLINK_REQUEST_NOT_FOUND` | `ENOENT` | terminal target 부재 |
| `ZLINK_REQUEST_TERMINATED` | `ETERM`, `ESHUTDOWN` | owner lifecycle 종료 |
| `ZLINK_REQUEST_PROTOCOL_ERROR` | `EPROTO`, `ENOCOMPATPROTO` | malformed 또는 호환되지 않는 reply |
| `ZLINK_REQUEST_INTERNAL_ERROR` | 보존된 errno | 다른 terminal 분류가 없는 내부 실패 |
| `ZLINK_REQUEST_REJECTED` | `EACCES`, `ECANCELED` | peer 또는 admission 거절 |
| `ZLINK_REQUEST_CONFLICT` | `EEXIST`, `ESTALE` | request correlation 또는 generation 충돌 |
| `ZLINK_REQUEST_BUSY` | `EBUSY` | active request lifecycle 존재 |
| `ZLINK_REQUEST_NOT_CONNECTED` | `ENOTCONN` | terminal route 단절 |
| `ZLINK_REQUEST_INVALID_ARGUMENT` | `EINVAL` | asynchronous validation 실패 |
| `ZLINK_REQUEST_INVALID_STATE` | `ESTALE`, `EALREADY`, `ESHUTDOWN` | terminal request state 오류 |
| `ZLINK_REQUEST_NOT_SUPPORTED` | `ENOTSUP` | operation 미지원 |
| `ZLINK_REQUEST_BACKPRESSURED` | `EAGAIN`, `ENOBUFS` | non-blocking admission 또는 reservation 실패 |

Request submit 성공 뒤에는 operation ID마다 terminal result를 정확히 한 번 reply callback으로 전달한다.

### 4. Receive result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_RECV_OK` | - | complete record 하나 이상 수신 |
| `ZLINK_RECV_NO_DATA` | `EAGAIN`, `ETIMEDOUT` | nonblocking 또는 receive timeout에 data 없음 |
| `ZLINK_RECV_BUSY` | `EBUSY` | 다른 receive mode 사용 중 |
| `ZLINK_RECV_TERMINATED` | `ETERM` | Context 종료 |
| `ZLINK_RECV_INVALID_HANDLE` | `EFAULT` | handle 또는 필수 output pointer가 유효하지 않음 |
| `ZLINK_RECV_NOT_SUPPORTED` | `ENOTSUP` | handle이 해당 receive를 지원하지 않음 |
| `ZLINK_RECV_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |
| `ZLINK_RECV_BUFFER_TOO_SMALL` | `ENOBUFS` | caller output capacity 부족 |
| `ZLINK_RECV_INVALID_STATE` | `EINVAL`, `ESTALE`, `ESHUTDOWN` | receive lifecycle state 오류 |

Raw subscription의 `BUFFER_TOO_SMALL`에서는 필요한 topic 길이만 기록하고 queued topic·payload와 다른
output을 변경하지 않는다.

### 5. Handler와 close result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_HANDLER_INVALID_ARGUMENT` | `EINVAL` | handler 또는 mask가 잘못됨 |
| `ZLINK_HANDLER_BUSY` | `EBUSY` | 배타적인 receive model이 이미 등록됨 |
| `ZLINK_HANDLER_NOT_SUPPORTED` | `ENOTSUP` | handle에서 handler를 지원하지 않음 |
| `ZLINK_HANDLER_DEADLOCK` | `EDEADLK` | 같은 callback 안의 금지한 등록·해제 |
| `ZLINK_HANDLER_INVALID_HANDLE` | `EFAULT` | handle이 유효하지 않음 |
| `ZLINK_HANDLER_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |
| `ZLINK_CLOSE_BUSY` | `EBUSY`, `EDEADLK` | active child·callback·API가 존재하거나 같은 handle close가 재진입함 |
| `ZLINK_CLOSE_SHUTDOWN` | `ESHUTDOWN` | 이미 종료된 handle |
| `ZLINK_CLOSE_INVALID_HANDLE` | `EFAULT`, `ESTALE` | pointer 또는 opaque value가 유효하지 않음 |
| `ZLINK_CLOSE_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |

### 6. Bind와 connect result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_BIND_INVALID_ARGUMENT` | `EINVAL` | endpoint가 잘못됨 |
| `ZLINK_BIND_ADDR_IN_USE` | `EADDRINUSE` | endpoint가 이미 사용 중 |
| `ZLINK_BIND_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | transport 미지원 |
| `ZLINK_BIND_INVALID_HANDLE` | `EFAULT` | handle이 유효하지 않음 |
| `ZLINK_BIND_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 bind 실패 |
| `ZLINK_CONNECT_INVALID_ARGUMENT` | `EINVAL` | endpoint 또는 expected RID가 잘못됨 |
| `ZLINK_CONNECT_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | transport 또는 operation 미지원 |
| `ZLINK_CONNECT_INVALID_HANDLE` | `EFAULT` | handle이 유효하지 않음 |
| `ZLINK_CONNECT_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 connect 실패 |
| `ZLINK_CONNECT_NOT_FOUND` | `ENOENT` | connection intent가 없음 |
| `ZLINK_CONNECT_CONFLICT` | `EEXIST`, `ESTALE`, `EADDRINUSE` | routing ID, endpoint 또는 connection lifecycle 충돌 |
| `ZLINK_CONNECT_BUSY` | `EBUSY`, `ESHUTDOWN` | lifecycle이 변경을 허용하지 않음 |
| `ZLINK_CONNECT_AUTH_FAILED` | `EACCES` | transport peer 인증 실패 |

### 7. Configuration result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_CONFIG_INVALID_HANDLE` | `EFAULT` | handle 또는 output pointer가 유효하지 않음 |
| `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | option, size, name 또는 value가 잘못됨 |
| `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` | handle과 option 조합 미지원 |
| `ZLINK_CONFIG_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |
| `ZLINK_CONFIG_INVALID_STATE` | `EINVAL`, `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN`, `ENOTCONN`, `ETIMEDOUT`, `EPROTO` | socket lifecycle 또는 terminal state가 변경을 거부함 |
| `ZLINK_CONFIG_NOT_FOUND` | `ENOENT` | local query target 없음 |
| `ZLINK_CONFIG_CONFLICT` | `EEXIST` | 중복 identity, endpoint 또는 등록 값 |
| `ZLINK_CONFIG_BUFFER_TOO_SMALL` | `ENOBUFS` | caller output capacity 부족, partial output 없음 |
| `ZLINK_CONFIG_BUSY` | `EBUSY` | 같은 mutable object를 동시에 사용함 |

## 내부 구조

이 절은 Core가 API 경계에서 안정적인 공개 result를 노출하면서 내부적으로는 어떻게 상세한
오류를 유지하는지 설명한다.

### Layers

- 내부 실행 경로는 계속 `int errno`를 사용한다.
- 공개 C API는 실패를 **함수 범주별 8개 typed result enum**으로 정규화한다. 정확한 enum은
  함수 범주에 따라 달라진다.
  - `zlink_submit_result_t` — send / publish / request submit / reply submit
  - `zlink_request_result_t` — request completion (callback)
  - `zlink_recv_result_t` — recv / subscribe / monitor recv / timer recv
  - `zlink_handler_result_t` — handler 등록
  - `zlink_close_result_t` — close / destroy
  - `zlink_bind_result_t` — bind
  - `zlink_connect_result_t` — connect / disconnect / unbind
  - `zlink_config_result_t` — option set/get, snapshot, poller mutation,
    message lifecycle, timer config
- Result enum 값은 0-706 범위 전체에서 전역적으로 고유하므로 하나의 `int` 값만으로도 항상
  출처를 명확히 식별할 수 있다.
- 정식 enum 목록은 위의 [Result와 errno 대응](#result와-errno-대응) 절을 참조한다.
- Request reply callback은 여전히 raw `errno_`를 전달하지만, 이 completion channel은 계약상
  `zlink_request_result_t`로 정규화되어 있다.

코드는 세 파일을 중심으로 구성된다.

- [core/include/zlink_errno.h](../../../include/zlink_errno.h)는
  공개 확장 errno 값을 정의한다.
- [core/include/zlink_enum.h](../../../include/zlink_enum.h)는
  공개 result enum을 정의한다.
- [core/src/runtime/core/internal_errno.hpp](../../../src/runtime/core/internal_errno.hpp)는
  정규화 helper가 사용하는 내부 errno catalog를 정의한다.

### 내부 `errno`를 유지하는 이유

Core는 여전히 `errno`로 실패를 알리는 OS와 protocol 코드와 상호작용한다. 이 상세 channel을
유지해야 구현 내부에서 정보를 잃지 않는다.

공개 caller는 그 정도의 세부 정보가 필요하지 않다. caller에게 필요한 것은 안정적인 result
분류다. 그래서 정규화는 공개 경계에서만 일어난다.

### 구현 규칙

Core와 benchmark/test helper 코드 내부에서 공개 result enum은 boolean이 아니라 이름이 있는
result code로 다뤄야 한다.

- `rc == ZLINK_*_OK` 또는 `rc != ZLINK_*_OK`를 사용한다.
- `if (!zlink_bind(...))`같은 boolean 스타일 검사를 작성하지 않는다.

이 규칙이 중요한 이유는 모든 공개 result enum이 성공을 `0`으로 사용하기 때문이다. boolean
스타일은 성공과 실패를 조용히 뒤집을 수 있으며, 이는 정확히 typed-result 정책이 막으려는
종류의 버그다.

### Submit 정규화

Send, request submit과 reply submit은 하나의 공개 result 타입인 `zlink_submit_result_t`(14개
값: OK, BACKPRESSURED, NOT_CONNECTED, NOT_FOUND, NOT_ADMITTED, TERMINATED, INVALID_HANDLE,
INVALID_ARGUMENT, NOT_SUPPORTED, INVALID_STATE, THREAD_VIOLATION, OUT_OF_MEMORY, SEQ_EXHAUSTED,
INTERNAL_ERROR)을 공유한다.

정규화 helper는
[core/src/api/message/submit_result_internal.hpp](../../../src/api/message/submit_result_internal.hpp)에
있다. 이 helper는 내부 submit errno catalog를 공개 submit result로 대응시킨다.

### Request completion 정규화

Request completion은 별도의 공개 result 타입인 `zlink_request_result_t`(13개 값: OK, TIMED_OUT,
NOT_FOUND, TERMINATED, PROTOCOL_ERROR, INTERNAL_ERROR, REJECTED, CONFLICT, BUSY, NOT_CONNECTED,
INVALID_ARGUMENT, INVALID_STATE, NOT_SUPPORTED)을 사용한다.

정규화 helper는
[core/src/api/message/request_result_internal.hpp](../../../src/api/message/request_result_internal.hpp)에
있다. 이 helper는 callback completion errno 값을 공개 completion result 계약으로 대응시킨다.

### Binding 표면

언어 bindings는 이 8개 범주 구조를 함수별 8개 exception/error subclass(예: `SubmitException` /
`BindException` / `RecvException` ...)로 그대로 물려받는다. method signature를 보면 어떤 실패
범주가 발생할 수 있는지 알 수 있다. 정식 binding 규칙은
[bindings/doc/spec/README.md](../../../../bindings/doc/spec/README.ko.md)(Per-Function Error Type
Hierarchy)를, 전체 enum 목록은 위의 [Result와 errno 대응](#result와-errno-대응) 절을 참조한다.

### `zlink_errno()`의 범위

`zlink_errno()`는 주로 **`INTERNAL_ERROR`의 세부 정보 접근자**로 존재한다(그리고 여러 원인을
아직 하나로 묶은 몇몇 거친 bucket을 위해서도 존재한다). 공개 result enum이 이미 자기 설명적이면
(예: `BACKPRESSURED`, `NOT_FOUND`, `TIMED_OUT`), caller는 `zlink_errno()`를 참고할 필요가 없다.
