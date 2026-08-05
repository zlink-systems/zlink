---
title: "15. E2E Testing — Verifying The Whole System With A Client · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/15-e2e-testing.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 14. Picking A Sample — Start With The Example Closest To Your Problem](14-samples.en.md) | [Next: 16. Options — Setting List And Defaults](16-options.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/15-e2e-testing.en.md) · [C++](../../../cpp/guide/server/15-e2e-testing.en.md) · **Java** · [Kotlin](../../../kotlin/guide/server/15-e2e-testing.en.md) · [Node/TypeScript](../../../node/guide/server/15-e2e-testing.en.md)
<!-- language-switch:end -->

# 15. E2E Testing — Verifying The Whole System With A Client

> **This chapter has no spec document that owns its contract.** That's because it covers
> how to build tests in your own system. What each sample verifies is defined by the
> [common sample document](../../../common/sample/README.en.md). The connector's formal API
> surface is owned by the
> [per-language Stream Connector public contract](../../../common/spec/stream-connector/README.en.md).
> This chapter covers **how to build E2E tests in your own system.**

## 0. Where E2E Testing Is Needed

No matter how tightly you write handler unit tests, some things stay unverified: whether
registration actually took effect, whether routing between two nodes is correct, whether a
push reaches other participants in the room. These can only be judged by **starting real
processes and checking over a real connection.**

At this point, teams usually implement a separate test-only client — opening a socket,
assembling frames, waiting for a response, rewritten for every scenario. ZLink doesn't need
that work. **The client library your real users use is itself the verification tool.** An
E2E test comes down to just this much code.

```java
client.connect().submit().toCompletableFuture().join();               // A real connection
AuthenticateRes auth = client.request(new AuthenticateReq(actorId))    // A real request
    .submit(AuthenticateRes.class).toCompletableFuture().join();
var push = other.waitFor(PlayerJoinedNotify.class)                    // Confirms a real push arrived
    .submit(PlayerJoinedNotify.class).toCompletableFuture().join();
ZLinkStreamAssert.ensure(
    push.payload().actorId().equals(auth.player().actorId()), "join push actor mismatch.");
```

Because **the connector itself provides the wait functions verification needs**, like
`waitFor`, you don't implement a separate test harness. Every sample in this repository is
verified this way.

**Distinguish what E2E does and doesn't cover.** E2E confirms things like registration,
routing, push, and lifecycle — **items that only surface when multiple processes run
together.** Branches or calculations inside a handler are far faster and more precise to
verify with a unit test, so they don't belong in E2E.

## 1. The Libraries Used For Verification

The two libraries used for verification don't overlap in role.

| | `Zlink.HttpClient` | `Systems.Zlink.Stream.Connector` |
| --- | --- | --- |
| What it verifies | The management/gateway HTTP API | A STREAM server node |
| When to use it | Things that **finish with one request-response**, like creating a room, querying, or admin commands | Things that need a live connection and confirming even **a push the server sends first** |
| Representative call | `Post(...).Body(...).Fetch<T>()` | `connect` · `request` · `waitFor` · `expectNone` |

Most scenarios chain the two together — create a target over HTTP, then connect to STREAM
using the endpoint carried in that response.

```java
// Step 1 -- create a room through the gateway API.
ZLinkHttpClient api = ZLinkHttpClient.create(options.apiUrl())
    .timeout(options.httpTimeout())
    .build();
// fetch returns the deserialized body as-is.
CreateGameHttpRes room = api.post("/games")
    .body(new CreateGameHttpReq(options.gameName()))
    .fetch(CreateGameHttpRes.class);

// Step 2 -- open a real-time connection to the endpoint the response gave us.
ZLinkStreamConnector client = ZLinkStreamConnectorFactory.create(
    new ZLinkStreamConnectorOptions(
        URI.create(room.playEndpoints().get(0)),
        ZLinkStreamDispatchMode.IMMEDIATE, // Console scenarios use the automatic pump.
        options.streamTimeout()));
```

