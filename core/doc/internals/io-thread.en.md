English | [한국어](io-thread.ko.md)

# I/O Thread Internals

This document describes what I/O threads do inside a zlink context,
how they are created, and how work is distributed across them.

For the high-level threading model (application threads, reaper thread,
inter-thread communication), see [Threading Model](threading-model.en.md).

## 1. Overview

Each I/O thread runs a dedicated **async event loop** that:

1. Polls registered sockets for read/write readiness
2. Processes commands received through its mailbox
3. Executes timers

I/O threads are the workhorses of zlink networking: all actual network
send/receive, protocol encoding/decoding, and connection management
happens on I/O threads.

## 2. Creation and Lifecycle

I/O threads are created lazily — `zlink_ctx_new()` allocates the
context but threads are not spawned until the first socket is created.

```c
void *ctx = zlink_ctx_new();
zlink_ctx_set(ctx, ZLINK_IO_THREADS, 4);  /* must be set before first socket */

void *socket = zlink_socket(ctx, ZLINK_SOCKET_DEALER);  /* triggers thread launch */
```

Internally, `ctx_runtime_resources.cpp:start_io_threads_locked()` creates
`io_thread_count` instances of `io_thread_t`. Each thread receives a
unique slot ID and its mailbox is registered in the context's slot
registry for command routing.

Thread names follow the pattern `IO/0`, `IO/1`, ... `IO/N-1`.

## 3. Event Loop

Each I/O thread owns a Boost ASIO-based poller (`asio_poller.cpp`).
The main loop in `poller_t::loop()` repeats the following cycle:

```
┌─────────────────────────────────────────────┐
│                Event Loop                   │
│                                             │
│  1. Execute due timers                      │
│  2. io_context.poll()  — non-blocking       │
│     Process all ready I/O events            │
│  3. If no events ready:                     │
│     io_context.run_for(≤100ms) — blocking   │
│  4. Clean up retired poll entries            │
│                                             │
│  ← repeat ─────────────────────────────────→│
└─────────────────────────────────────────────┘
```

- **Step 2** uses non-blocking `poll()` to batch-process all ready
  events in one pass for throughput.
- **Step 3** blocks up to 100 ms when no events are pending, avoiding
  busy-wait CPU burn.

## 4. Socket I/O Handling

Network I/O uses the Proactor model. The engine (`asio_engine_t`) issues
`async_read_some()` / `async_write_some()` to the transport; the I/O thread's
`io_context` waits for the OS async I/O to complete and then invokes the
completion handler. The engine does not poll readiness — it only processes
completion results.

- **Read complete** → the received bytes are handed to the protocol decoder to
  decode frames, messages are pushed into the receive pipe, and the next
  `async_read_some()` is issued.
- **Write complete** → a message pulled from the send pipe is encoded and sent;
  if data remains, the next `async_write_some()` is issued.

`asio_poller`'s `async_wait` readiness path is used only for mailbox command
wakeup, not for network data.

## 5. Command Processing

Each I/O thread has a **mailbox** — a command pipe
(`ypipe_t<command_t>`, send side protected by a mutex) paired with a signaler for wake-up.

```cpp
// io_thread.cpp — process_mailbox()
do {
    command_t cmd;
    int rc = _mailbox.recv(&cmd, 0);
    while (rc == 0 || errno == EINTR) {      // EINTR retry
        if (rc == 0)
            cmd.destination->process_command(cmd);
        rc = _mailbox.recv(&cmd, 0);
    }
} while (_mailbox.reschedule_if_needed());    // reschedule if more remain
```

Commands arrive from application threads (via `ctx_t::send_command()`)
and include operations such as:

| Command | Purpose |
|---------|---------|
| `plug` | Attach a new session/engine to this I/O thread |
| `attach` | Attach an engine to a session |
| `bind` | Establish pipe(s) between session and socket |
| `activate_read` | Resume reading from a pipe |
| `activate_write` | Resume writing to a pipe |
| `stop` | Shut down the I/O thread |

The mailbox is connected to the I/O thread's `io_context` (`set_io_context()`);
sending a command posts an ASIO handler that wakes the blocking event loop to
process the command.

## 6. Thread Assignment

When a socket creates a new connection, it picks an I/O thread based
on:

1. **Affinity mask** — if set, restricts the candidate set
2. **Load distribution** — general connections pick the thread with the fewest
   registered handles (least-load); STREAM connections default to round-robin
   (`ZLINK_ASIO_STREAM_SESSION_SCHED=minload` switches to least-load)

This distributes network connections across I/O threads for load
balancing. The assignment is per-connection, not per-socket — a single
socket with multiple connections may span several I/O threads.

## 7. Tuning Guidelines

| Scenario | Recommended `ZLINK_IO_THREADS` |
|----------|-------------------------------|
| Single socket, few connections | 1 |
| Many sockets or connections | 2–4 (default 4) |
| High-throughput server (100+ connections) | Match to available CPU cores |

Setting more I/O threads than CPU cores provides no benefit and adds
context-switch overhead. Profile with the
[perf benchmarks](../../../bindings/c/perf) before increasing beyond 4.

## Key Source Files

| File | Role |
|------|------|
| `core/src/runtime/core/io_thread.hpp/.cpp` | I/O thread class, mailbox processing |
| `core/src/runtime/core/ctx_runtime_resources.cpp` | Thread creation in `start_io_threads_locked()` |
| `core/src/runtime/engine/asio/asio_poller.hpp/.cpp` | Boost ASIO event loop and socket monitoring |
| `core/src/runtime/core/poller_base.hpp` | Worker thread base class |
| `core/src/runtime/core/mailbox.hpp` | Lock-free command queue with signaler |

---
[← Threading Model](threading-model.en.md)
