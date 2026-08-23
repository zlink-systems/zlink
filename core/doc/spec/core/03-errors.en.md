[한국어](03-errors.ko.md) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Message](02-message.en.md) | [Next: Events](04-events.en.md)
<!-- zlink-nav:end -->

# Errors, result enums, and version

This document defines the error ABI contract for ZLink Core. Its audience is developers of the C API and bindings. It answers: “How do typed public results map to thread-local errno, and how is the build version identified?”

## 1. General rules

Public functions return their primary control flow through a `zlink_*_result_t` and record a more detailed cause in `zlink_errno()` for the same thread. Callers branch on the result enum and use errno for logging and finer diagnosis. Success is always numeric zero, and errno is unspecified after success.

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

POSIX errno values missing on a platform use public values based on
`ZLINK_HAUSNUMERO`. `ESTALE`, `EALREADY`, `EDEADLK`, and `ESHUTDOWN` are
available on every supported platform for stale handles, duplicate operations,
reentrant callbacks, and closed sockets. The
[Result and errno mapping](#result-and-errno-mapping) section owns
the exact per-function mappings.

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

`BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND`, and `NOT_ADMITTED` are normal
runtime control flow. `NOT_ADMITTED` means that the target route was identified
but the raw socket's current admission state rejects a new submit. This includes
a peer whose handshake has not completed. The submit owner document separately defines input-message ownership;
callers do not infer it from the result value alone.

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

This enum represents terminal completion of a raw socket request operation.
Timeout is represented by `ZLINK_REQUEST_TIMED_OUT`. `BACKPRESSURED` means that
outbound admission failed because capacity was unavailable.

## 4. Receive and handler results

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

`BUFFER_TOO_SMALL` means that a caller-provided batch cannot hold the first
complete message, or that a raw SUB/XSUB topic-buffer capacity is zero or less
than the required length. Raw subscription receive reports the required topic
length without consuming the queued topic or payload, so the caller can retry
with a sufficient buffer. `INVALID_STATE` covers a stale handle or closed
receive state. Unregistering or replacing a handler from the same callback
returns `DEADLOCK`.

## 5. Close, bind, and connect results

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

A raw connect-intent or routing-ID collision is `CONFLICT`. A transport peer-authentication mismatch is `AUTH_FAILED`.

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

`CONFLICT` covers duplicate names, duplicate bindings, and process-local identity collisions. `BUFFER_TOO_SMALL` means that query or retain output capacity is insufficient and no partial caller-owned output was written. `BUSY` means that the same mutable batch or configuration object was used concurrently.

### 6.1 Receive flow state

`zlink_socket_set_receive_flow_state()` returns the following rows. The
[Socket overview](socket/README.en.md) owns the function declaration and the
state enum; [DEALER](socket/06-dealer.en.md) and [ROUTER](socket/07-router.en.md)
own the resulting behavior.

| Condition | Result | errno |
|---|---|---|
| A DEALER or ROUTER handle and a state inside `zlink_receive_flow_state_t`, including a repeat of the state the socket already holds | `ZLINK_CONFIG_OK` | unspecified |
| A null handle, a handle that is not a socket, or a handle whose close already finished its teardown | `ZLINK_CONFIG_INVALID_HANDLE` | unspecified |
| A state value outside `zlink_receive_flow_state_t` | `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL` |
| A socket type with no completion lane: PAIR, PUB, SUB, XPUB, XSUB, or STREAM | `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` |
| A concurrent close won admission for this socket before the call | `ZLINK_CONFIG_INVALID_STATE` | `ESHUTDOWN` |
| The owning context is terminating | `ZLINK_CONFIG_INTERNAL_ERROR` | `ETERM` |

Repeating the current state is a successful no-op, not an error: the state is
absolute, not a counter.

A concurrent close and this call compete for the same socket admission, and
only the one that is admitted first is observed. Both outcomes of that race are
defined. `INVALID_STATE` means that close was admitted first while the handle
was still registered. `INVALID_HANDLE` means that close had already completed
its teardown, so the handle no longer resolves to a socket. Neither outcome
applies the state partially.

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

Core uses SOVERSION 0. The pointer returned by `zlink_strerror()` refers to library-owned static storage and must not be freed or modified. All three functions are thread-safe, and `zlink_errno()` returns only the calling thread’s value.

## Result and errno mapping

This section defines result-enum and thread-local errno mappings for the ZLink
Core raw public API. Results drive control flow; errno describes the same
failure in more detail.

### 1. Common precedence

When failure conditions overlap, one result is selected in this order:
argument, handle and lifecycle, target and connection lookup, capacity, then
transport and internal failure. Errno is unspecified after success.

### 2. Submit results

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_SUBMIT_OK` | - | The function's ownership transition completed |
| `ZLINK_SUBMIT_BACKPRESSURED` | `EAGAIN`, `ETIMEDOUT`, `ENOBUFS` | Socket queue or reservation capacity is unavailable |
| `ZLINK_SUBMIT_NOT_CONNECTED` | `ENOTCONN` | No target connection |
| `ZLINK_SUBMIT_NOT_FOUND` | `ENOENT` | No raw target |
| `ZLINK_SUBMIT_NOT_ADMITTED` | `EACCES` | Handshake or raw-routing admission rejected the submit |
| `ZLINK_SUBMIT_TERMINATED` | `ETERM`, `ESHUTDOWN` | Context or socket lifecycle ended |
| `ZLINK_SUBMIT_INVALID_HANDLE` | `EFAULT` | Handle is null or has the wrong kind |
| `ZLINK_SUBMIT_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | Invalid pointer, count, metadata, or flags |
| `ZLINK_SUBMIT_NOT_SUPPORTED` | `ENOTSUP` | Operation is unsupported by the handle |
| `ZLINK_SUBMIT_INVALID_STATE` | `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN` | Invalid socket lifecycle or request state |
| `ZLINK_SUBMIT_THREAD_VIOLATION` | `EDEADLK`, `EPERM` | Forbidden callback reentry or thread use |
| `ZLINK_SUBMIT_OUT_OF_MEMORY` | `ENOMEM` | Required storage cannot be acquired |
| `ZLINK_SUBMIT_SEQ_EXHAUSTED` | `EOVERFLOW` | Operation-sequence space is exhausted |
| `ZLINK_SUBMIT_INTERNAL_ERROR` | preserved errno | Internal failure without a finer public category |

Each socket document defines input ownership and socket-specific conditions.

### 3. Request completion results

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_REQUEST_OK` | - | Terminal success |
| `ZLINK_REQUEST_TIMED_OUT` | `ETIMEDOUT` | Operation deadline expired |
| `ZLINK_REQUEST_NOT_FOUND` | `ENOENT` | Terminal target absence |
| `ZLINK_REQUEST_TERMINATED` | `ETERM`, `ESHUTDOWN` | Owner lifecycle ended |
| `ZLINK_REQUEST_PROTOCOL_ERROR` | `EPROTO`, `ENOCOMPATPROTO` | Malformed or incompatible reply |
| `ZLINK_REQUEST_INTERNAL_ERROR` | preserved errno | Internal failure without a finer terminal category |
| `ZLINK_REQUEST_REJECTED` | `EACCES`, `ECANCELED` | Peer or admission rejection |
| `ZLINK_REQUEST_CONFLICT` | `EEXIST`, `ESTALE` | Request-correlation or generation conflict |
| `ZLINK_REQUEST_BUSY` | `EBUSY` | Active request lifecycle |
| `ZLINK_REQUEST_NOT_CONNECTED` | `ENOTCONN` | Terminal route loss |
| `ZLINK_REQUEST_INVALID_ARGUMENT` | `EINVAL` | Asynchronous validation failure |
| `ZLINK_REQUEST_INVALID_STATE` | `ESTALE`, `EALREADY`, `ESHUTDOWN` | Terminal request-state error |
| `ZLINK_REQUEST_NOT_SUPPORTED` | `ENOTSUP` | Unsupported operation |
| `ZLINK_REQUEST_BACKPRESSURED` | `EAGAIN`, `ENOBUFS` | Nonblocking admission or reservation failed |

After successful request submission, exactly one terminal result is delivered
to the reply callback for each operation ID.

### 4. Receive results

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_RECV_OK` | - | At least one complete record was received |
| `ZLINK_RECV_NO_DATA` | `EAGAIN`, `ETIMEDOUT` | No data under nonblocking receive or receive timeout |
| `ZLINK_RECV_BUSY` | `EBUSY` | Another receive mode is active |
| `ZLINK_RECV_TERMINATED` | `ETERM` | Context terminated |
| `ZLINK_RECV_INVALID_HANDLE` | `EFAULT` | Invalid handle or required output pointer |
| `ZLINK_RECV_NOT_SUPPORTED` | `ENOTSUP` | Handle does not support the receive operation |
| `ZLINK_RECV_INTERNAL_ERROR` | preserved errno | Internal failure without a finer public category |
| `ZLINK_RECV_BUFFER_TOO_SMALL` | `ENOBUFS` | Caller output capacity is insufficient |
| `ZLINK_RECV_INVALID_STATE` | `EINVAL`, `ESTALE`, `ESHUTDOWN` | Invalid receive-lifecycle state |

For a raw subscription, `BUFFER_TOO_SMALL` reports only the required topic
length and leaves the queued topic, payload, and other outputs unchanged.

### 5. Handler and close results

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_HANDLER_INVALID_ARGUMENT` | `EINVAL` | Invalid handler or mask |
| `ZLINK_HANDLER_BUSY` | `EBUSY` | An exclusive receive model is already registered |
| `ZLINK_HANDLER_NOT_SUPPORTED` | `ENOTSUP` | Handle does not support the handler |
| `ZLINK_HANDLER_DEADLOCK` | `EDEADLK` | Forbidden registration or removal inside the same callback |
| `ZLINK_HANDLER_INVALID_HANDLE` | `EFAULT` | Invalid handle |
| `ZLINK_HANDLER_INTERNAL_ERROR` | preserved errno | Internal failure without a finer public category |
| `ZLINK_CLOSE_BUSY` | `EBUSY`, `EDEADLK` | An active child, callback, or API exists, or close reentered the same handle |
| `ZLINK_CLOSE_SHUTDOWN` | `ESHUTDOWN` | Handle is already stopped |
| `ZLINK_CLOSE_INVALID_HANDLE` | `EFAULT`, `ESTALE` | Invalid pointer or opaque value |
| `ZLINK_CLOSE_INTERNAL_ERROR` | preserved errno | Internal failure without a finer public category |

### 6. Bind and connect results

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_BIND_INVALID_ARGUMENT` | `EINVAL` | Invalid endpoint |
| `ZLINK_BIND_ADDR_IN_USE` | `EADDRINUSE` | Endpoint is already in use |
| `ZLINK_BIND_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | Unsupported transport |
| `ZLINK_BIND_INVALID_HANDLE` | `EFAULT` | Invalid handle |
| `ZLINK_BIND_INTERNAL_ERROR` | preserved errno | Bind failure without a finer public category |
| `ZLINK_CONNECT_INVALID_ARGUMENT` | `EINVAL` | Invalid endpoint or expected RID |
| `ZLINK_CONNECT_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | Unsupported transport or operation |
| `ZLINK_CONNECT_INVALID_HANDLE` | `EFAULT` | Invalid handle |
| `ZLINK_CONNECT_INTERNAL_ERROR` | preserved errno | Connect failure without a finer public category |
| `ZLINK_CONNECT_NOT_FOUND` | `ENOENT` | Connection intent does not exist |
| `ZLINK_CONNECT_CONFLICT` | `EEXIST`, `ESTALE`, `EADDRINUSE` | Routing-ID, endpoint, or connection-lifecycle conflict |
| `ZLINK_CONNECT_BUSY` | `EBUSY`, `ESHUTDOWN` | Lifecycle rejects the change |
| `ZLINK_CONNECT_AUTH_FAILED` | `EACCES` | Transport peer-authentication failure |

### 7. Configuration results

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_CONFIG_INVALID_HANDLE` | `EFAULT` | Invalid handle or output pointer |
| `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | Invalid option, size, name, or value |
| `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` | Unsupported handle and option combination |
| `ZLINK_CONFIG_INTERNAL_ERROR` | preserved errno | Internal failure without a finer public category |
| `ZLINK_CONFIG_INVALID_STATE` | `EINVAL`, `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN`, `ENOTCONN`, `ETIMEDOUT`, `EPROTO` | Socket lifecycle or terminal state rejects the change |
| `ZLINK_CONFIG_NOT_FOUND` | `ENOENT` | Local query target does not exist |
| `ZLINK_CONFIG_CONFLICT` | `EEXIST` | Duplicate identity, endpoint, or registration value |
| `ZLINK_CONFIG_BUFFER_TOO_SMALL` | `ENOBUFS` | Caller output capacity is insufficient; no partial output |
| `ZLINK_CONFIG_BUSY` | `EBUSY` | The same mutable object is in concurrent use |

## Internals

This section explains how Core keeps detailed internal errors while exposing
stable public results at the API boundary.

### Layers

- Internal execution paths keep using `int errno`.
- Exported C APIs normalize failures into **eight function-specific typed
  result enums**. The exact enum depends on the function category:
  - `zlink_submit_result_t` — send / publish / request submit / reply submit
  - `zlink_request_result_t` — request completion (callback)
  - `zlink_recv_result_t` — recv / subscribe / monitor recv / timer recv
  - `zlink_handler_result_t` — handler registration
  - `zlink_close_result_t` — close / destroy
  - `zlink_bind_result_t` — bind
  - `zlink_connect_result_t` — connect / disconnect / unbind
  - `zlink_config_result_t` — option set/get, snapshot, poller mutation,
    message lifecycle, timer config
- Result enum values are globally unique across the 0-706 range so a single
  `int` always identifies the origin unambiguously.
- See the [Result and errno mapping](#result-and-errno-mapping) section above
  for the canonical enum catalog.
- Request reply callbacks still pass raw `errno_`, but that completion channel
  is normalized by contract as `zlink_request_result_t`.

The code is organized around three files:

- [core/include/zlink_errno.h](../../../include/zlink_errno.h)
  defines public extended errno values.
- [core/include/zlink_enum.h](../../../include/zlink_enum.h)
  defines public result enums.
- [core/src/runtime/core/internal_errno.hpp](../../../src/runtime/core/internal_errno.hpp)
  defines the internal errno catalog used by normalization helpers.

### Why Internal `errno` Stays

Core still interacts with OS and protocol code that naturally reports failure
through `errno`. Keeping that detailed channel avoids losing information inside
the implementation.

Public callers do not need that full detail. They need stable result classes.
That is why normalization happens only at the exported boundary.

### Implementation Rule

Inside core and benchmark/test helper code, public result enums must be treated
as named result codes, not as booleans.

- Use `rc == ZLINK_*_OK` or `rc != ZLINK_*_OK`.
- Do not write boolean-style checks such as `if (!zlink_bind(...))`.

That rule matters because all public result enums use `0` for success. Boolean
style can silently invert success and failure, which is exactly the kind of bug
the typed-result policy is meant to prevent.

### Submit Normalization

Send, request submit, and reply submit share one public result type:
`zlink_submit_result_t` (14 values: OK, BACKPRESSURED, NOT_CONNECTED,
NOT_FOUND, NOT_ADMITTED, TERMINATED, INVALID_HANDLE, INVALID_ARGUMENT,
NOT_SUPPORTED, INVALID_STATE, THREAD_VIOLATION, OUT_OF_MEMORY,
SEQ_EXHAUSTED, INTERNAL_ERROR).

The normalization helper lives in
[core/src/api/message/submit_result_internal.hpp](../../../src/api/message/submit_result_internal.hpp).
It maps the internal submit errno catalog to public submit results.

### Request Completion Normalization

Request completion uses a separate public result type:
`zlink_request_result_t` (13 values: OK, TIMED_OUT, NOT_FOUND, TERMINATED,
PROTOCOL_ERROR, INTERNAL_ERROR, REJECTED, CONFLICT, BUSY, NOT_CONNECTED,
INVALID_ARGUMENT, INVALID_STATE, NOT_SUPPORTED).

The normalization helper lives in
[core/src/api/message/request_result_internal.hpp](../../../src/api/message/request_result_internal.hpp).
It maps callback completion errno values to the public completion result
contract.

### Binding Surface

Language bindings inherit this 8-category structure as eight per-function
exception/error subclasses (e.g. `SubmitException` / `BindException` /
`RecvException` ...). The method signature reveals which failure category
can occur. See
[bindings/doc/spec/README.md](../../../../bindings/doc/spec/README.en.md)
(Per-Function Error Type Hierarchy) for the canonical binding rule and the
[Result and errno mapping](#result-and-errno-mapping) section above for the
full enum catalog.

### `zlink_errno()` Scope

`zlink_errno()` exists primarily as an **`INTERNAL_ERROR` detail accessor**
(and for the handful of coarse buckets that still collapse multiple causes).
When a public result enum is already self-descriptive (e.g. `BACKPRESSURED`,
`NOT_FOUND`, `TIMED_OUT`), callers do not need to consult `zlink_errno()`.
