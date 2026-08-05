[한국어](03-errors.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md) · [errno map](04-errno-map.en.md)

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
reentrant callbacks, and closed sockets. The [errno map](04-errno-map.en.md) owns
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

Core uses SOVERSION 11. The pointer returned by `zlink_strerror()` refers to library-owned static storage and must not be freed or modified. All three functions are thread-safe, and `zlink_errno()` returns only the calling thread’s value.
