# java SupportChat — ClientServer reply delivered only at the request deadline

Job: root-cause the java SupportChat sample failure from framework gate g15
(`zlink-work/gates/g15/java-samples.log`), where the session node's ClientServer
request `AuthenticateUserReq` is answered by the api node in 35 ms but the
framework only observes `reply_received` 4.95 s later, at the request deadline.

## Cause

`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:22`

The framework's DEALER wrapper built its readiness poller with the
completion-queue-owning constructor:

```java
this.receivePoller = new ZLinkJavaSocketReceivePoller(socket);   // ownsCompletionQueue = true
```

`ZLinkJavaSocketReceivePoller.ensureRegistered()` then registers the socket as
`POLLIN | POLLOUT | POLLCOMPLETION`
(`.../binding/ZLinkJavaSocketReceivePoller.java:50`). Registering POLLCOMPLETION
on a public `Poller` calls `InternalAccess.completionTransferToPublic(socket, this)`
(`bindings/java/.../eventing/NativePoller.java` add/modify), which takes the
socket's completion queue away from the binding's per-Context `CompletionPump`
(`CompletionOwner.transferToPublic` → `runtime.unregister(this)`;
`CompletionOwner.drainFromRuntime()` then returns -1 while `publicOwner != null`,
`bindings/java/.../sockets/CompletionOwner.java:587`).

From that point the socket's REQUEST/WRITABLE completions are drained **only**
inside `Poller.wait(...)` on that framework poller
(`NativePoller.wait` → `InternalAccess.completionDrain`). For a ClientServer
client DEALER the only caller of `waitForReadable` is

`framework/.../runtime/channels/ZLinkChannelSocketRegistry.java:895`
`drainClientServerControls(connection)` → `connection.dealer.waitForReadable(Duration.ZERO)`

which is a **zero-timeout** probe driven by the 100 ms infrastructure tick
`tickClientServerLiveness` (`ZLinkChannelRuntime.java:578-582`, scheduled on the
single-threaded `zlink-java-channel-timeout` executor via
`scheduleInfrastructureAtFixedRate`, guarded by an in-flight `admitted` flag).

Consequences:

* Steady state: every ClientServer request reply is delivered at tick cadence.
  Measured in a passing pre-fix run (session.log, `phase=sent` →
  `phase=reply_received`): **147 ms, 101 ms, 81 ms, 104 ms** for a server that
  replies in ~35 ms — i.e. quantised to the 100 ms tick, not to the wire
  latency. After the fix the same trace reads **102 ms (first call, JIT warmup),
  then 4 ms, 17 ms, 32 ms, 4 ms, 17 ms** — the tick quantisation is gone.
* Failure mode: while that tick is delayed or skipped, no completion is drained
  at all. `NativePoller.wait` also runs `settlement.run()` →
  `Pending.awaitSettlement()`, so the tick thread blocks until the application's
  whole reply continuation chain has run; in SupportChat that chain immediately
  issues the next ClientServer request and can sit in
  `awaitClientServerTarget` (5 s ready-wait cap) — so one slow continuation
  stops the only pump for every ClientServer connection of the node.
  In g15 the `AuthenticateUserReq` reply therefore surfaced only at
  20:46:46.594, racing `ZLinkChannelCallRuntime.track`'s deadline task; the
  `outcome=succeeded` line on `[channel-timeout]` with no `flow=` is that
  losing `completeExceptionally` running the already-completed future's
  dependent stack in `postComplete()`.

This is java-only: cpp/dotnet/node do not hand the client DEALER's completion
queue to a poller that is only probed with a zero timeout. ROUTER and STREAM
wrappers keep completion ownership legitimately — they have blocking receive
owners (`ZLinkChannelReceiveLoops`/`ZLinkStreamRuntime` wait with
`RECEIVE_POLL_TIMEOUT = 250 ms`), so POLLCOMPLETION wakes them at once. SUB was
already fixed the same way in `36c310ffef`.

## Hypotheses checked and refuted

* (a) missed Core completion-queue wake / D-118/D-137/D-138 interaction — no:
  the completion is queued and is drained correctly as soon as *something*
  polls; the delay is entirely on the java side of the ownership handover.
* (b) `published`/`captured` race in `CompletionOwner.Pending` — no.
* (c) the binding's completion/pump thread runs a blocking framework
  continuation (the codex draft test
  `backendReplyCompletionDoesNotWaitForCallerContinuation`) — **refuted**: the
  SupportChat session handler is fully asynchronous
  (`SupportChatSession.authenticate` uses `thenCompose`/`thenAccept`, no join),
  and the reply is late even before any continuation runs. The draft test and
  its `businessReply` fixture hook were dropped;
  `ZLinkClientServerReadyWaitTest` is back at HEAD.
* Binding R3 commits (`4f73f6cd8d`…`a4f8cde0ef`) — not implicated; no binding
  file was changed by this fix.

## Fix

`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java`
— the DEALER readiness poller is built with `ownsCompletionQueue = false`
(POLLIN only), so completion-queue ownership stays with the binding's
per-Context `CompletionPump`, whose dedicated thread blocks in
`zlink_poller_wait` on POLLCOMPLETION and settles a reply the moment Core
queues it. `drainClientServerControls`' zero-timeout POLLIN probe for control
frames is unchanged.

No mitigation: no added timeout, retry, sleep, thread or poll. Public API
unchanged. `bindings/java/**` unchanged.

Rule count: unchanged. The socket-wrapper rule set stays at two — *a wrapper
with a blocking receive owner owns its completion queue on its own poller
(ROUTER, STREAM); a wrapper that is only probed with a zero timeout leaves the
completion queue with the binding pump (SUB, and now DEALER)*. The
`ownsCompletionQueue` seam already existed; DEALER moved to the side it belongs
on.

## Regression

`framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketReceiveOwnerTest.java`
— `dealerRequestReplyArrivesWithoutAnotherReadinessPoll`: a framework DEALER is
asked for readiness once with `Duration.ZERO` (as the control tick does), then
issues a request; the ROUTER replies and the stage must settle with **no
further readiness poll**. Verified failing on the pre-fix source
(`java.util.concurrent.TimeoutException` at the reply get) and passing with the
fix.

## Gates

* `ZLinkJavaSocketReceiveOwnerTest` — pass with fix, `dealerRequestReply…` FAILS
  without it (deliberate check).
* `framework/languages/java` `./gradlew --no-daemon test` — BUILD SUCCESSFUL, 0
  failures.
* `framework/languages/java/samples/java/SupportChat/run_sample.sh` — 5/5.
* `framework/languages/java/samples/run_samples.sh` (java + kotlin, 14) —
  "All Java/Kotlin samples passed" (exit 0). The same aggregate was run on the
  unmodified tree first as a baseline and also passed, so the sample gate is not
  the discriminating evidence; the latency trace above is.
* `bindings/java/tests/run_tests.sh` — not required: no binding file changed.

## Notes / residual

* The SupportChat failure is intermittent on this machine: the aggregate sample
  run passed 14/14 on the unmodified tree before the fix, and 5/5 standalone
  SupportChat runs passed. The deterministic evidence is the reply latency
  quantisation to the 100 ms tick, which the regression test pins directly.
* `ZLinkJavaRawServicePort` (`.../binding/ZLinkJavaRawServicePort.java:72,243`)
  keeps completion ownership on routers that are also only probed with
  `Duration.ZERO`. Its poll loop is application-driven rather than a 100 ms
  timer, and the spot/mesh path is green, so it was left alone — but it is the
  same shape and is worth a follow-up look.
