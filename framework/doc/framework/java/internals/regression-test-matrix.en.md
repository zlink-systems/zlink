# JVM Service Runtime Regression Test Matrix

[Java Docs](../README.en.md) · [Kotlin Docs](../../kotlin/README.en.md) ·
[Runtime Lifecycle](../../common/internals/README.en.md)

Java and Kotlin verify a single JVM service runtime. The Java public ABI and the Kotlin
metadata/extension ABI are checked separately, but the protocol state machine and runtime E2E are not
implemented twice.

## 1. Contract And Module Boundary

- The Java/Kotlin exact interface and artifact ABI match.
- The JVM runtime calls only the Java binding's exported public raw socket API.
- There are zero calls to `runtime.nativeapi`, package-private members, reflection, JNI symbols, or
  Core private symbols directly.
- Core MeshNode, Spot, Actor, dispatch record, and STREAM session service types are absent from the
  public ABI and runtime dependency.
- The common protocol schema and golden fixture hash are the same across C++, .NET, JVM, and
  Node.js.

## 2. Lifecycle And Maintenance

- `ApplicationVersion` is a non-negative Java/Kotlin `long`, and target-eligibility comparison matches
  the common contract.
- The effective intent, outcome, and reason wire values of `Retire` and `Shutdown` match the common
  fixture.
- `Retire` during `Preparing`/`Error` is `Blocked/RuntimeNotReady` and does not change admission.
- `Relocate` with no `DisableRelocation` participant or no target capability is
  `Blocked/RelocationDisabled`.
- `RecreateOnRelocation` and `PreserveStateWith` use the same Location authority CAS and Relocation
  Store publication order.
- The `PreserveStateWith` adapter takes only opaque `byte[]` application state — not an owner token,
  relocation reference, or phase.
- Instance Spot's public local-only create and existing-only resolve do not start a hidden remote
  `GetOrCreate`.
- There is exactly one terminal completion in deadline, disconnect, reply, and shutdown races.

## 3. Location And Recovery

- Authority Store read and compare-exchange return the store version, lease, and store time as one
  result.
- Owner and relocation use the same authority row's 9 phases and do not create a separate relocation
  row.
- A stale owner, coordinator, and lifecycle generation do not get past a message, reply, timer, or
  phase write.
- A `missing` relocation payload and an idempotent delete are treated as a closed result.
- A 24-hour retention orphan is not mistaken for an active authority.

## 4. Transport Liveness

- The JVM runtime applies a 5-second idle probe and 15-second inbound deadline probe/ACK to
  RouteMesh/ClientServer.
- Each fanout publisher uses a dedicated SUB socket and receive loop, becomes ready on the first
  valid receive, and an idle publisher's exact two-frame beacon is not delivered to the application
  handler.
- An orderly disconnect is removed from the ready index without waiting for a timeout.
- A half-open peer becomes not-ready within 15 seconds without turning other ready peers and the host
  into `Error`.
- Reconnect performs admission again and does not reuse the previous connection's completion and
  binding state.
- The Location owner lease and the service/fanout liveness are not used with the same option or
  signal.

## 5. Java/Kotlin Public Entrypoint

- Java's `CompletionStage` and Kotlin's `await()` observe the same shared operation and terminal
  result.
- Coroutine cancellation ends only the waiter — it does not cancel the runtime operation.
- Kotlin has no separate lifecycle enum, termination wrapper, runtime facade, or relocation registry.
- An Actor factory/Snapshot-state/adapter type mismatch ends as startup validation before the socket
  binds.

## 6. E2E And Samples

- The `4 x 4` caller/server combinations across C++, .NET, JVM, and Node.js pass the common Channel,
  Spot, Actor, and STREAM scenarios.
- The JVM lane compiles the Java and Kotlin public entrypoints separately and connects each to the
  same runtime E2E.
- Samples use only the public API and contain no internal adapter or raw-frame workaround code.

## 7. Performance Smoke

At this stage, the performance test is a smoke gate confirming only that the runner-to-package
connection is executable. Numeric performance judgment, baseline comparison, hotspot analysis, and
tuning are done in a separate performance improvement effort.

- The common perf runner and JVM consumer build clean, then start and shut down normally using the
  current Framework/binding package and Core runtime.
- The minimum workload of publish, request/reply, and bidirectional send each succeeds at least once.
- There are no crashes, hangs, timeouts, or duplicate terminal completions.
- The result records runtime/package version, artifact absolute path and SHA-256, source revision,
  protocol/fixture revision, build mode, and success/failure counts.
- After shutdown, no process, thread/event-loop handle, timer, pending operation, or endpoint
  resource remains.
- Smoke numbers are not used as evidence of meeting the release performance target.
