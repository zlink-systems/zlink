# 03. Messaging execution — Channel messaging

[Reference index](README.en.md)

Completion kinds, timeout rules, and codec registration are exactly the same as
[Java reference 03. Messaging execution](../../java/reference/03-messaging-execution.ko.md)
(Korean-only) — Kotlin only adds `ZLinkKotlinClient`/`ZLinkKotlinRouteClient`/
`ZLinkKotlinFanoutClient`, which wrap the same operations in coroutine shape. The exact signatures
are owned by the
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.en.md)
(Korean-only).

A Kotlin application does not use Java's `ZLinkRouteClient`/`ZLinkFanoutClient` directly — these
Kotlin-only clients and call wrappers hold the Java call internally, projecting an ordinary
completion as `await()` and a completion that returns the current Spot turn as `yield()`.

---

## `sendToChannel` / `sendToNode` (ZLinkKotlinMessageSendCall)

Sends a one-way message. `ZLinkKotlinClient.sendToChannel(...)` and
`ZLinkKotlinRouteClient.sendToChannel(...)`/`sendToNode(...)` share the same return type
`ZLinkKotlinMessageSendCall`.

```kotlin
routeClient.sendToChannel("game.api", PlayerOnline("player-1")).await()
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the handler |
| `.await()` | Required terminal, `suspend fun` | Waits only until source-local admission succeeds. The normal result is `Unit` |

**Completion result.** Same completion kinds as the Java reference's `sendToChannel`/
`sendToNode` — a failure propagates the Java stage's exception as-is.

**When to use.** Use this for fire-and-forget where no reply is needed. Use
`requestToChannel`/`requestToNode` if a reply is needed.

---

## `requestToChannel` / `requestToNode` (ZLinkKotlinRequestCall\<TReply\>)

Sends a typed request and waits for a typed reply. A reified extension lets you omit the
`KClass<TReply>` argument.

```kotlin
val reply = routeClient
    .requestToChannel<Player>("game.api", GetPlayer("player-1"))
    .timeout(Duration.ofSeconds(3))
    .await()
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Attaches only to the request |
| `.timeout(Duration)` | Same default as the Java reference | The upper bound for waiting on the reply |
| `.await()` | terminal (pick one), `suspend fun` | Waits until the reply arrives |
| `.yield()` | terminal (pick one), `suspend fun` | Only valid inside a `SPOT_WIDE` User Spot/Instance Spot handler. It is only a coroutine bridge for Java's `yield(...)` — it does not turn an arbitrary suspension into a Yield. In any other execution context, it completes with `InvalidOperation` before suspending the coroutine or submitting the operation |

`requestToChannel<TReply>(channelName, request)`/`requestToNode<TReply>(...)` are reified inline
extensions that internally pass `TReply::class` to the base overload taking `KClass<TReply>`.

**Completion result.** Same completion kinds as the Java reference's `requestToChannel`/
`requestToNode`.

**When to use.** Use this when the reply value is needed. Use `sendToChannel`/`sendToNode` if it
is one-way.

---

## `publish` (ZLinkKotlinFanoutClient, classic fanout)

Publishes a typed event to an independent fanout channel.

```kotlin
fanoutClient.publish("lobby.events", PlayerJoined("player-1")).await()

// when a topic must be specified explicitly
fanoutClient.publish("lobby.events", "region.eu", PlayerJoined("player-1")).await()
```

**Options.** The return type `ZLinkKotlinSubmissionCall` only has the `.await()` terminal.

**Completion result.** Same completion rules as the Java reference's classic fanout `publish`.
Specifying the reserved topic bytes (`01 5A 4C 46 31`) throws the Java runtime's
`ZLinkConfigurationException` as-is.

**When to use.** Same as the `publish` (classic fanout) entry in the Java reference.

---

## Common failure/cancellation rules (apply to every entry)

- If the queue is full, it waits until the send timeout. Timeout completes with
  `DeadlineExceeded`, a route disconnect with `Unavailable`, and a runtime shutdown with
  `ShuttingDown`. No target or session binding is `NotFound`.
- If coroutine cancellation is confirmed before admission, it completes as a coroutine
  cancellation and does not start admission.
- A one-way wrapper preserves FIFO queue admission and does not call the handler inline or
  reentrantly.
- Calling `yield()` from a context other than a `SPOT_WIDE` User Spot/Instance Spot application
  handler completes with `InvalidOperation` before suspending the coroutine or submitting the
  underlying operation. The same rule applies to a Node-direct request, Entry/`PER_ACTOR`,
  Channel handlers, and outside an owner context.

See the
[Kotlin channel messaging exact interface](../../common/spec/server/languages/kotlin/interfaces/channel-messaging.en.md)
and
[Java reference 03. Messaging execution](../../java/reference/03-messaging-execution.ko.md)
(Korean-only) for the full rationale.
