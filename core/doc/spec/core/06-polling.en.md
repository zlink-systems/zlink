[한국어](06-polling.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md) · [errno map](04-errno-map.en.md)

# Poll and poller

This document defines the ZLink Core public readiness contract. Its audience is developers of the C API and bindings that wait for raw sockets, file descriptors, and generic timers in one event loop. It answers: “What do `POLLIN`, `POLLOUT`, single-consumer receive mode, and lifetime mean for each source?”

## 1. Public types

```c
#if defined _WIN32
typedef uintptr_t zlink_fd_t;
#else
typedef int zlink_fd_t;
#endif

typedef short zlink_poller_event_mask_t;

typedef enum zlink_poller_event_flag_e {
  ZLINK_POLLIN         = 1,
  ZLINK_POLLOUT        = 2,
  ZLINK_POLLERR        = 4,
  ZLINK_POLLPRI        = 8,
  ZLINK_POLLITEMS_DFLT = 16,
  ZLINK_POLLCOMPLETION = 32
} zlink_poller_event_flag_e;

#define ZLINK_HAVE_POLLER 1

typedef enum zlink_poller_source_kind_t {
  ZLINK_POLLER_SOURCE_SOCKET    = 1,
  ZLINK_POLLER_SOURCE_FD        = 2,
  ZLINK_POLLER_SOURCE_TIMER     = 3
} zlink_poller_source_kind_t;

typedef struct zlink_pollitem_t {
  void *socket;
  zlink_fd_t fd;
  short events;
  short revents;
} zlink_pollitem_t;

typedef struct zlink_poller_event_t {
  zlink_poller_source_kind_t source_kind;
  void *socket;
  zlink_fd_t fd;
  void *timer;
  void *user_data;
  short events;
} zlink_poller_event_t;
```

Only a `SOCKET` source has a valid `socket`, only an `FD` source has a valid `fd`, and only a `TIMER` source has a valid `timer`. `user_data` is the borrowed pointer supplied at registration.

## 2. One-shot poll

```c
ZLINK_EXPORT int zlink_poll(
  zlink_pollitem_t *items,
  int item_count,
  long timeout_ms,
  zlink_config_result_t *error_out);
```

The return value is the number of ready items, zero on timeout, and -1 on failure. Failure sets both `error_out` and errno. A timeout of -1 waits indefinitely; zero returns immediately. Every item’s `revents` is cleared before evaluation and is only a snapshot after return.

## 3. Reusable poller

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

Adding the same source twice returns `ZLINK_CONFIG_CONFLICT` with `EEXIST`. Modifying or removing a missing source returns `ZLINK_CONFIG_NOT_FOUND` with `ENOENT`. A poller borrows source handles, so a source is removed before it is destroyed. A registered source that closes produces `POLLERR` once and remains registered until removal.

The caller serializes add, modify, remove, and wait on one poller. Different pollers can be used concurrently. The event array returned by wait is caller-owned and contains no pointer to Core storage.

## 4. Readiness by source

| Source | `POLLIN` | `POLLOUT` | Additional rule |
|---|---|---|---|
| raw socket | A complete record can be received | A submit retry is worthwhile | Per-socket receive mode applies |
| timer | A fire count can be received | Unsupported | Drain with `zlink_timer_recv()` |
| FD | Platform-readable | Platform-writable | Platform poll semantics |

`ZLINK_POLLITEMS_DFLT` is the recommended initial item count for internal and
application stack buffers; it is not a readiness bit. `ZLINK_HAVE_POLLER == 1`
means this public poller API is present in the build.

`ZLINK_POLLCOMPLETION` is valid only when adding a raw DEALER or ROUTER with
`zlink_poller_add()`. It may be registered alone or OR-ed with `ZLINK_POLLIN`
and `ZLINK_POLLOUT` so one poller owns receive, send, and completion progress
for the socket. A request
completion signal is not a public receive record. When `zlink_poller_wait()`
observes the signal, Core receives reply payload from the paired Completion
transport and dispatches the registered reply callback on the thread executing
that wait call. Reply payload is not copied into a second internal payload
queue. Timeout, shutdown, and other payloadless terminal results may use a
small control queue to preserve callback ownership. If only a completion signal
was processed, no public event is produced and wait may return `0`; the caller
can inspect state changed by the callback and continue. The `recv_part`
families do not drain this completion. Using
`ZLINK_POLLCOMPLETION` on another source, in a `zlink_poll()` item, or in
`zlink_poller_modify()` returns `ZLINK_CONFIG_INVALID_ARGUMENT` with
`errno == EINVAL`.

## 5. Errors and close

An invalid event bit returns `ZLINK_CONFIG_INVALID_ARGUMENT` with `EINVAL`; an event unsupported by the source returns `ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP`. Destroy while wait is active returns `ZLINK_CLOSE_BUSY` with `EBUSY`. The [errno map](04-errno-map.en.md) defines all result and errno mappings.
