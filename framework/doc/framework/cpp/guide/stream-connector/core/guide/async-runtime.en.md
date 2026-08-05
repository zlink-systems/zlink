# C++ Stream Connector Async Runtime Guide

This document explains how a client using the core connector must handle callbacks and
`dispatch()`. Internal socket structure and Boost.Asio implementation details aren't public usage,
so they aren't described here.

## Dispatch Mode

In `manual` mode, a user callback runs on the thread that called `dispatch()`. Engine wrappers like
Unreal, Godot, and Axmol use this mode to run a delegate or signal on the engine main thread.

In `immediate` mode, the runtime worker that produced the completion can run the user callback.
Used in code with no engine main-thread constraint, like a CLI, tool, or perf client.

## Callback Completion

`request().submit(callback)`, `wait_for().submit(callback)`, `connect(callback)`,
`close(callback)` register the operation and return immediately. The synchronous
`request().submit()`, `wait_for().submit()`, `connect()`, `close()` are kept for existing users.

On TCP, TLS, WebSocket, and WSS transport, `connect(callback)` doesn't block the calling thread to
wait for connection completion or the handshake. TLS and WSS can be used in a build with the
OpenSSL feature turned on.

`send().submit()` submits a one-way send request and doesn't return a completion result to the
caller. Send acceptance and backpressure handling are the connector's internal responsibility.

`request().submit(callback)` returns after registering the request-frame write. When one of a
reply, timeout, close, or transport error occurs, it's delivered to the callback as a `result_t<T>`.
While waiting for a reply, neither the calling thread nor a worker thread is tied up in a blocking
wait.

`wait_for().submit(callback)` also waits via callback completion until one of a matching packet,
timeout, or close occurs. It doesn't occupy the calling thread while waiting.

It must be possible to call the connector API again from inside a callback. So the implementation
doesn't call the user callback while holding the connector's internal lock.

If the connector closes, a callback operation that hasn't finished yet must not remain as a success.
If the user called `close()` or `close(callback)` to shut it down, it completes with a `closed`
error. If the user canceled the operation via a coroutine task or an explicit cancellation token, a
`canceled` error is used. A transport drop is still distinguished as `disconnected`, as before.

## Engine Rule

An engine wrapper doesn't expose core types in its public API. A core callback is put into the
adapter queue, then converted and run as an engine delegate on the engine's Tick, Update, or an
explicit `Dispatch()`.
