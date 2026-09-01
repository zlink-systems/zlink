---
title: "Polling"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/05-polling/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Events](04-events.en.md) | [Next: Monitoring](06-monitoring.en.md)
<!-- zlink-nav:end -->

# Polling

> **What this chapter defines** — The public contract for waiting on the readiness
> of multiple [sockets](glossary.en.md#socket) and sources through the `zlink_poll`
> and `zlink_poller_*` APIs.

## 1. Polling overview

This document defines the ZLink Core public readiness contract. Readiness is the
state in which it is worthwhile for a source to proceed with receive or send. An
application can wait for three types of sources—raw sockets, OS file descriptors,
and generic timers—together in one event loop. The intended audience is developers
who carry this contract into the C API and each language binding. This document
answers: “What do `POLLIN` and `POLLOUT`, single-consumer receive mode, and lifetime
mean for each source?”

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Event-family classification and the boundary of readiness semantics | [Events](04-events.en.md) |
| Specific meanings of `ZLINK_POLLIN` and `ZLINK_POLLOUT` for each socket type | [Socket — DEALER](socket/06-dealer.en.md#5-results-and-readiness), [Socket — ROUTER](socket/07-router.en.md#10-results-and-readiness) |
| Result and errno mapping | [Errors](03-errors.en.md#result-and-errno-mapping) |
| Generic timer creation and receive (`zlink_timer_*`) | [Utilities](07-utilities.en.md) |

## 2. One-shot poll and reusable poller

There are two ways to wait for readiness.

- **One-shot poll** — [`zlink_poll`](#zlink_poll) receives its targets as an item
  array on each call and waits once.
- **Reusable poller** — The [`zlink_poller_*`](#poller-functions) functions keep
  sources registered with a poller object and repeatedly wait through
  `zlink_poller_wait`.

## 3. Source types and readiness

The readiness reported through `ZLINK_POLLIN` and `ZLINK_POLLOUT` for each source
type is as follows.

| Source | `POLLIN` | `POLLOUT` | Additional readiness and rules |
|---|---|---|---|
| raw socket | A complete record can be received | A submit retry is worthwhile | Per-socket receive mode applies. A closed registered socket reports `ZLINK_POLLERR` once |
| timer | A fire count can be received | Unsupported | Drain with `zlink_timer_recv()` |
| FD | Platform-readable | Platform-writable | Platform `POLLPRI` maps to `ZLINK_POLLPRI`; all other platform error bits map to `ZLINK_POLLERR` |

For a raw socket with multiple peers, `ZLINK_POLLOUT` is aggregate readiness
for the socket. The event does not identify which routing ID or transport pair
became writable and can be raised because another peer has capacity. Therefore,
after a nonblocking submit to one target reports backpressure, observing
`ZLINK_POLLOUT` does not guarantee that the next submit to that target succeeds.
Use the part-send completion ID and `zlink_completion_recv()` to distinguish
operation-specific admission results.

`ZLINK_POLLITEMS_DFLT` is the recommended initial item count for internal and
application stack buffers; it is not a readiness bit. `ZLINK_HAVE_POLLER == 1`
means that this public poller API is included in the build.

## 4. Completion polling

`ZLINK_POLLCOMPLETION` is level-triggered readiness indicating that a PAIR,
DEALER, ROUTER, or STREAM socket-local completion queue contains at least one
record and the next `zlink_completion_recv()` can succeed. It can be registered
alone or OR-ed with `ZLINK_POLLIN` and `ZLINK_POLLOUT`. Readiness remains set
while records remain in the queue.

`zlink_poller_wait()` does not remove completions or invoke callbacks. The event
array does not contain operation payloads, and its capacity is unrelated to the
number of completions. For each ready socket, the caller repeatedly invokes
`zlink_completion_recv(..., ZLINK_RECV_FLAGS_DONTWAIT)` until it receives
`ZLINK_RECV_NO_DATA`. Add, modify, and remove do not consume the queue.

```mermaid
sequenceDiagram
    participant App as Application
    participant P as Poller
    participant S as Socket completion queue
    App->>P: Call zlink_poller_wait()
    P-->>App: Return POLLCOMPLETION readiness
    loop Until NO_DATA
        App->>S: zlink_completion_recv(DONTWAIT)
        S-->>App: One SEND or REQUEST completion
    end
```

`zlink_poller_add()` and `zlink_poller_modify()` can add or remove the completion
bit for a supported socket. Using the bit with another source or in a
`zlink_poll()` item returns `ZLINK_CONFIG_INVALID_ARGUMENT` with `errno == EINVAL`.

At most one poller registration owns a socket's completion bit. If another poller
already owns it, adding the socket or adding the bit through modify fails with
`ZLINK_CONFIG_INVALID_STATE` and `errno == EBUSY`, and the existing registration
remains unchanged. Another poller may take ownership after the current owner removes
the bit with modify or removes the registration. Queue records and readiness are not
lost during the transfer. The application maintains one completion-drain owner per
socket.

## 5. Source lifetime and serialization

When a socket source is registered with a poller, Core acquires a lifetime pin on
that socket. It is therefore safe for the application to close a registered
socket before removing it from the poller. A closed socket source reports
`POLLERR` once, and its registration and lifetime pin remain in place until it is
removed.

The caller serializes add, modify, remove, and wait on one poller. Different
pollers can be used concurrently. An event array returned by wait is caller-owned
and contains no pointer to Core storage.

## 6. Public types

```c
#if defined _WIN32
typedef uintptr_t zlink_fd_t;
#else
typedef int zlink_fd_t;
#endif

typedef short zlink_poller_event_mask_t;

typedef enum zlink_poller_event_flag_e {
  ZLINK_POLLIN         = 1,   // receive can proceed (source-specific meaning in §3)
  ZLINK_POLLOUT        = 2,   // send/submit retry is worthwhile (source-specific meaning in §3)
  ZLINK_POLLERR        = 4,   // socket close or FD platform error (§3, §5)
  ZLINK_POLLPRI        = 8,   // platform POLLPRI for an FD (§3)
  ZLINK_POLLITEMS_DFLT = 16,  // recommended initial item count; not a readiness bit (§3)
  ZLINK_POLLCOMPLETION = 32   // socket completion-queue readiness (§4)
} zlink_poller_event_flag_e;

#define ZLINK_HAVE_POLLER 1   // public poller API is included in the build

typedef enum zlink_poller_source_kind_t {
  ZLINK_POLLER_SOURCE_SOCKET    = 1,  // raw socket
  ZLINK_POLLER_SOURCE_FD        = 2,  // OS file descriptor
  ZLINK_POLLER_SOURCE_TIMER     = 3   // generic timer
} zlink_poller_source_kind_t;

typedef struct zlink_pollitem_t {
  void *socket;    // valid only for a SOCKET source
  zlink_fd_t fd;   // valid only for an FD source
  short events;    // event bits to wait for
  short revents;   // returned readiness; initialize to 0 before the call (§7 zlink_poll)
} zlink_pollitem_t;

typedef struct zlink_poller_event_t {
  zlink_poller_source_kind_t source_kind;  // source type for this event
  void *socket;     // valid only for a SOCKET source
  zlink_fd_t fd;    // valid only for an FD source
  void *timer;      // valid only for a TIMER source
  void *user_data;  // borrowed value returning the pointer supplied at registration
  short events;     // observed readiness bits
} zlink_poller_event_t;
```

## 7. Functions

### zlink_poll

Waits once for the readiness of an item array.

```c
ZLINK_EXPORT int zlink_poll(
  zlink_pollitem_t *items,
  int item_count,
  long timeout_ms,
  zlink_config_result_t *error_out);
```

The return value is the number of items with readiness, `0` on timeout, and `-1`
on failure. Failure sets both `error_out` and errno. `timeout_ms == -1` waits
indefinitely, and `0` returns immediately. Each item's `revents` is initialized
to `0` before the call, and only its snapshot after the function returns is
valid. `error_out` is an optional output that may be NULL.

### Poller functions

```c
ZLINK_EXPORT void *zlink_poller_new(void);
ZLINK_EXPORT zlink_close_result_t zlink_poller_destroy(void **poller_p);
ZLINK_EXPORT int zlink_poller_size(void *poller, zlink_config_result_t *error_out);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add(
  void *poller,
  void *source,
  void *user_data,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_modify(
  void *poller,
  void *source,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove(void *poller, void *source);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add_fd(
  void *poller,
  zlink_fd_t fd,
  void *user_data,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_modify_fd(
  void *poller,
  zlink_fd_t fd,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove_fd(void *poller, zlink_fd_t fd);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add_timer(
  void *poller,
  void *timer,
  void *user_data);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove_timer(
  void *poller,
  void *timer);
ZLINK_EXPORT int zlink_poller_wait(
  void *poller,
  zlink_poller_event_t *events,
  int event_capacity,
  long timeout_ms,
  zlink_config_result_t *error_out);
```

On success, `zlink_poller_new()` returns a new poller. If allocation fails, it
returns `NULL` and sets `errno` to `ENOMEM`. On successful completion,
`zlink_poller_destroy()` sets the caller-provided pointer to NULL.

`zlink_poller_size()` returns the current registration count on success and `-1`
on failure. `zlink_poller_wait()` returns the number of events written on
success, `0` on timeout, and `-1` on failure. If `events == NULL` or
`event_capacity <= 0`, it fails with `EINVAL`. The `error_out` parameters of
`zlink_poller_size()` and `zlink_poller_wait()` are optional outputs that may be
NULL.

Adding the same source twice returns `ZLINK_CONFIG_CONFLICT`/`EEXIST`. Modifying
or removing a missing source returns `ZLINK_CONFIG_NOT_FOUND`/`ENOENT`. An
invalid event bit returns `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`, while an
event unsupported by the source returns `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`.
Destroying a poller while a wait is active returns `ZLINK_CLOSE_BUSY`/`EBUSY`.
The [errno map](03-errors.en.md#result-and-errno-mapping) defines all result and
errno mappings.

## 8. Internal structure

> **Contract ownership for this section** — The public contract for completion
> polling is owned by [Completion polling](#4-completion-polling) and
> [Verification requirements](#9-implementation-and-contract-test-verification-requirements)
> in this document. This section describes how that contract is achieved
> internally.

SEND and REQUEST resolvers append results to the same socket-local ready queue. The
linearization order of these appends is the public receive order; it does not imply
submit order or per-target wire order.

## 9. Implementation and contract-test verification requirements

Verify the following using only the public surface: the `zlink_poll`,
`zlink_poller_*`, and `zlink_completion_recv` functions, return values and errno,
and event-array contents. Each item maps to one unit test.

**zlink_poll**

- It returns the number of items with readiness, `0` on timeout, and `-1` on failure, setting both `error_out` and errno on failure.
- `timeout_ms == -1` waits indefinitely, and `0` returns immediately.
- A `revents` field initialized to `0` before the call is valid only as a snapshot after the function returns.

**Poller registration**

- Adding the same source twice returns `ZLINK_CONFIG_CONFLICT`/`EEXIST`.
- Modifying or removing a missing source returns `ZLINK_CONFIG_NOT_FOUND`/`ENOENT`.
- An invalid event bit returns `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`, while an event unsupported by the source returns `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`.
- `ZLINK_POLLCOMPLETION` can be added or removed by add or modify for PAIR, DEALER, ROUTER, and STREAM, alone or OR-ed with other socket readiness. Other sources and `zlink_poll()` items return `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`.
- When two pollers try to own the same socket's completion bit, the second add or modify fails with `ZLINK_CONFIG_INVALID_STATE`/`EBUSY`, and the existing registration remains unchanged. Moving ownership after the current owner removes the bit or source loses neither queued records nor readiness.

**Wait and events**

- Registering a socket source acquires a lifetime pin, so it is safe to close the socket before removal. After close, it reports `POLLERR` once and remains registered until removal.
- Platform `POLLPRI` for an FD maps to `ZLINK_POLLPRI`; all other platform error bits map to `ZLINK_POLLERR`.
- The event fields `socket`, `fd`, and `timer` are valid only for SOCKET, FD, and TIMER sources, respectively, and `user_data` returns the pointer supplied at registration unchanged.
- An event array returned by wait is caller-owned and contains no pointer to Core storage.

**Completion polling**

- Wait returns `ZLINK_POLLCOMPLETION` while at least one completion record exists; calls to wait, add, modify, or remove alone do not reduce the queue.
- Receiving the last record with DONTWAIT completion receive clears readiness; readiness remains set while records remain.
- Completions are neither lost nor merged when their count exceeds event-array capacity; the caller drains each ready socket until `ZLINK_RECV_NO_DATA`.

**Lifetime**

- Destroying a poller while a wait is active returns `ZLINK_CLOSE_BUSY`/`EBUSY`.

**Poller function returns and outputs**

- Allocation failure in `zlink_poller_new` returns `NULL`/`ENOMEM`, and successful `zlink_poller_destroy` sets the caller pointer to NULL.
- `zlink_poller_size` returns the registration count or `-1` on failure.
- `zlink_poller_wait` returns an event count, `0` on timeout, and `-1` on failure; `events == NULL` or `event_capacity <= 0` returns `EINVAL`.
- The `error_out` parameters of `zlink_poll`, `zlink_poller_size`, and `zlink_poller_wait` are optional outputs that may be NULL.

Caller serialization of add, modify, remove, and wait on one poller is a usage
precondition ([§5](#5-source-lifetime-and-serialization)); concurrent use of
different pollers is allowed.
