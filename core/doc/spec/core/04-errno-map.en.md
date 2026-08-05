[한국어](04-errno-map.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md) · [Errors and result enums](03-errors.en.md)

# Result and errno mapping

This document defines result-enum and thread-local errno mappings for the ZLink
Core raw public API. Results drive control flow; errno describes the same
failure in more detail.

## 1. Common precedence

When failure conditions overlap, one result is selected in this order:
argument, handle and lifecycle, target and connection lookup, capacity, then
transport and internal failure. Errno is unspecified after success.

## 2. Submit results

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

## 3. Request completion results

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

## 4. Receive results

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

## 5. Handler and close results

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

## 6. Bind and connect results

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

## 7. Configuration results

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
