# ZLink Framework for Kotlin -- Documentation

> This set is the ZLink Framework documentation for `Kotlin` (Spring Boot) users.
> `zlink-framework-kotlin` is a thin coroutine-idiom layer that reuses the Java
> `zlink-framework` runtime as-is. The Java surface follows the
> [Java spec](../common/spec/server/languages/java/README.ko.md), and the Kotlin-specific
> public contract is fixed in the [Kotlin spec](../common/spec/server/languages/kotlin/README.ko.md).
> Internal criteria are shared with the [Java/Kotlin documentation](../java/README.en.md).
> The Kotlin usage guide will be rewritten as Kotlin-specific once the 11.0 public
> interface and samples are finalized. Common meaning follows the
> [common spec](../common/README.ko.md).

The common meaning of async execution, `CompletionStage`, and the Kotlin coroutine wrapper
follows the [Async Execution And Coroutine Policy](../common/spec/05-async-execution-policy.ko.md).

Sample and E2E config files, the ban on environment variables, and
`@ConfigurationProperties`-binding criteria follow the
[Sample/E2E Configuration Policy](../common/sample-e2e-configuration-policy.en.md).

Coroutine usage for the client libraries used separately from the server framework is
found in the [HTTP client guide](guide/http-client/README.ko.md) and the
[Stream connector guide](guide/stream-connector/README.ko.md).

## 0. The Kotlin Surface At A Glance

`zlink-framework-kotlin` doesn't create a new transport. It only adds a coroutine surface
on top of the same channel/Spot/actor/stream the Java framework exposes.

| Java surface | Kotlin surface |
|-----------|-------------|
| `ZLinkRequestHandler<T, R>` (returns plain `TReply`) | `ZLinkSuspendingRequestHandler<T, R>` (`suspend fun handle`) |
| `ZLinkSendHandler` / `ZLinkFanoutHandler` | `ZLinkSuspendingSendHandler` / `ZLinkSuspendingPublishHandler` |
| `ZLinkSpot<TActor>` / `ZLinkEntrySpot<TActor>` | `ZLinkSuspendingSpot<TActor>` / `ZLinkSuspendingEntrySpot<TActor>` (handles actor admission, joined, leave as `suspend`) |
| The Java relocation policy and opaque byte adapter | `ZLinkRelocationPolicy.snapshot(Adapter::class.java)` |
| `ZLinkSession` | `ZLinkSuspendingSession` (`onConnectedSuspending`, etc.) |
| `client.requestToChannel(...).submit(R::class.java)` | `client.request<R>(channel, msg)` / `call.awaitReply<R>()` |
| The `connector.on(name) { ... }` callback | `connector.kotlin().messages(name): Flow<...>` |

The exact signature of coroutine handler configuration is owned by the Kotlin interfaces.

## 2. Public Contract Spec

Kotlin adds a coroutine extension on top of the same Spring Boot runtime. Java types used
as-is follow the Java spec, and `suspend`, `Flow`, and adapter signatures newly exposed in
Kotlin follow the Kotlin spec.

| Document | Scope |
|------|------|
| [Kotlin spec table of contents](../common/spec/server/languages/kotlin/README.ko.md) | The list of Kotlin-specific public-contract documents |
| [Kotlin interfaces](../common/spec/server/languages/kotlin/interfaces/README.ko.md) | The exact public signature for coroutines/DSL |
| [Java spec table of contents](../common/spec/server/languages/java/README.ko.md) | The Java public contract Kotlin uses as-is |
| [Java interfaces](../common/spec/server/languages/java/interfaces/README.ko.md) | The canonical Java types and builders Kotlin reuses |
| [Channel messaging](../common/spec/server/languages/java/interfaces/channel-messaging.en.md) | Channel registration, the outbound client, and dispatch |
| [Spot](../common/spec/server/languages/java/interfaces/spots.en.md) | Spot lifecycle and factory |
| [Actor](../common/spec/server/languages/java/interfaces/actors.en.md) | The actor factory, relocation adapter, and bound session |
| [STREAM](../common/spec/server/languages/java/interfaces/stream-session.en.md) | The stream node and header session |
| [stream-connector](../common/spec/stream-connector/languages/java/03-stream-connector.en.md) | The Java/Kotlin Stream Connector |
| [Location and maintenance](../common/spec/server/languages/java/interfaces/location-maintenance.en.md) | Discovery, authority, and relocation |
| [Monitoring](../common/spec/server/languages/java/interfaces/monitoring.en.md) | Runtime events and typed handlers |

## 3. Internal Criteria -- Shared With Java/Kotlin

Since they use the same runtime, implementation structure, lifecycle, and regression
criteria **share the Java/Kotlin `internals/`.**

| Document | Scope |
|------|------|
| [backend-dependency-policy](../java/internals/backend-dependency-policy.en.md) | Java binding dependency isolation |
| [Common Internals](../common/internals/README.en.md) | Runtime architecture decisions shared across all four languages |
| [regression-test-matrix](../java/internals/regression-test-matrix.en.md) | JVM contract, E2E, and performance smoke criteria |

## 4. Samples (Kotlin)

The samples provide the same scenario set as Java, implemented with Kotlin coroutines. The
6 canonical ones are per-app documents; feature-axis samples are kept as separate
documents.

The 6 canonical samples' server roles, message contracts, state transitions, and
completion criteria are owned by the [common sample](../common/sample/README.en.md). The
Kotlin documents don't restate this contract.

| Document | Scope |
|------|------|
| [samples README](../../../languages/java/samples/README.md) | Java/Kotlin sample structure and how to run them |