When `dispatchMode` is `Immediate`, the connector handles receiving on its own, so the
scenario code never runs a separate pump. Environments that must pump manually to match a
frame loop, like a game engine, are covered by the Stream Connector guide.

Full usage for both libraries is owned by their own guides.

- The HTTP Client guide — request construction, body, auth/TLS, retry, and error handling,
  across 13 chapters
- The Stream Connector guide — per-runtime integration (Unity, Godot). Server-side STREAM
  registration is covered by [09-stream](09-stream.en.md).

## 2. Verification Functions And Usage

Most scenarios are expressed with the verification functions the connector provides.

| What's verified | Function used |
| --- | --- |
| Send a request and check the response | `Request(req)` — the response type is specified on the terminal |
| Confirm a push the server sends first arrives | `WaitFor<TNotify>()` |
| Confirm a push does **not** arrive | `ExpectNone<TNotify>().Within(window)` |
| Confirm pushes arrive in a **fixed order** | `WaitForSequence<TNotify>().Expect(...).Expect(...)` |
| Confirm a request **fails** | `expectFailure(...)` |

**The terminal call follows the language** — `.NET` uses `Async`, Java/Node/C++ use
`submit`, Kotlin uses `await`
([Async Execution Policy](../../../common/spec/05-async-execution-policy.ko.md)).

Value comparison uses `Ensure(condition, message)`. The message is required, and on
failure the scenario ends with an exception carrying that message.

### Confirming A Push Arrives

Specify a condition with `where(...)` to **wait for the first message matching that
condition.** Other pushes that don't match arriving in the mix doesn't affect the scenario.

```java
var joined = client1.waitFor(PlayerJoinedNotify.class)
    .where(PlayerJoinedNotify.class,
        message -> message.payload().actorId().equals(options.oActorId()))
    .submit(PlayerJoinedNotify.class)
    .toCompletableFuture().join();
ZLinkStreamAssert.ensure(joined.payload().mark() == TicTacToeMarks.O, "joined mark mismatch.");
```

### Confirming A Push Doesn't Arrive

You can't confirm something never arrives without an observation window, so `within(...)`
must be specified. Omitting it is an error.

```java
// The player who just joined shouldn't receive their own join notification.
client2.expectNone(PlayerJoinedNotify.class)
    .within(Duration.ofMillis(250))
    .submit()
    .toCompletableFuture().join();
```

### Confirming Push Order

In a flow where state changes in stages, the contract isn't whether something arrives but
its **order.**

```java
var statusSequence = customer.waitForSequence(DeliveryStatusNotify.class)
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Assigned))
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Accepted))
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.PickedUp))
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Delivered))
    .timeout(customer.options().waitTimeout())
    .submit(DeliveryStatusNotify.class)
    .toCompletableFuture().join();
```

### Confirming A Request Fails

Whether a request with no permission or an out-of-order request **gets rejected** is also
part of the contract. Verifying only the success path leaves this path unverified.

```java
// Can't open a conversation before authenticating.
ZLinkStreamAssert.expectFailure(
    () -> agent.request(new OpenConversationReq("unauthenticated"))
        .submit(OpenConversationRes.class),
    ZLinkStreamErrorCode.RemoteError);
```

## 3. How To Handle Waiting For A Message

Most E2E flakiness has the same cause. **You act first, then start waiting**, and miss a
push that arrived in between.

Reverse the order. Register the wait first, then run the action that triggers that push.

```java
// Register the wait first -- don't join it yet.
var statusSequenceStage = customer.waitForSequence(DeliveryStatusNotify.class)
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Assigned))
    .timeout(customer.options().waitTimeout())
    .submit(DeliveryStatusNotify.class);

// Then run the action that triggers the push.
CreateDeliveryRes created = http.post("/deliveries")
    .body(new CreateDeliveryReq(deliveryId, "customer-1", "Kitchen 12", "Customer Lobby"))
    .fetch(CreateDeliveryRes.class);

// Receive the result last.
var statusSequence = statusSequenceStage.toCompletableFuture().join();
```

