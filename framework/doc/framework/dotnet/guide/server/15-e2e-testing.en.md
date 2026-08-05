---
title: "15. E2E Testing — Verifying The Whole System With A Client · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/15-e2e-testing.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 14. Picking A Sample — Start With The Example Closest To Your Problem](14-samples.en.md) | [Next: 16. Options — Configuration List And Defaults](16-options.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C#/.NET** · [C++](../../../cpp/guide/server/15-e2e-testing.en.md) · [Java](../../../java/guide/server/15-e2e-testing.en.md) · [Kotlin](../../../kotlin/guide/server/15-e2e-testing.en.md) · [Node/TypeScript](../../../node/guide/server/15-e2e-testing.en.md)
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

```csharp
await client.Connect.Async(ct);                                    // A real connection
var auth = await client.Request(new AuthenticateReq(actorId))      // A real request
    .Async<AuthenticateRes>(ct);
var push = await other.WaitFor<PlayerJoinedNotify>().Async(ct);    // Confirms a real push arrived
ZlinkStreamAssert.Ensure(push.Payload.ActorId == auth.Player.ActorId, "join push actor mismatch.");
```

Because **the connector itself provides the wait functions verification needs**, like
`WaitFor`, you don't implement a separate test harness. Every sample in this repository is
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
| Representative call | `Post(...).Body(...).Fetch<T>()` | `Connect` · `Request` · `WaitFor` · `ExpectNone` |

Most scenarios chain the two together — create a target over HTTP, then connect to STREAM
using the endpoint carried in that response.

```csharp
using Zlink.HttpClient;
using Systems.Zlink.Stream.Connector.Contracts;

// Step 1 -- create a room through the gateway API.
using var api = ZLinkHttpClient.Create(options.ApiUrl.ToString())
    .Timeout(options.HttpTimeout)
    .Build();
var room = await api.Post("/games")
    .Body(new CreateGameHttpReq(options.GameName))
    .Fetch<CreateGameHttpRes>(ct);   // Fetch returns the deserialized body as-is.

// Step 2 -- open a real-time connection to the endpoint the response gave us.
await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri(room.PlayEndpoints[0]),
    ConnectTimeout = options.StreamTimeout,
    RequestTimeout = options.StreamTimeout,
    DispatchMode = ZlinkStreamDispatchMode.Immediate  // Console scenarios use the automatic pump.
});
```

When `DispatchMode` is `Immediate`, the connector handles receiving on its own, so the
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
| Confirm a request **fails** | `ExpectFailureAsync(...)` |

**The terminal call follows the language** — `.NET` uses `Async`, Java/Node/C++ use
`submit`, Kotlin uses `await`
([Async Execution Policy](../../../common/spec/05-async-execution-policy.ko.md)).

Value comparison uses `Ensure(condition, message)`. The message is required, and on
failure the scenario ends with an exception carrying that message.

### Confirming A Push Arrives

Specify a condition with `Where(...)` to **wait for the first message matching that
condition.** Other pushes that don't match arriving in the mix doesn't affect the scenario.

```csharp
var joined = await client1.WaitFor<PlayerJoinedNotify>()
    .Where(message => message.Payload.ActorId == options.OActorId)
    .Async(ct);
ZlinkStreamAssert.Ensure(joined.Payload.Mark == TicTacToeMarks.O, "joined mark mismatch.");
```

### Confirming A Push Doesn't Arrive

You can't confirm something never arrives without an observation window, so `Within(...)`
must be specified. Omitting it is an error.

```csharp
// The player who just joined shouldn't receive their own join notification.
await client2.ExpectNone<PlayerJoinedNotify>()
    .Within(TimeSpan.FromMilliseconds(250))
    .Async(ct);
```

### Confirming Push Order

In a flow where state changes in stages, the contract isn't whether something arrives but
its **order.**

```csharp
var statusSequence = await customer.WaitForSequence<DeliveryStatusNotify>()
    .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Assigned }
                       && id == deliveryId)
    .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Accepted }
                       && id == deliveryId)
    .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.PickedUp }
                       && id == deliveryId)
    .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Delivered }
                       && id == deliveryId)
    .Timeout(customer.Options.WaitTimeout)
    .Async(ct);
```

