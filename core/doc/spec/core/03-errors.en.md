---
title: "Errors"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/03-errors/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Message](02-message.en.md) | [Next: Events](04-events.en.md)
<!-- zlink-nav:end -->

# Errors

> **What this chapter defines** — The contract for public result enums, errno, and
> version queries. The detailed result-to-errno table is in
> [Result and errno mapping](#result-and-errno-mapping) in this document.

## 1. Errors overview

ZLink Core public functions report failures at two levels. They return the primary control
flow through a typed result enum for each function category (`zlink_*_result_t`). They
retain a more detailed cause separately for each calling thread as thread-local errno,
which callers query with `zlink_errno()`. This document defines that error ABI contract:
public result enums, errno constants, and version queries. Its audience is C API and
bindings developers. It answers: "How do typed public function results map to
thread-local errno, and how is the build version identified?"

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Ownership of an input message submitted to a [socket](glossary.en.md#socket) and detailed failure conditions for each socket | [Socket common specification](socket/README.en.md) and each formal socket document |
| Function declaration and state enum for `zlink_socket_set_receive_flow_state()` | [Socket common specification](socket/README.en.md) |
| Behavior resulting from receive flow state configuration | [DEALER](socket/06-dealer.en.md), [ROUTER](socket/07-router.en.md) |
| Per-function error type hierarchy for language bindings | [Bindings specification](https://zlink-systems.github.io/zlink/bindings/spec/README/) |

## 2. Basic result and errno rules

Public functions return their primary control flow through `zlink_*_result_t` and record
the detailed cause in `zlink_errno()` for the same thread. Callers branch on the result
enum and use errno for logging and finer diagnosis. Success is always numeric `0`, and
errno is unspecified after success.

## 3. Extended errno constants

```c
#define ZLINK_HAUSNUMERO 156384712      // Base value for ZLink extended errno values

#define EFSM            (ZLINK_HAUSNUMERO + 51)
#define ENOCOMPATPROTO  (ZLINK_HAUSNUMERO + 52)
#define ETERM           (ZLINK_HAUSNUMERO + 53)
#define EMTHREAD        (ZLINK_HAUSNUMERO + 54)

#ifndef ESTALE
#define ESTALE          (ZLINK_HAUSNUMERO + 19)  // stale handle
#endif
#ifndef EALREADY
#define EALREADY        (ZLINK_HAUSNUMERO + 20)  // duplicate operation
#endif
#ifndef EDEADLK
#define EDEADLK         (ZLINK_HAUSNUMERO + 21)  // forbidden reentry
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN       (ZLINK_HAUSNUMERO + 22)  // closed socket
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE      (ZLINK_HAUSNUMERO + 23)  // peer socket type does not match the operation
#endif
#ifndef EOVERFLOW
#define EOVERFLOW       (ZLINK_HAUSNUMERO + 24)  // completion ID sequence exhausted
#endif
```

POSIX errno values missing on a platform use public values based on
`ZLINK_HAUSNUMERO`. `ESTALE`, `EALREADY`, `EDEADLK`, `ESHUTDOWN`, `EPROTOTYPE`,
and `EOVERFLOW`, which represent a stale handle, duplicate operation, forbidden
reentry, closed socket, peer-type rejection, and sequence exhaustion, are available
with the values above on every supported platform.

The same fallback rule applies to the following 18 POSIX errno values. When the platform
does not define a name, the public header defines it with values from
`ZLINK_HAUSNUMERO + 1` through `+ 18`, in this order: `ENOTSUP`,
`EPROTONOSUPPORT`, `ENOBUFS`, `ENETDOWN`, `EADDRINUSE`, `EADDRNOTAVAIL`,
`ECONNREFUSED`, `EINPROGRESS`, `ENOTSOCK`, `EMSGSIZE`, `EAFNOSUPPORT`,
`ENETUNREACH`, `ECONNABORTED`, `ECONNRESET`, `ENOTCONN`, `ETIMEDOUT`,
`EHOSTUNREACH`, and `ENETRESET`. When the platform already defines a name, ZLink
uses the platform value. [Result and errno mapping](#result-and-errno-mapping) owns
the exact per-function mappings.

## 4. Result enums

Each function category uses one result enum. This section contains the declarations and
usage rules for each category. [Result and errno mapping](#result-and-errno-mapping)
owns the errno mapping and meaning of each value.

### 4.1 Submit result

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
runtime control flow. `NOT_ADMITTED` means that the target route was identified, but
the raw socket's current admission state rejected a new submit. This includes a peer
whose handshake has not completed. The submit owner document separately defines input
message ownership; callers do not infer it from the result value alone.

### 4.2 Request completion result

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

This enum represents terminal completion of a raw socket request operation. A timeout
is represented by `ZLINK_REQUEST_TIMED_OUT`. `BACKPRESSURED` means that the request
failed because capacity was unavailable before outbound admission.

### 4.3 Receive and handler results

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

`BUFFER_TOO_SMALL` means that a caller-provided batch cannot hold the first complete
message, or that a raw SUB/XSUB or XPUB topic buffer is shorter than the required
length. A zero-length topic succeeds with zero capacity and a `NULL` buffer. Topic
receive returns the required topic length without consuming the queued topic or payload,
so the caller can retry with a sufficient buffer. `INVALID_STATE` applies to a stale
handle or a terminated receive state.

### 4.4 Close, bind, and connect results

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

A raw connect intent or routing ID collision is `CONFLICT`. A transport peer
authentication mismatch is `AUTH_FAILED`.

### 4.5 Configuration result

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

`CONFLICT` represents a duplicate name, duplicate binding, or process-local identity
collision. `BUFFER_TOO_SMALL` means that query or retain output capacity is insufficient
and no partial caller-owned output was written. `BUSY` means that the same mutable batch
or configuration object was used concurrently.

### 4.6 Receive flow state configuration result

`zlink_socket_set_receive_flow_state()` returns the following results. The
[Socket common specification](socket/README.en.md) owns the function declaration and
state enum. [DEALER](socket/06-dealer.en.md) and [ROUTER](socket/07-router.en.md) own
the resulting behavior.

| Condition | Result | errno |
|---|---|---|
| A DEALER or ROUTER handle and a state within the range of `zlink_receive_flow_state_t`. This includes setting the state that the socket already holds | `ZLINK_CONFIG_OK` | unspecified |
| A handle that is `NULL`, is not a socket, or has already completed close teardown | `ZLINK_CONFIG_INVALID_HANDLE` | unspecified |
| A state value outside the range of `zlink_receive_flow_state_t` | `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL` |
| A socket type that does not support receive flow: PAIR, PUB, SUB, XPUB, XSUB, or STREAM | `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` |
| A concurrent close acquired socket admission before this call | `ZLINK_CONFIG_INVALID_STATE` | `ESHUTDOWN` |
| The owning [Context](glossary.en.md#context) is terminating | `ZLINK_CONFIG_INTERNAL_ERROR` | `ETERM` |

Setting the current state again is a successful no-op, not an error. This state is an
absolute value, not a counter.

A concurrent close and this call compete for the same socket admission, and only the one
admitted first is observed. Both outcomes of the race are defined. `INVALID_STATE` means
that close was admitted first while the handle was still registered. `INVALID_HANDLE`
means that close already completed teardown, so the handle no longer resolves to a socket.
Neither outcome applies the state partially.

## 5. Version and diagnostic functions

```c
#define ZLINK_VERSION_MAJOR 0
#define ZLINK_VERSION_MINOR 13
#define ZLINK_VERSION_PATCH 0

#define ZLINK_MAKE_VERSION(major, minor, patch) \
  ((major) * 10000 + (minor) * 100 + (patch))

#define ZLINK_VERSION \
  ZLINK_MAKE_VERSION(ZLINK_VERSION_MAJOR, ZLINK_VERSION_MINOR, ZLINK_VERSION_PATCH)
```

Core uses SOVERSION 0.

### zlink_errno

Returns the calling thread's errno value.

```c
ZLINK_EXPORT int zlink_errno(void);
```

Returns the detailed cause errno recorded by a public function on the same thread. It
returns only the calling thread's value, and the value is unspecified after a successful
call ([§2](#2-basic-result-and-errno-rules)).

**Return value:** The calling thread's current errno value.

**Thread safety:** Thread-safe. Returns only the calling thread's value.

**See also:** `zlink_strerror`

---

### zlink_strerror

Returns a description string for an errno value.

```c
ZLINK_EXPORT const char *zlink_strerror(int errnum);
```

The caller does not free or modify the returned pointer. For a ZLink extended errno, it
points to a constant string inside the library. For any other errno, it points to the
platform libc `strerror` result. Therefore, callers must not assume that the pointer
remains valid after later calls or locale changes. Copy the string immediately if it must
be retained.

**Return value:** A pointer to the description string for `errnum`. Do not free or modify
it; its lifetime follows the rules above.

**Thread safety:** May be called from any thread.

**See also:** `zlink_errno`

---

### zlink_version

Queries the version of the linked ZLink build.

```c
ZLINK_EXPORT void zlink_version(int *major, int *minor, int *patch);
```

Writes the major, minor, and patch values to the three output pointers. All three pointers
must be non-NULL. Passing `NULL` for any pointer results in undefined behavior.

**Return value:** None (`void`).

**Thread safety:** Thread-safe.

**See also:** `zlink_errno`

## Result and errno mapping

This section defines the mapping between result enums and thread-local errno for the
ZLink Core raw public API. Results drive control flow; errno describes the same failure
in more detail.

### 1. Common precedence

When failure conditions overlap in one call, the function returns one result in this
order: argument, handle and lifecycle, target and connection lookup, capacity, then
transport and internal failure. Errno is unspecified after a successful call.

### 2. Submit result

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_SUBMIT_OK` | - | The ownership transition defined by the function completed |
| `ZLINK_SUBMIT_BACKPRESSURED` | `EAGAIN`, `ETIMEDOUT`, `ENOBUFS` | Socket queue or reservation capacity is unavailable |
| `ZLINK_SUBMIT_NOT_CONNECTED` | `ENOTCONN`, `EHOSTUNREACH` | No target connection |
| `ZLINK_SUBMIT_NOT_FOUND` | `ENOENT` | No raw target |
| `ZLINK_SUBMIT_NOT_ADMITTED` | `EACCES`, `ECONNREFUSED`, `EPROTOTYPE` | Handshake, raw routing admission, or peer socket type rejected the submit |
| `ZLINK_SUBMIT_TERMINATED` | `ETERM`, `ESHUTDOWN` | Context or socket lifecycle ended |
| `ZLINK_SUBMIT_INVALID_HANDLE` | `EFAULT` | The handle is `NULL` or has the wrong kind |
| `ZLINK_SUBMIT_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | Invalid pointer, count, metadata, or flags |
| `ZLINK_SUBMIT_NOT_SUPPORTED` | `ENOTSUP` | The handle does not support the operation |
| `ZLINK_SUBMIT_INVALID_STATE` | `EBUSY`, `ESTALE`, `EALREADY` | Socket lifecycle or request state error |
| `ZLINK_SUBMIT_THREAD_VIOLATION` | `EDEADLK`, `EPERM`, `EMTHREAD` | Forbidden reentry or thread use |
| `ZLINK_SUBMIT_OUT_OF_MEMORY` | `ENOMEM` | Required storage could not be acquired |
| `ZLINK_SUBMIT_SEQ_EXHAUSTED` | `EOVERFLOW` | Operation sequence space is exhausted |
| `ZLINK_SUBMIT_INTERNAL_ERROR` | preserved errno | Internal failure without another public category |

Each socket document defines input ownership and socket-specific detailed conditions.

### 3. Request completion result

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_REQUEST_OK` | - | Terminal success |
| `ZLINK_REQUEST_TIMED_OUT` | `ETIMEDOUT` | Operation deadline expired |
| `ZLINK_REQUEST_NOT_FOUND` | `ENOENT` | Terminal target is absent |
| `ZLINK_REQUEST_TERMINATED` | `ETERM`, `ESHUTDOWN` | Owner lifecycle ended |
| `ZLINK_REQUEST_PROTOCOL_ERROR` | `EPROTO`, `ENOCOMPATPROTO` | Malformed or incompatible reply |
| `ZLINK_REQUEST_INTERNAL_ERROR` | preserved errno | Internal failure without another terminal category |
| `ZLINK_REQUEST_REJECTED` | `EACCES`, `ECONNREFUSED`, `ECANCELED` | Peer or admission rejection |
| `ZLINK_REQUEST_CONFLICT` | `EEXIST`, `ESTALE` | Request correlation or generation conflict |
| `ZLINK_REQUEST_BUSY` | `EBUSY` | An active request lifecycle exists |
| `ZLINK_REQUEST_NOT_CONNECTED` | `ENOTCONN`, `EHOSTUNREACH` | Terminal route is disconnected |
| `ZLINK_REQUEST_INVALID_ARGUMENT` | `EINVAL`, `EFAULT` | Asynchronous validation failure |
| `ZLINK_REQUEST_INVALID_STATE` | `EFSM`, `EALREADY` | Terminal request state error |
| `ZLINK_REQUEST_NOT_SUPPORTED` | `ENOTSUP`, `EOPNOTSUPP` | Unsupported operation |
| `ZLINK_REQUEST_BACKPRESSURED` | `EAGAIN`, `ENOBUFS` | Nonblocking admission or reservation failed |

After a successful request submit, exactly one terminal result is delivered by
`zlink_completion_recv()` for each nonzero completion ID.

### 4. Receive result

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_RECV_OK` | - | At least one complete record was received |
| `ZLINK_RECV_NO_DATA` | `EAGAIN`, `ETIMEDOUT` | No data under nonblocking receive or receive timeout |
| `ZLINK_RECV_BUSY` | `EBUSY` | Another receive mode is active |
| `ZLINK_RECV_TERMINATED` | `ETERM` | Context terminated |
| `ZLINK_RECV_INVALID_HANDLE` | `EFAULT` | The handle or a required output pointer is invalid |
| `ZLINK_RECV_NOT_SUPPORTED` | `ENOTSUP` | The handle does not support this receive operation |
| `ZLINK_RECV_INTERNAL_ERROR` | preserved errno | Internal failure without another public category |
| `ZLINK_RECV_BUFFER_TOO_SMALL` | `ENOBUFS` | Caller output capacity is insufficient |
| `ZLINK_RECV_INVALID_STATE` | `EINVAL`, `ESTALE`, `ESHUTDOWN` | Receive lifecycle state error |

For raw subscription and XPUB receive, `BUFFER_TOO_SMALL` records only the required
topic length and leaves the queued record and other outputs unchanged.

### 5. Handler and close result

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_HANDLER_INVALID_ARGUMENT` | `EINVAL` | A handler argument is invalid |
| `ZLINK_HANDLER_BUSY` | `EBUSY` | An exclusive handler state already exists |
| `ZLINK_HANDLER_NOT_SUPPORTED` | `ENOTSUP` | The handle does not support the handler operation |
| `ZLINK_HANDLER_DEADLOCK` | `EDEADLK` | Forbidden handler reentry |
| `ZLINK_HANDLER_INVALID_HANDLE` | `EFAULT` | The handle is invalid |
| `ZLINK_HANDLER_INTERNAL_ERROR` | preserved errno | Internal failure without another public category |
| `ZLINK_CLOSE_BUSY` | `EBUSY`, `EDEADLK` | An active child or API exists, or close reentered the same handle |
| `ZLINK_CLOSE_SHUTDOWN` | `ESHUTDOWN` | The handle is already shut down |
| `ZLINK_CLOSE_INVALID_HANDLE` | `EFAULT`, `ESTALE` | The pointer or opaque value is invalid |
| `ZLINK_CLOSE_INTERNAL_ERROR` | preserved errno | Internal failure without another public category |

### 6. Bind and connect result

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_BIND_INVALID_ARGUMENT` | `EINVAL` | The endpoint is invalid |
| `ZLINK_BIND_ADDR_IN_USE` | `EADDRINUSE` | The endpoint is already in use |
| `ZLINK_BIND_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | Unsupported transport |
| `ZLINK_BIND_INVALID_HANDLE` | `EFAULT` | The handle is invalid |
| `ZLINK_BIND_INTERNAL_ERROR` | preserved errno | Bind failure without another public category |
| `ZLINK_CONNECT_INVALID_ARGUMENT` | `EINVAL` | The endpoint or expected RID is invalid |
| `ZLINK_CONNECT_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | Unsupported transport or operation |
| `ZLINK_CONNECT_INVALID_HANDLE` | `EFAULT` | The handle is invalid |
| `ZLINK_CONNECT_INTERNAL_ERROR` | preserved errno | Connect failure without another public category |
| `ZLINK_CONNECT_NOT_FOUND` | `ENOENT` | No connection intent exists |
| `ZLINK_CONNECT_CONFLICT` | `EEXIST`, `ESTALE`, `EADDRINUSE` | Routing ID, endpoint, or connection lifecycle conflict |
| `ZLINK_CONNECT_BUSY` | `EBUSY`, `ESHUTDOWN` | The lifecycle does not allow the change |
| `ZLINK_CONNECT_AUTH_FAILED` | `EACCES` | Transport peer authentication failed |

### 7. Configuration result

| Result | errno | Meaning |
|---|---|---|
| `ZLINK_CONFIG_INVALID_HANDLE` | `EFAULT` | The handle or output pointer is invalid |
| `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | The option, size, name, or value is invalid |
| `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` | Unsupported handle and option combination |
| `ZLINK_CONFIG_INTERNAL_ERROR` | preserved errno | Internal failure without another public category |
| `ZLINK_CONFIG_INVALID_STATE` | `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN`, `ENOTCONN`, `ETIMEDOUT`, `EPROTO` | Socket lifecycle or terminal state rejected the change |
| `ZLINK_CONFIG_NOT_FOUND` | `ENOENT` | No local query target exists |
| `ZLINK_CONFIG_CONFLICT` | `EEXIST` | Duplicate identity, endpoint, or registration value |
| `ZLINK_CONFIG_BUFFER_TOO_SMALL` | `ENOBUFS` | Caller output capacity is insufficient; no partial output |
| `ZLINK_CONFIG_BUSY` | `EBUSY` | The same mutable object is used concurrently |

## Internal structure

> **Contract ownership for this section** — [Result and errno mapping](#result-and-errno-mapping)
> and [Implementation and contract-test verification requirements](#implementation-and-contract-test-verification-requirements)
> in this document own the public result and errno contract. This section explains how
> Core retains detailed errors internally while exposing stable public results at the API
> boundary.

### Layers

- Internal execution paths continue to use `int errno`.
- The public C API normalizes failures into **eight typed result enums by function
  category**. The exact enum depends on the function category.
  - `zlink_submit_result_t` — send / publish / request submit / reply submit
  - `zlink_request_result_t` — request completion record
  - `zlink_recv_result_t` — recv / subscribe / monitor recv / timer recv
  - `zlink_handler_result_t` — handler operation
  - `zlink_close_result_t` — close / destroy
  - `zlink_bind_result_t` — bind
  - `zlink_connect_result_t` — connect / disconnect / unbind
  - `zlink_config_result_t` — option set/get, snapshot, poller mutation,
    message lifecycle, timer config
- Nonzero result enum values use nonoverlapping numeric ranges for each family
  (1-13, 101-113, 201-208, 301-306, 401-404, 501-505, 601-608, and 701-709).
  Therefore, any nonzero `int` value always identifies its origin unambiguously.
- See [Result and errno mapping](#result-and-errno-mapping) above for the formal enum
  catalog.
- The request completion queue passes internal errno through `from_errno` normalization
  as `zlink_request_result_t`; this completion channel is normalized by contract as
  `zlink_request_result_t`.

The code is organized around three files.

- [core/include/zlink_errno.h](https://github.com/zlink-systems/zlink/blob/main/core/include/zlink_errno.h)
  defines public extended errno values.
- [core/include/zlink_enum.h](https://github.com/zlink-systems/zlink/blob/main/core/include/zlink_enum.h)
  defines public result enums.
- [core/src/runtime/core/internal_errno.hpp](https://github.com/zlink-systems/zlink/blob/main/core/src/runtime/core/internal_errno.hpp)
  defines the internal errno catalog used by normalization helpers.

### Why internal `errno` is retained

Core continues to interact with OS and protocol code that reports failures through
`errno`. Retaining this detailed channel prevents loss of information inside the
implementation.

Public callers do not need that degree of detail. They need stable result categories.
Normalization therefore occurs only at the public boundary.

### Implementation rules

Core and benchmark/test helper code must treat public result enums as named result codes,
not as booleans.

- Use `rc == ZLINK_*_OK` or `rc != ZLINK_*_OK`.
- Do not write boolean-style checks such as `if (!zlink_bind(...))`.

This rule matters because every public result enum uses `0` for success. Boolean-style
checks can silently invert success and failure, which is exactly the type of bug that the
typed-result policy prevents.

### Submit normalization

Send, request submit, and reply submit share one public result type,
`zlink_submit_result_t` (14 values: OK, BACKPRESSURED, NOT_CONNECTED, NOT_FOUND,
NOT_ADMITTED, TERMINATED, INVALID_HANDLE, INVALID_ARGUMENT, NOT_SUPPORTED,
INVALID_STATE, THREAD_VIOLATION, OUT_OF_MEMORY, SEQ_EXHAUSTED, INTERNAL_ERROR).

The normalization helper is in
[core/src/api/message/submit_result_internal.hpp](https://github.com/zlink-systems/zlink/blob/main/core/src/api/message/submit_result_internal.hpp).
It maps the internal submit errno catalog to public submit results.

### Request completion normalization

Request completion uses a separate public result type, `zlink_request_result_t`
(14 values: OK, TIMED_OUT, NOT_FOUND, TERMINATED, PROTOCOL_ERROR, INTERNAL_ERROR,
REJECTED, CONFLICT, BUSY, NOT_CONNECTED, INVALID_ARGUMENT, INVALID_STATE,
NOT_SUPPORTED, BACKPRESSURED).

The normalization helper is in
[core/src/api/message/request_result_internal.hpp](https://github.com/zlink-systems/zlink/blob/main/core/src/api/message/request_result_internal.hpp).
It maps completion errno values to the public completion result contract.

### Binding surface

Language bindings inherit this eight-category structure as eight per-function
exception/error subclasses (for example, `SubmitException` / `BindException` /
`RecvException` ...). A method signature identifies the failure category that can occur.
See
[bindings/doc/spec/README.md](https://zlink-systems.github.io/zlink/bindings/spec/README/)
(Per-Function Error Type Hierarchy) for the formal binding rules and
[Result and errno mapping](#result-and-errno-mapping) above for the complete enum list.

### Scope of `zlink_errno()`

`zlink_errno()` exists primarily as an **`INTERNAL_ERROR` detail accessor** and also
for a few coarse buckets that still combine multiple causes. When a public result enum
already describes the failure (for example, `BACKPRESSURED`, `NOT_FOUND`, or
`TIMED_OUT`), the caller does not need to consult `zlink_errno()`.

## Implementation and contract-test verification requirements

Verify the following using only the public surface: the result returned by each public
function, the completion result from `zlink_completion_recv()`, `zlink_errno()`,
`zlink_strerror()`, and `zlink_version()`. Each item maps to one unit test.

**Common rules**

- The success value of every public result enum is numeric `0`.
- When a public function fails, `zlink_errno()` on the same thread returns one of the
  errno values in that result's row in
  [Result and errno mapping](#result-and-errno-mapping).
- When failure conditions overlap in one call, the function returns exactly one result in
  this order: argument, handle and lifecycle, target and connection lookup, capacity, then
  transport and internal failure.
- The value of `zlink_errno()` is unspecified after a successful call. Tests do not
  verify errno after success.
- `zlink_errno()` returns only the calling thread's value. A failing call on another
  thread does not change this thread's `zlink_errno()` value.

**Extended errno constants**

- Even when a platform does not provide their POSIX definitions, `ESTALE`, `EALREADY`,
  `EDEADLK`, `ESHUTDOWN`, `EPROTOTYPE`, and `EOVERFLOW` are observed on every supported
  platform with the public `ZLINK_HAUSNUMERO`-based values in
  [§3](#3-extended-errno-constants).

**Submit and request completion**

- When a ROUTER sends a typed request to a DEALER RID, the result is
  `ZLINK_SUBMIT_NOT_ADMITTED` with `EPROTOTYPE`.
- When the completion ID sequence is exhausted, submit returns
  `ZLINK_SUBMIT_SEQ_EXHAUSTED` with `EOVERFLOW` and ID `0`.
- After a successful request submit, exactly one terminal result
  (`zlink_request_result_t`) is delivered as a REQUEST completion for each nonzero ID.
- When a peer sends an errno from [Request completion result](#3-request-completion-result)
  in the first 4-byte part of a valid error reply, `zlink_completion_recv()` receives the
  `zlink_request_result_t` from the same row. An unlisted nonzero errno produces
  `ZLINK_REQUEST_INTERNAL_ERROR`.

**Receive**

- When the topic buffer for SUB or XPUB receive is shorter than the required length, the
  result is `ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`. Only the required length is
  recorded; the queued record and other outputs remain unchanged. A zero-length topic
  succeeds with zero capacity and a `NULL` buffer.

**Receive flow state**

- `zlink_socket_set_receive_flow_state()` returns the result and errno from the
  corresponding row in
  [§4.6](#46-receive-flow-state-configuration-result) for each condition.
- DEALER supports `zlink_socket_set_receive_flow_state()` without a separate Completion lane and
  returns `ZLINK_CONFIG_OK` for a valid state. PAIR, PUB, SUB, XPUB, XSUB, and STREAM return
  `ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP` because those socket types do not support receive flow.
- Setting the state that the socket already holds is a successful no-op that returns
  `ZLINK_CONFIG_OK`.
- When the call races with a concurrent close, only
  `ZLINK_CONFIG_INVALID_STATE` (`ESHUTDOWN`) or `ZLINK_CONFIG_INVALID_HANDLE` is
  observed, and neither outcome applies the state partially.

**Version and diagnostic functions**

- `zlink_version()` writes major, minor, and patch to three non-NULL output pointers.
  Passing `NULL` is undefined.
- `zlink_strerror()` may be called from any thread and returns a non-NULL description
  string for a ZLink extended errno. The caller does not free or modify the pointer and
  copies the string immediately if it must be retained.
- `zlink_errno()` and `zlink_version()` are safe to call concurrently from multiple
  threads.

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Message](02-message.en.md) | [Next: Events](04-events.en.md)
<!-- zlink-nav:end -->
