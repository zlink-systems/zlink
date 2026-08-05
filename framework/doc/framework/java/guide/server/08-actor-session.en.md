---
title: "8. Session And Actor Binding · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/08-actor-session.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 7. Actor And Spot](07-actor-spot.en.md) | [Next: 9. STREAM](09-stream.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/08-actor-session.en.md) · [C++](../../../cpp/guide/server/08-actor-session.en.md) · **Java** · [Kotlin](../../../kotlin/guide/server/08-actor-session.en.md) · [Node/TypeScript](../../../node/guide/server/08-actor-session.en.md)
<!-- language-switch:end -->

# 8. Session And Actor Binding

> **The documents that own this chapter's contract** —
> [Session Actor dispatch](../../../common/spec/20-session-actor-dispatch.ko.md) owns the
> behavior, and the
> [per-language STREAM session / bound session public contract](../../../common/spec/server/languages/README.ko.md)
> owns the exact signatures.

Session binding connects a client STREAM session to an exact Actor incarnation. After
binding, the session can relay a client packet to the Actor, and the Actor can push through
the same session.

Binding is independent of the Actor's Spot membership. Even when an Actor relocates to
another Spot or node, `ActorId` and `ObjectGeneration` are preserved and the Framework
updates the binding route.

**The cardinality is open on only one side.** One Session can bind several Actors at
once — one connection can use both a player Actor and a party Actor together. Conversely,
**one Actor is bound to only one session at a time.** Once a new binding is confirmed, the
previous binding becomes invalid, and a late message that arrives for it is rejected.

**Relay doesn't re-query the Location Store.** The session keeps, per Actor, the route it
confirmed at bind time, and sends using that. When an Actor moves, the Framework updates
that stored route after the relocation commits — the application doesn't rebind.

## 1. Binding An Actor After Authentication

Create or find the Actor in the Session handler, then bind the `ActorRef`. Don't pass a
local Actor instance or a target `NodeRid` directly.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the spot that receives an authentication request, creates the player Actor, binds it to
> the session, and sends the reply. Actual code from the repository.

```java
--8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/handlers/AuthenticatePlaySessionHandler.java:doc-session-auth"
```

In its minimal shape, it looks like this.

```java
public CompletionStage<Void> handle(
    ZLinkSessionContext context, ZLinkSessionDispatchContext dispatch, Authenticate request) {
    return actors
        .getOrCreate(request.playerId(), "player")
        .request(new CreatePlayer(request.displayName()))
        .submit()
        // Returns the existing route if the same exact incarnation is already bound.
        .thenCompose(result -> context.actors().bind(requireActor(result)))
        // Submits the current request's one-shot reply.
        .thenRun(() -> context.client()
            .reply(new Authenticated(request.playerId()))
            .submit());
}

private static ActorRef requireActor(ZLinkActorCreateResult result) {
    if (result instanceof ZLinkActorCreateResult.Existing existing) return existing.actor();
    if (result instanceof ZLinkActorCreateResult.Created created) return created.actor();
    throw new IllegalStateException("Player creation was rejected.");
}
```

`bind` treats a duplicate bind as an error. For a flow that might already be bound, like a
retried authentication, use `bindOrGet`.

## 2. Relaying A Session Packet To An Actor

Register session-only handlers, such as authentication, in the Session's `configure()`. An
unhandled packet is handed to the bound Actor.

```java
public final class PlaySession implements ZLinkSession {
    private final ZLinkSessionContext context;

    @Override
    public void configure() {
        // Registers the packet to handle before Actor binding.
        context.handlers().addHandler(AuthenticateHandler.class);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        return context.handlers().tryHandle(context, dispatch, payload).thenCompose(handled -> {
            if (handled) return CompletableFuture.completedFuture(null);
            // Hands the Framework-owned payload to the Actor handler without decoding it.
            return requireSingleBoundActor().relay(payload).thenApply(ignored -> null);
        });
    }

    @Override
    public CompletionStage<Void> onConnected() { return CompletableFuture.completedFuture(null); }

    @Override
    public CompletionStage<Void> onDisconnected() { return CompletableFuture.completedFuture(null); }
}
```

One session can bind several Actors. In that case, the application protocol passes the
ActorId it chose to `Context.Actors.Find(actorId)`. The Framework never picks an arbitrary
Actor on its own.

## 3. Disconnect Notification

The Framework automatically notifies every current binding on a physical STREAM disconnect.
Call it explicitly only to signal a logical disconnect while the connection stays up.

```java
ZLinkSessionActor actor = context.actors().find(playerId);
if (actor != null) {
    // Waits for the onDisconnectActor callback on the Actor's Spot to complete.
    actor.notifyDisconnected().toCompletableFuture().join();
}
```

A disconnect doesn't delete the Actor or move it to the Entry Spot. A reconnecting session
can look up the same `ActorRef` again and bind it.

**If notifying one Actor fails, the rest continue.** The Framework fixes a snapshot of the
bindings at the moment the connection drops and notifies each Actor; if one of them fails, or
a callback exceeds its deadline, it doesn't stop notifying the remaining Actors or stop
session cleanup.

**Even if the automatic notification and an explicit call overlap, the callback runs only
once.** The Framework merges two notifications for the same binding, so if the connection
drops right after an explicit call, the Spot's disconnect callback doesn't run twice.

## 4. Pushing From An Actor To The Client

An Actor handler sends a message to the currently bound client through
`Context.BoundSession`.

```java
public CompletionStage<Void> handle(
    GameRoom spot, PlayerActor actor, ZLinkMessageContext messageContext, StateChanged message) {
    // Waits for local admission on the current bound session.
    return actor.context().boundSession()
        .send(new GameStateNotify(message.state()))
        .metadata("revision", String.valueOf(message.revision()))
        .submit();
}
```

A bound session only provides push and disconnect. An Actor's reply to a client request is
handled through the request handler's return value.

## 5. Error-Handling Standard

| Situation | Result |
|---|---|
| The Actor doesn't exist or isn't Ready | The bind ends with a typed framework error. |
| `ObjectGeneration` differs | A stale ActorRef is never bound to a different incarnation. |
| An Actor relocation seal is in progress | Ends with `ActorMoving`, with no hidden retry. |
| An Actor relocates after binding | The Framework updates the route without rebinding the session. |
| Session disconnect | The Actor and its Spot membership are preserved. |
| A reply arrives after the session has closed | Discarded. Never used as the reply for a new session or a new binding. |
| A timeout/route failure after a relay | Never auto-resent to a different Actor, new owner, or different node. |

`ActorRef.MeshName` and `NodeRid` are a snapshot of the initial control route. The
application doesn't assemble a stale route on its own — it re-obtains the current ref through
the actor manager's lookup call.

## 6. Related Documents

- Runnable verification examples for this chapter's contract: `13. Interface Catalog`
  chapter §5 — the verification class `StreamContracts`
- The STREAM node and session lifecycle: [STREAM](09-stream.ko.md)
- Actor creation and Spot join: [Actor And Spot](07-actor-spot.ko.md)
