[한국어](15-polling.ko.md) | English

[Reference index](README.en.md)

# 15. Polling and pollers

This category covers the entry points for waiting on raw sockets, file descriptors, and generic
timers in one event loop: the one-shot `zlink_poll` and the reusable poller family. Poller
readiness is one of Core's three event families (the others are socket monitor events and timer
fires — Socket monitor and Timers categories). The exact signatures are owned by the
[Polling specification](../spec/core/06-polling.en.md).

---

## `zlink_poll`

Waits once on a caller-provided array of poll items.

```c
zlink_pollitem_t items[2] = {
    { .socket = s1, .events = ZLINK_POLLIN },
    { .socket = s2, .events = ZLINK_POLLIN | ZLINK_POLLOUT },
};
zlink_config_result_t err;
int ready = zlink_poll(items, 2, /*timeout_ms=*/1000, &err);
```

**Parameters.** `items` is an array of `zlink_pollitem_t` (`socket`/`fd`/`events`/`revents`).
`item_count` is the array length. `timeout_ms` is `-1` for infinite, `0` to return immediately.
`error_out` receives the failure detail.

**Return and errno.** Returns the number of ready items, `0` on timeout, or `-1` on failure (with
both `error_out` and `errno` set).

**When to use.** Use this for a single wait over a small, fixed set of items — every item's
`revents` is cleared before evaluation, so it is only a snapshot immediately after return. Use the
reusable poller family below instead when the item set changes often or you need `fd`/timer
sources, since re-scanning an array each call doesn't scale as well as an incrementally
maintained poller.

---

## `zlink_poller_new` / `zlink_poller_destroy` / `zlink_poller_size`

Creates a reusable poller, destroys it, or reads how many sources it currently holds.

```c
void *poller = zlink_poller_new();
// ...
zlink_config_result_t err;
int n = zlink_poller_size(poller, &err);
// ...
zlink_poller_destroy(&poller);
```

**Parameters.** `new` takes no arguments. `destroy` takes `void **poller_p` (may clear the
handle). `size` takes the poller and an `error_out` output.

**Return and errno.** `new` returns a poller handle or `NULL`. `destroy` returns
`zlink_close_result_t` — destroying while a `wait` is active on it returns `ZLINK_CLOSE_BUSY`
with `EBUSY`. `size` returns the count or `-1` on failure.

**When to use.** One poller instance handles as many sources as needed and is reused across
`wait` calls; different pollers may be used concurrently, but the caller must serialize
add/modify/remove/wait on any one poller.

---

## `zlink_poller_add` / `zlink_poller_modify` / `zlink_poller_remove`

Registers, updates, or removes a raw socket source on a poller.

```c
zlink_poller_add(poller, s, userdata, ZLINK_POLLIN);
zlink_poller_modify(poller, s, ZLINK_POLLIN | ZLINK_POLLOUT);
zlink_poller_remove(poller, s);
```

**Parameters.** `source` is the socket handle. `user_data` (add only) is the borrowed pointer
delivered back in matching `zlink_poller_event_t` entries. `events` is the same
`zlink_poller_event_mask_t` bits as `zlink_pollitem_t`, plus `ZLINK_POLLCOMPLETION` (valid only
when adding a raw DEALER or ROUTER — see below).

**Return and errno.** All three return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
Adding an already-registered source returns `ZLINK_CONFIG_CONFLICT` with `EEXIST`. Modifying or
removing a missing source returns `ZLINK_CONFIG_NOT_FOUND` with `ENOENT`. An invalid event bit
returns `ZLINK_CONFIG_INVALID_ARGUMENT` with `EINVAL`; an event unsupported by the source returns
`ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP`.

**When to use.** A poller only borrows the source handle — remove a source before destroying it.
Set `ZLINK_POLLCOMPLETION` (alone, or OR-ed with `ZLINK_POLLIN`/`ZLINK_POLLOUT`) when adding a
DEALER or ROUTER to let one poller own receive, send, and request-completion progress together;
using it on any other source, in a `zlink_poll` item, or in `zlink_poller_modify` returns
`ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`. A registered source that closes produces `POLLERR`
once and stays registered until explicitly removed.

---

## `zlink_poller_add_fd` / `zlink_poller_modify_fd` / `zlink_poller_remove_fd`

Registers, updates, or removes a raw platform file-descriptor source on a poller.

```c
zlink_poller_add_fd(poller, fd, userdata, ZLINK_POLLIN);
```

**Parameters.** `fd` is a `zlink_fd_t` (platform file descriptor/handle). Otherwise identical in
shape to `zlink_poller_add`/`_modify`/`_remove`.

**Return and errno.** Same as the socket-source trio above — `ZLINK_CONFIG_CONFLICT`/`EEXIST` on
duplicate add, `ZLINK_CONFIG_NOT_FOUND`/`ENOENT` on a missing source.

**When to use.** Use this to fold a plain OS file descriptor into the same event loop as sockets
and timers — readiness follows platform poll semantics (readable/writable), not the raw-socket
readiness rules.

---

## `zlink_poller_add_timer` / `zlink_poller_remove_timer`

Registers or removes a generic timer source (Timers category) on a poller.

```c
zlink_poller_add_timer(poller, timer, userdata);
zlink_poller_remove_timer(poller, timer);
```

**Parameters.** `timer` is a handle from `zlink_timer_new` (Timers category). No `events` mask —
a timer source only ever signals `POLLIN` (a fire count is available).

**Return and errno.** Both return `zlink_config_result_t` — the same conflict/not-found mapping
as the other two source families.

**When to use.** Use this to receive timer fires through the same `wait` loop as sockets and FDs,
draining the accumulated count with `zlink_timer_recv` (Timers category) once `wait` reports it.

---

## `zlink_poller_wait`

Waits for readiness across every source currently registered on a poller.

```c
zlink_poller_event_t events[16];
zlink_config_result_t err;
int ready = zlink_poller_wait(poller, events, 16, /*timeout_ms=*/1000, &err);
```

**Parameters.** `events`/`event_capacity` is the caller-owned output array and its size.
`timeout_ms` and `error_out` follow the same convention as `zlink_poll`.

**Return and errno.** Returns the number of ready events written, `0` on timeout, or `-1` with
`error_out`/`errno` set on failure. Each `zlink_poller_event_t` reports `source_kind`
(`SOCKET`/`FD`/`TIMER` — only the matching one of `socket`/`fd`/`timer` is valid),
`user_data` (the borrowed pointer from registration), and `events`.

**When to use.** The returned array is caller-owned and contains no pointer into Core storage.
When only a `ZLINK_POLLCOMPLETION` signal was processed internally (DEALER/ROUTER request
completion, Socket lifecycle category and DEALER/ROUTER categories), `wait` may return `0` with
no public event produced — the caller can inspect state the reply callback already changed and
continue; the `recv_part` families never drain this completion signal themselves.

---

## Readiness by source

| Source | `POLLIN` | `POLLOUT` | Additional rule |
|---|---|---|---|
| raw socket | A complete record can be received | A submit retry is worthwhile | Per-socket receive mode applies |
| timer | A fire count can be received | Unsupported | Drain with `zlink_timer_recv()` (Timers category) |
| FD | Platform-readable | Platform-writable | Platform poll semantics |

`ZLINK_POLLITEMS_DFLT` is a recommended initial item-count hint for stack buffers, not a
readiness bit. `ZLINK_HAVE_POLLER == 1` means this public poller API is present in the build.

---

See the [Polling specification](../spec/core/06-polling.en.md) for the full rationale.