### Confirming A Request Fails

Whether a request with no permission or an out-of-order request **gets rejected** is also
part of the contract. Verifying only the success path leaves this path unverified.

```csharp
// Can't open a conversation before authenticating.
await ZlinkStreamAssert.ExpectFailureAsync(
    async ct => _ = await agent.Request(new OpenConversationReq("unauthenticated"))
        .Async<OpenConversationRes>(ct),
    nameof(ZlinkStreamErrorCode.RemoteError));
```

## 3. How To Handle Waiting For A Message

Most E2E flakiness has the same cause. **You act first, then start waiting**, and miss a
push that arrived in between.

Reverse the order. Register the wait first, then run the action that triggers that push.

```csharp
// Register the wait first -- don't await it yet.
var statusSequenceTask = customer.WaitForSequence<DeliveryStatusNotify>()
    .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Assigned }
                       && id == deliveryId)
    .Timeout(customer.Options.WaitTimeout)
    .Async(ct).AsTask();

// Then run the action that triggers the push.
var created = await http.Post("/deliveries")
    .Body(new CreateDeliveryReq(deliveryId, "customer-1", "Kitchen 12", "Customer Lobby"))
    .Fetch<CreateDeliveryRes>(ct);

// Receive the result last.
var statusSequence = await statusSequenceTask;
```

If multiple clients need to confirm the same event, register a wait for each and receive
them together with `Task.WhenAll`.

```csharp
// Bingo -- once both players have joined the room starts, and both clients get the same push.
var client1StartedTask = client1.WaitFor<BingoGameStartedNotify>().Async(ct).AsTask();
var client2StartedTask = client2.WaitFor<BingoGameStartedNotify>().Async(ct).AsTask();

await Task.WhenAll(client1StartedTask, client2StartedTask);
```

Don't use `Sleep` to line up timing. Express every wait through the timeout on
`WaitFor`/`ExpectNone`/`WaitForSequence`. `Sleep` fails on slow hardware and wastes time on
fast hardware.

## 4. A Complete Scenario Example

The `TicTacToe` sample is the shortest. Create a room over HTTP → both players connect and
authenticate → confirm the join push → make a move → confirm the opponent observes that
move, in that order.