If multiple clients need to confirm the same event, register a wait for each and receive
them together with `Task.WhenAll`.

```java
// Bingo -- once both players have joined the room starts, and both clients get the same push.
var client1Started = client1.waitFor(BingoGameStartedNotify.class)
    .submit(BingoGameStartedNotify.class);
var client2Started = client2.waitFor(BingoGameStartedNotify.class)
    .submit(BingoGameStartedNotify.class);

CompletableFuture.allOf(
    client1Started.toCompletableFuture(), client2Started.toCompletableFuture()).join();
```

Don't use `Sleep` to line up timing. Express every wait through the timeout on
`waitFor`/`expectNone`/`waitForSequence`. `Sleep` fails on slow hardware and wastes time on
fast hardware.

## 4. A Complete Scenario Example

The `TicTacToe` sample is the shortest. Create a room over HTTP → both players connect and
authenticate → confirm the join push → make a move → confirm the opponent observes that
move, in that order.

```java
public void run(TicTacToeClientOptions options) {
    // 1. Create a room through the gateway API and get the endpoint to connect to.
    ZLinkHttpClient api = ZLinkHttpClient.create(options.apiUrl())
        .timeout(options.httpTimeout()).build();
    CreateGameHttpRes room = api.post("/games")
        .body(new CreateGameHttpReq(options.gameName()))
        .fetch(CreateGameHttpRes.class);
    ZLinkStreamAssert.ensure(room.playEndpoints().size() >= 2, "play endpoints are missing.");

    // 2. Connect the two players to different Play nodes -- this verifies routing between nodes.
    ZLinkStreamConnector client1 = createStreamClient(room.playEndpoints().get(0), options);
    ZLinkStreamConnector client2 = createStreamClient(room.playEndpoints().get(1), options);

    // 3. Whoever connects first authenticates and enters the empty room.
    client1.connect().submit().toCompletableFuture().join();
    client1.request(new AuthenticateReq(options.xActorId()))
        .submit(AuthenticateRes.class).toCompletableFuture().join();
    JoinGameRes join1 = joinGame(client1, room.roomId()); // Register wait -> send -> receive (see §3)
    ZLinkStreamAssert.ensure(
        join1.state().status() == TicTacToeGameStatuses.WaitingForPlayers,
        "room should wait for the second player.");

    // Being alone in the room, their own join notification shouldn't come back to them.
    client1.expectNone(PlayerJoinedNotify.class)
        .within(Duration.ofMillis(250)).submit().toCompletableFuture().join();

    // 4. Once the second player joins, the room starts.
    client2.connect().submit().toCompletableFuture().join();
    client2.request(new AuthenticateReq(options.oActorId()))
        .submit(AuthenticateRes.class).toCompletableFuture().join();
    JoinGameRes join2 = joinGame(client2, room.roomId());
    ZLinkStreamAssert.ensure(
        join2.state().status() == TicTacToeGameStatuses.InProgress,
        "room should start with two players.");

    // 5. Making a move -- the response and the push delivered to the opponent should point to the same state.
    PlaceMarkRes move = client1.request(new PlaceMarkReq(0))
        .submit(PlaceMarkRes.class).toCompletableFuture().join();
    var sawMove = client2.waitFor(GameStateNotify.class)
        .where(GameStateNotify.class, message -> message.payload().state().lastMoveCell() == 0)
        .submit(GameStateNotify.class).toCompletableFuture().join();
    ZLinkStreamAssert.ensure(
        sawMove.payload().state().board().equals(move.state().board()), "board state mismatch.");
}
```

**Choose verification points by this rule.** Don't just check a request's own response —
also check *whether another client observes the same fact.* Making **the result that
actually reaches the user**, not server-internal state, the contract, is the point of E2E.

## 5. Verifying With Multiple Clients

