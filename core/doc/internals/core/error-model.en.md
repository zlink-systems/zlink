# Core Error Model

This document explains how core keeps detailed internal errors while exposing
stable public results at the API boundary.

## Layers

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
- See [spec/core/04-errno-map.md](../../spec/core/04-errno-map.en.md)
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

## Why Internal `errno` Stays

Core still interacts with OS and protocol code that naturally reports failure
through `errno`. Keeping that detailed channel avoids losing information inside
the implementation.

Public callers do not need that full detail. They need stable result classes.
That is why normalization happens only at the exported boundary.

## Implementation Rule

Inside core and benchmark/test helper code, public result enums must be treated
as named result codes, not as booleans.

- Use `rc == ZLINK_*_OK` or `rc != ZLINK_*_OK`.
- Do not write boolean-style checks such as `if (!zlink_bind(...))`.

That rule matters because all public result enums use `0` for success. Boolean
style can silently invert success and failure, which is exactly the kind of bug
the typed-result policy is meant to prevent.

## Submit Normalization

Send, request submit, and reply submit share one public result type:
`zlink_submit_result_t` (14 values: OK, BACKPRESSURED, NOT_CONNECTED,
NOT_FOUND, NOT_ADMITTED, TERMINATED, INVALID_HANDLE, INVALID_ARGUMENT,
NOT_SUPPORTED, INVALID_STATE, THREAD_VIOLATION, OUT_OF_MEMORY,
SEQ_EXHAUSTED, INTERNAL_ERROR).

The normalization helper lives in
[core/src/api/message/submit_result_internal.hpp](../../../src/api/message/submit_result_internal.hpp).
It maps the internal submit errno catalog to public submit results.

## Request Completion Normalization

Request completion uses a separate public result type:
`zlink_request_result_t` (13 values: OK, TIMED_OUT, NOT_FOUND, TERMINATED,
PROTOCOL_ERROR, INTERNAL_ERROR, REJECTED, CONFLICT, BUSY, NOT_CONNECTED,
INVALID_ARGUMENT, INVALID_STATE, NOT_SUPPORTED).

The normalization helper lives in
[core/src/api/message/request_result_internal.hpp](../../../src/api/message/request_result_internal.hpp).
It maps callback completion errno values to the public completion result
contract.

## Binding Surface

Language bindings inherit this 8-category structure as eight per-function
exception/error subclasses (e.g. `SubmitException` / `BindException` /
`RecvException` ...). The method signature reveals which failure category
can occur. See
[bindings/doc/spec/README.md](../../../../bindings/doc/spec/README.en.md)
(Per-Function Error Type Hierarchy) for the canonical binding rule and
[spec/core/04-errno-map.md](../../spec/core/04-errno-map.en.md)
for the full enum catalog.

## `zlink_errno()` Scope

`zlink_errno()` exists primarily as an **`INTERNAL_ERROR` detail accessor**
(and for the handful of coarse buckets that still collapse multiple causes).
When a public result enum is already self-descriptive (e.g. `BACKPRESSURED`,
`NOT_FOUND`, `TIMED_OUT`), callers do not need to consult `zlink_errno()`.