```csharp
public async ValueTask RunAsync(TicTacToeClientOptions options, CancellationToken ct = default)
{
    // 1. Create a room through the gateway API and get the endpoint to connect to.
    using var api = ZLinkHttpClient.Create(options.ApiUrl.ToString())
        .Timeout(options.HttpTimeout)
        .Build();
    var room = await api.Post("/games")
        .Body(new CreateGameHttpReq(options.GameName))
        .Fetch<CreateGameHttpRes>(ct);
    ZlinkStreamAssert.Ensure(room.PlayEndpoints.Count >= 2, "play endpoints are missing.");

    // 2. Connect the two players to different Play nodes -- this verifies routing between nodes.
    await using var client1 = CreateStreamClient(room.PlayEndpoints[0], options, "host", logger);
    await using var client2 = CreateStreamClient(room.PlayEndpoints[1], options, "guest", logger);

    // 3. Whoever connects first authenticates and enters the empty room.
    await client1.Connect.Async(ct);
    var auth1 = await client1.Request(new AuthenticateReq(options.XActorId)).Async<AuthenticateRes>(ct);
    ZlinkStreamAssert.Ensure(auth1.Player.ActorId == options.XActorId, "player x actor id mismatch.");

    var join1 = await JoinGameAsync(client1, room.RoomId, ct);   // Register wait -> send -> receive (see §3)
    ZlinkStreamAssert.Ensure(join1.State.Status == TicTacToeGameStatuses.WaitingForPlayers,
        "room should wait for the second player.");

    // Being alone in the room, their own join notification shouldn't come back to them.
    await client1.ExpectNone<PlayerJoinedNotify>()
        .Within(TimeSpan.FromMilliseconds(250))
        .Async(ct);

    // 4. Once the second player joins, the room starts and a push reaches the first player.
    await client2.Connect.Async(ct);
    await client2.Request(new AuthenticateReq(options.OActorId)).Async<AuthenticateRes>(ct);

    var join2 = await JoinGameAsync(client2, room.RoomId, ct);
    ZlinkStreamAssert.Ensure(join2.State.Status == TicTacToeGameStatuses.InProgress,
        "room should start with two players.");

    var sawJoin = await client1.WaitFor<PlayerJoinedNotify>()
        .Where(message => message.Payload.ActorId == options.OActorId)
        .Async(ct);
    ZlinkStreamAssert.Ensure(sawJoin.Payload.Mark == TicTacToeMarks.O, "second player should take O.");

    // 5. Making a move -- the response and the push delivered to the opponent should point to the same state.
    var move = await client1.Request(new PlaceMarkReq(0)).Async<PlaceMarkRes>(ct);
    ZlinkStreamAssert.Ensure(move.State.Board == "X........", "board state mismatch after the first move.");

    var sawMove = await client2.WaitFor<GameStateNotify>()
        .Where(message => message.Payload.State.LastMoveCell == 0)
        .Async(ct);
    ZlinkStreamAssert.Ensure(sawMove.Payload.State.Board == move.State.Board, "board state mismatch.");
}

// The join response arrives as a push, not the request's reply -- register the wait first, then send.
private static async ValueTask<JoinGameRes> JoinGameAsync(
    IZlinkStreamConnector connector, string roomId, CancellationToken ct)
{
    var completion = connector.WaitFor<JoinGameRes>().Async(ct);
    await connector.Send(new JoinGameReq(roomId)).Async(ct);
    return (await completion).Payload;
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

```csharp
await using var client1  = CreateStreamClient(room.PlayEndpoints[0], options, "host", logger);
await using var client2  = CreateStreamClient(room.PlayEndpoints[1], options, "guest", logger);
await using var observer = CreateStreamClient(room.PlayEndpoints[1], options, "observer", logger);
```

The `Bingo` sample uses this composition as-is — it keeps two players and one spectator
together, and even confirms the win notification is delivered only to the spectator.

## 6. Run Scripts And Success Criteria

The run script is responsible for **starting the server, running the client, and cleaning
up afterward.**

```bash
start_server play-a  ".../TicTacToe.Server.Play.dll"  "${PLAY_A_CONFIG}"
start_server play-b  ".../TicTacToe.Server.Play.dll"  "${PLAY_B_CONFIG}"
start_server api-a   ".../TicTacToe.Server.Api.dll"   "${API_A_CONFIG}"

wait_port play-a "${PLAY_A_STREAM_ENDPOINT}"   # Wait until the port opens. Doesn't use sleep.

dotnet run --no-build --project Client/TicTacToe.Client.csproj -- \
  --config "${CLIENT_CONFIG}" >"${LOG_DIR}/client.log" 2>&1

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
- **`ExpectNone` ends in an error** → `Within(...)` wasn't specified. You can't confirm
  something never arrives without an observation window, so the window is required
  explicitly.
- **`WaitFor` returns a different message** → you waited on type alone, with no condition.
  Narrow it with `Where(...)` to the event this scenario is actually waiting for.
- **It passes locally but fails only in CI** → check for leftover timing pinned with
  `sleep`. Express every wait through a wait function with a timeout specified.
- **The client passes but the server log has an error** → the script doesn't check server
  logs for errors ([§6](#6-run-scripts-and-success-criteria)).
- **It connects but the push never arrives** → in an environment that needs manual
  pumping, like engine integration, `Dispatch` wasn't run (see the Stream Connector guide).

## 8. Related Documents

- Which sample to look at first: [14-samples](14-samples.en.md)
- Server-side STREAM registration and sessions: [09-stream](09-stream.en.md)
- Full HTTP client usage: the HTTP Client guide
- Engine integration and manual pumping: the Stream Connector guide
- The connector's formal contract:
  [per-language Stream Connector public contract](../../../common/spec/stream-connector/README.en.md)
- What each sample verifies: [common sample document](../../../common/sample/README.en.md)