A single scenario can create several clients. Splitting roles verifies contracts a single
client can't confirm.

- **Two players** — whether one's action reaches the other, and conversely, that it
  **doesn't reach themselves**
- **A spectator** — whether a notification reaches a non-participant connection, and
  conversely, that a participant-only notification doesn't
- **Two connected to different nodes** — whether routing and location resolution between
  nodes actually work

```java
// The join response arrives as a push, not the request's reply -- register the wait first, then send.
private static JoinGameRes joinGame(ZLinkStreamConnector connector, String roomId) {
    var completion = connector.waitFor(JoinGameRes.class).submit(JoinGameRes.class);
    connector.send(new JoinGameReq(roomId)).submit().toCompletableFuture().join();
    return completion.toCompletableFuture().join().payload();
}
```

The `Bingo` sample uses this composition as-is — it keeps two players and one spectator
together, and even confirms the win notification is delivered only to the spectator.

## 6. Run Scripts And Success Criteria

The run script is responsible for **starting the server, running the client, and cleaning
up afterward.**

```bash
# gradle builds a runnable distribution, and the script launches that executable.
gradle_run :Server:installDist :Client:installDist

start_server play-a "$(app_bin Server Server)" --config "${CONFIG_DIR}/play-a.json"
start_server play-b "$(app_bin Server Server)" --config "${CONFIG_DIR}/play-b.json"
start_server api-a  "$(app_bin Server Server)" --config "${CONFIG_DIR}/api-a.json"

wait_port "${PLAY_A_ROUTE_ENDPOINT}"        # Wait until the port opens. Doesn't use sleep.

"$(app_bin Client Client)" --api-url "http://127.0.0.1:${api_a_http_port}" \
  >"${log_dir}/client.log" 2>&1

RUN_SUCCEEDED=1
```

The script follows these rules.

- **The client's exit code is the success criterion.** Under `set -e`, if the client exits
  with an exception, the script fails at that point too. No separate judgment logic is
  implemented.
- **Wait on a condition, not `sleep`.** Server startup is confirmed by whether the port is
  open; async post-processing, by whether a specific line appeared in the log.
- **Clean up with `trap`.** So that a scenario failing partway through doesn't leave
  started processes, temp directories, or containers behind to affect the next run.

Even if the client passes, also check that **the server logs have no errors.** Sometimes
the client observes normal behavior while the server records a dispatch error.

```bash
if grep -R -q "dispatch-error" "${LOG_DIR}"; then
  echo "Unexpected dispatch-error in sample logs." >&2
  exit 1
fi
```

## 7. Common Problems

- **A push isn't received, causing intermittent failure** → check that the wait was
  registered before the action ([§3](#3-how-to-handle-waiting-for-a-message)). Starting
  the wait afterward misses a push that arrived in between.
- **`expectNone` ends in an error** → `within(...)` wasn't specified. You can't confirm
  something never arrives without an observation window, so the window is required
  explicitly.
- **`waitFor` returns a different message** → you waited on type alone, with no condition.
  Narrow it with `where(...)` to the event this scenario is actually waiting for.
- **It passes locally but fails only in CI** → check for leftover timing pinned with
  `sleep`. Express every wait through a wait function with a timeout specified.
- **The client passes but the server log has an error** → the script doesn't check server
  logs for errors ([§6](#6-run-scripts-and-success-criteria)).
- **It connects but the push never arrives** → in an environment that needs manual
  pumping, like engine integration, `dispatch` wasn't run (see the Stream Connector guide).

## 8. Related Documents

- Which sample to look at first: [14-samples](14-samples.en.md)
- Server-side STREAM registration and sessions: [09-stream](09-stream.en.md)
- Full HTTP client usage: the HTTP Client guide
- Engine integration and manual pumping: the Stream Connector guide
- The connector's formal contract:
  [per-language Stream Connector public contract](../../../common/spec/stream-connector/README.en.md)
- What each sample verifies: [common sample document](../../../common/sample/README.en.md)
