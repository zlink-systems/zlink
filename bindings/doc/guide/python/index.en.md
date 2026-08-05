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
current Core 11 wheel is Linux x86_64. Other operating systems or CPU
architectures aren't treated as a supported target until a separate Core 11
candidate and clean-consumer verification are complete.

## Installation And A First Round Trip

```python
import zlink

with zlink.create_context() as ctx:
    with zlink.create_pair_socket(ctx) as sender:
        with zlink.create_pair_socket(ctx) as receiver:
            sender.bind("inproc://python-guide-pair")
            receiver.connect("inproc://python-guide-pair")

            sender.send().message(b"hello").submit()  # sends one part.
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
    socket.send().message(message).submit()  # native ownership moves once submit succeeds.

received = zlink.create_received()
socket.recv_into(received)  # returns False for a non-blocking call that allows no-data.
with received:
    parts = received.to_bytes_list()  # makes a snapshot to hand outside the owner.
```

A receive with `RecvFlags.DONT_WAIT` returns `False` when there's no message.
Control APIs that directly return a pending value — timers, monitors — return
`None` when there's no value.

## DEALER And ROUTER

DEALER and ROUTER carry a raw `RoutingId` and a request sequence. Read
`routing_id` off the `Received` the ROUTER gets, and the reply builder uses the
same metadata.

```python
with zlink.create_context() as ctx:
    with zlink.create_dealer_socket(ctx) as dealer:
        with zlink.create_router_socket(ctx) as router:
            router.bind("inproc://python-guide-request")
            dealer.connect("inproc://python-guide-request")

            dealer.request().message(b"ping").submit(on_reply)  # registers a reply callback.
            received = zlink.create_received()
            assert router.recv_into(received)
            with received:
                received.reply().message(b"pong").submit()  # replies using the received routing metadata.
```

See `request_reply_callback_sample.py` for real callback lifetime and timeout
handling.

## Routing ID And Errors

Fixed-length routing IDs are built with `RoutingId.from_(bytes)`. Empty values
and values exceeding the Core max length are rejected at input validation.

```python
rid = zlink.RoutingId.from_(b"server-01")
try:
    socket.send().message(b"data").submit()
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

## Samples And Perf

The raw sample runner includes the following:

- `pair_recv_sample.py`
- `dealer_router_recv_sample.py`
- `request_reply_callback_sample.py`
- `pubsub_recv_sample.py`
- `stream_recv_sample.py`
- `stream_packet_callback_sample.py`
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
