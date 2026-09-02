---
title: "Python Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: Node.js](../node/index.en.md) | [Next: Go](../go/index.en.md)
<!-- bindings-nav:end -->

# Python Binding Usage Guide

> **Contract-owning document for this chapter** — the [Python bindings spec](../../spec/python/README.en.md)
> covers it. This chapter shows that contract as working sample code.

This document explains how to use Core raw messaging through the `zlink` Python
package. It supports Python 3.9 and later; the native runtime target for the
current Core 0.9.0 wheel is Linux x86_64. Other operating systems or CPU
architectures aren't treated as a supported target until a separate Core 0.9.0
candidate and clean-consumer verification are complete.

## Installation And A First Round Trip

```python
import zlink

with zlink.create_context() as ctx:
    with zlink.create_pair_socket(ctx) as sender:
        with zlink.create_pair_socket(ctx) as receiver:
            sender.bind("inproc://python-guide-pair")
            receiver.connect("inproc://python-guide-pair")

            sender.send().message(b"hello").submit_sync()
            received = zlink.create_received()  # the caller owns the receive storage.
            assert receiver.recv_into(received)
            with received:
                assert received.to_bytes_list() == [b"hello"]
```

The `with` block releases the native resources of the context and socket. See
`bindings/python/samples/pair_recv_sample.py` for a TCP example across separate
processes.

## Message And Received

A send builder can accumulate multiple parts. `Message.from_(value)` builds a
message independent from the caller's value. The receive result is held by
`Received`; the native view under `parts` is only valid while its owner is
open. To pass it to another task or hold onto it longer, copy it explicitly as
shown below.

```python
message = zlink.Message.from_(bytearray(b"payload"))  # builds a message detached from the caller's buffer.
with message:
    socket.send().message(message).submit_sync()

received = zlink.create_received()
socket.recv_into(received)  # returns False for a non-blocking call that allows no-data.
with received:
    parts = received.to_bytes_list()  # makes a snapshot to hand outside the owner.
```

A receive with `RecvFlags.DONT_WAIT` returns `False` when there's no message.
Control APIs that directly return a pending value — timers, monitors — return
`None` when there's no value.

HWM-managed sends provide asynchronous `submit()` and synchronous
`submit_sync()` terminals. In async code, use
`await socket.send().message(message).submit()`; it submits with DONTWAIT and
settles from the socket completion queue. On a plain thread, `submit_sync()`
blocks in Core until local admission.

Request provides `submit_sync()` to block until the reply and `submit()` to
return an awaitable `list[Message]` settled from the socket completion queue.
The reply is that terminal result, not DATA received separately.

Core owns retry after accepting a pre-admission operation; do not create a
caller retry queue or resubmit its payload. The shared native
`ZLINK_OPT_PENDING_MAX_MSGS/BYTES` caps cover pending SEND and REQUEST; no
send-only pending names exist. Completion means local admission, not peer
delivery or an application acknowledgement.

Canceling an asyncio Task can stop the Python waiter. Before Core submit, abort
without calling Core; after Core accepts the payload, admission or request work
may continue and the socket owner drains a late completion. Set
`stream.options.recv_mode` to `zlink.StreamRecvMode.RAW` or `.PACKET` before
bind/connect, then use `recv_into` or `recv_packet_into` respectively.

If a public poller owns `zlink.PollEventFlag.POLLCOMPLETION` for a socket, keep
another thread calling `wait()` while a blocking request or awaitable is
pending. `wait()` drains native completions and settles or cleans Python state;
calling a blocking terminal between waits on the same thread can stall it.

## DEALER And ROUTER

DEALER and ROUTER carry a raw `RoutingId`; a ROUTER request also carries an
opaque `ReplyToken`. The reply builder requires both values from `Received`.

```python
--8<-- "bindings/python/samples/request_reply_async_sample.py:doc"
```

See `request_reply_async_sample.py` for the completion-backed awaitable and
reply-token lifetime.

## Routing ID And Errors

Fixed-length routing IDs are built with `RoutingId.from_(bytes)`. Empty values
and values exceeding the Core max length are rejected at input validation.

```python
rid = zlink.RoutingId.from_(b"server-01")
try:
    socket.send().message(b"data").submit_sync()
except zlink.SubmitError as exc:
    if exc.result == zlink.SubmitResult.BACKPRESSURED:
        # handle back-pressure as application policy, after checking the result.
        pass
    else:
        raise
```

`SubmitError`, `RequestError`, `RecvError`, `BindError`, `ConnectError`,
`ConfigError`, and `CloseError` are all `ZlinkError` subtypes and expose
`result`, `code`, and `native_errno`.

## Threading Notes

`submit_sync()` stops its calling thread while waiting for HWM admission. This
is safe on a plain thread because only that thread waits.
Calling it inside an asyncio event loop stops the entire loop, so other tasks and
send completions cannot progress. In asyncio code, `await` asynchronous
`submit()`.

## Samples And Perf

The raw sample runner includes the following:

- `pair_recv_sample.py`
- `dealer_router_recv_sample.py`
- `request_reply_async_sample.py`
- `pubsub_recv_sample.py`
- `stream_recv_sample.py`
- `stream_packet_recv_sample.py`
- `monitor_recv_sample.py`

The perf runner must specify which Core or wheel runtime to use. Compare the
path and SHA-256 it prints against the candidate evidence.

```bash
ZLINK_LIBRARY_PATH=/absolute/path/to/libzlink.so \
  bindings/python/perf/run_benchmarks.sh --smoke --pattern PAIR \
  --duration 1 --msg-sizes 64 --transports inproc --runs 1
```

Smoke mode checks process lifecycle and the required `RESULT` row; it doesn't
produce an official report.
