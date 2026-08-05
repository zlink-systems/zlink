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

=== "C#/.NET"

    ```csharp
    await client.Connect.Async(ct);                                    // A real connection
    var auth = await client.Request(new AuthenticateReq(actorId))      // A real request
        .Async<AuthenticateRes>(ct);
    var push = await other.WaitFor<PlayerJoinedNotify>().Async(ct);    // Confirms a real push arrived
    ZlinkStreamAssert.Ensure(push.Payload.ActorId == auth.Player.ActorId, "join push actor mismatch.");
    ```

=== "C++"

    ```cpp
    co_await client.connect ().submit ();                                     // A real connection
    auto auth = co_await client.request (authenticate_req_t{actor_id})       // A real request
                  .submit<authenticate_res_t> ();
    auto push = co_await other.wait_for<player_joined_notify_t> ().async (); // Confirms a real push arrived
    ensure (push.payload.actor_id == auth.player.actor_id);
    ```

=== "Java"

    ```java
    client.connect().submit().toCompletableFuture().join();               // A real connection
    AuthenticateRes auth = client.request(new AuthenticateReq(actorId))    // A real request
        .submit(AuthenticateRes.class).toCompletableFuture().join();
    var push = other.waitFor(PlayerJoinedNotify.class)                    // Confirms a real push arrived
        .submit(PlayerJoinedNotify.class).toCompletableFuture().join();
    ZLinkStreamAssert.ensure(
        push.payload().actorId().equals(auth.player().actorId()), "join push actor mismatch.");
    ```

=== "Kotlin"

    ```kotlin
    client.connect().submit().await()                                   // A real connection
    val auth = client.request(AuthenticateReq(actorId))                 // A real request
        .submit(AuthenticateRes::class.java).await()
    val push = other.waitFor(PlayerJoinedNotify::class.java)            // Confirms a real push arrived
        .submit(PlayerJoinedNotify::class.java).await()
    ZLinkStreamAssert.ensure(
        push.payload().actorId == auth.player.actorId, "join push actor mismatch.")
    ```

=== "Node/TypeScript"

    ```typescript
    await client.connect(signal);                                            // A real connection
    const auth = await client.request(authenticateReq(actorId))              // A real request
      .submit<AuthenticateRes>(signal);
    const push = await other.waitFor<PlayerJoinedNotify>(                    // Confirms a real push arrived
      PacketNames.playerJoinedNotify).submit(signal);
    zlinkStreamAssert.ensure(
      push.payload.actorId === auth.player.actorId, 'join push actor mismatch.');
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

=== "C#/.NET"

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

=== "C++"

    ```cpp
    // Step 1 -- create a room through the gateway API.
    auto api = zlink::http_client::client_builder_t (options.api_url)
                 .timeout (options.http_timeout)
                 .build ();
    // fetch returns the deserialized body as-is.
    auto room = api.post ("/games")
                  .body (create_game_http_req_t{options.game_name})
                  .fetch<create_game_http_res_t> ();

    // Step 2 -- open a real-time connection to the endpoint the response gave us.
    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = room.play_endpoints[0];
    connector_options.connect_timeout = options.stream_timeout;
    connector_options.request_timeout = options.stream_timeout;
    // Console scenarios use the automatic pump.
    connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto client = zlink::stream_connector::connector_factory_t::create (connector_options);
    ```

=== "Java"

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

=== "Kotlin"

    ```kotlin
    // Step 1 -- create a room through the gateway API.
    val api = ZLinkHttpClient.create(options.apiUrl)
        .timeout(options.httpTimeout)
        .build()
    // fetch returns the deserialized body as-is.
    val room = api.post("/games")
        .body(CreateGameHttpReq(options.gameName))
        .fetch(CreateGameHttpRes::class.java)

    // Step 2 -- open a real-time connection to the endpoint the response gave us.
    val client = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(room.playEndpoints[0]),
            ZLinkStreamDispatchMode.IMMEDIATE, // Console scenarios use the automatic pump.
            options.streamTimeout))
    ```

=== "Node/TypeScript"

    ```typescript
    // Step 1 -- create a room through the gateway API.
    const api = ZLinkHttpClient.create(options.apiUrl).timeout(options.httpTimeout).build();
    // fetch returns the deserialized body as-is.
    const room = await api.post('/games')
      .body(createGameHttpReq(options.gameName))
      .fetch<CreateGameHttpRes>();

    // Step 2 -- open a real-time connection to the endpoint the response gave us.
    const client = zlinkStreamConnectorFactory.create({
      endpoint: room.playEndpoints[0],
      connectTimeoutMs: options.streamTimeoutMs,
      requestTimeoutMs: options.streamTimeoutMs,
      dispatchMode: ZlinkStreamDispatchMode.Immediate // Console scenarios use the automatic pump.
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
| Confirm a request **fails** | `ExpectFailure(...)` |

**The terminal call follows the language** — `.NET` uses `Async`, Java/Node/C++ use
`submit`, Kotlin uses `await`
([Async Execution Policy](../../../common/spec/05-async-execution-policy.ko.md)).

Value comparison uses `Ensure(condition, message)`. The message is required, and on
failure the scenario ends with an exception carrying that message.

### Confirming A Push Arrives

Specify a condition with `Where(...)` to **wait for the first message matching that
condition.** Other pushes that don't match arriving in the mix doesn't affect the scenario.

=== "C#/.NET"

    ```csharp
    var joined = await client1.WaitFor<PlayerJoinedNotify>()
        .Where(message => message.Payload.ActorId == options.OActorId)
        .Async(ct);
    ZlinkStreamAssert.Ensure(joined.Payload.Mark == TicTacToeMarks.O, "joined mark mismatch.");
    ```

=== "C++"

    ```cpp
    auto joined = co_await client1.wait_for<player_joined_notify_t> ()
                    .where (&player_joined_notify_t::actor_id, options.o_actor_id)
                    .async ();
    ensure (joined.payload.mark == tictactoe_marks_t::o);
    ```

=== "Java"

    ```java
    var joined = client1.waitFor(PlayerJoinedNotify.class)
        .where(PlayerJoinedNotify.class,
            message -> message.payload().actorId().equals(options.oActorId()))
        .submit(PlayerJoinedNotify.class)
        .toCompletableFuture().join();
    ZLinkStreamAssert.ensure(joined.payload().mark() == TicTacToeMarks.O, "joined mark mismatch.");
    ```

=== "Kotlin"

    ```kotlin
    val joined = client1.waitFor(PlayerJoinedNotify::class.java)
        .where(PlayerJoinedNotify::class.java) { it.payload().actorId == options.oActorId }
        .submit(PlayerJoinedNotify::class.java)
        .await()
    ZLinkStreamAssert.ensure(joined.payload().mark == TicTacToeMarks.O, "joined mark mismatch.")
    ```

=== "Node/TypeScript"

    ```typescript
    const joined = await client1.waitFor<PlayerJoinedNotify>(PacketNames.playerJoinedNotify)
      .where((message) => message.payload.actorId === options.oActorId)
      .submit(signal);
    zlinkStreamAssert.ensure(joined.payload.mark === TicTacToeMarks.O, 'joined mark mismatch.');
    ```


### Confirming A Push Doesn't Arrive

You can't confirm something never arrives without an observation window, so `Within(...)`
must be specified. Omitting it is an error.

=== "C#/.NET"

    ```csharp
    // The player who just joined shouldn't receive their own join notification.
    await client2.ExpectNone<PlayerJoinedNotify>()
        .Within(TimeSpan.FromMilliseconds(250))
        .Async(ct);
    ```

=== "C++"

    ```cpp
    // The player who just joined shouldn't receive their own join notification.
    co_await client2.expect_none<player_joined_notify_t> ()
      .within (std::chrono::milliseconds (250))
      .async ();
    ```

=== "Java"

    ```java
    // The player who just joined shouldn't receive their own join notification.
    client2.expectNone(PlayerJoinedNotify.class)
        .within(Duration.ofMillis(250))
        .submit()
        .toCompletableFuture().join();
    ```

=== "Kotlin"

    ```kotlin
    // The player who just joined shouldn't receive their own join notification.
    client2.expectNone(PlayerJoinedNotify::class.java)
        .within(Duration.ofMillis(250))
        .submit()
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    // The player who just joined shouldn't receive their own join notification.
    await client2.expectNone<PlayerJoinedNotify>(PacketNames.playerJoinedNotify)
      .within(250)
      .run(signal);
    ```


### Confirming Push Order

In a flow where state changes in stages, the contract isn't whether something arrives but
its **order.**

=== "C#/.NET"

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

=== "C++"

    ```cpp
    auto status_sequence =
      co_await customer.wait_for_sequence<delivery_status_notify_t> ()
        .expect ([&] (const auto &m) { return m.delivery_id == delivery_id
                                              && m.status == delivery_status_t::assigned; })
        .expect ([&] (const auto &m) { return m.delivery_id == delivery_id
                                              && m.status == delivery_status_t::accepted; })
        .expect ([&] (const auto &m) { return m.delivery_id == delivery_id
                                              && m.status == delivery_status_t::picked_up; })
        .expect ([&] (const auto &m) { return m.delivery_id == delivery_id
                                              && m.status == delivery_status_t::delivered; })
        .timeout (customer.options ().wait_timeout)
        .async ();
    ```

=== "Java"

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

=== "Kotlin"

    ```kotlin
    val statusSequence = customer.waitForSequence(DeliveryStatusNotify::class.java)
        .expect(DeliveryStatusNotify::class.java) { matchesStatus(it, deliveryId, DeliveryStatus.Assigned) }
        .expect(DeliveryStatusNotify::class.java) { matchesStatus(it, deliveryId, DeliveryStatus.Accepted) }
        .expect(DeliveryStatusNotify::class.java) { matchesStatus(it, deliveryId, DeliveryStatus.PickedUp) }
        .expect(DeliveryStatusNotify::class.java) { matchesStatus(it, deliveryId, DeliveryStatus.Delivered) }
        .timeout(customer.options().waitTimeout)
        .submit(DeliveryStatusNotify::class.java)
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    const statusSequence = await customer
      .waitForSequence<DeliveryStatusNotify>(PacketNames.deliveryStatusNotify)
      .expect((message) => matchesStatus(message, deliveryId, DeliveryStatus.Assigned))
      .expect((message) => matchesStatus(message, deliveryId, DeliveryStatus.Accepted))
      .expect((message) => matchesStatus(message, deliveryId, DeliveryStatus.PickedUp))
      .expect((message) => matchesStatus(message, deliveryId, DeliveryStatus.Delivered))
      .timeout(customer.options.waitTimeoutMs)
      .submit(signal);
    ```


### Confirming A Request Fails

Whether a request with no permission or an out-of-order request **gets rejected** is also
part of the contract. Verifying only the success path leaves this path unverified.

=== "C#/.NET"

    ```csharp
    // Can't open a conversation before authenticating.
    await ZlinkStreamAssert.ExpectFailureAsync(
        async ct => _ = await agent.Request(new OpenConversationReq("unauthenticated"))
            .Async<OpenConversationRes>(ct),
        nameof(ZlinkStreamErrorCode.RemoteError));
    ```

=== "C++"

    ```cpp
    // Can't open a conversation before authenticating.
    bool failed = false;
    try {
        co_await agent.request (open_conversation_req_t{"unauthenticated"})
          .submit<open_conversation_res_t> ();
    } catch (const zlink::stream_connector::stream_error_t &error) {
        failed = error.code == zlink::stream_connector::error_code_t::remote_error;
    }
    ensure (failed);
    ```

=== "Java"

    ```java
    // Can't open a conversation before authenticating.
    ZLinkStreamAssert.expectFailure(
        () -> agent.request(new OpenConversationReq("unauthenticated"))
            .submit(OpenConversationRes.class),
        ZLinkStreamErrorCode.RemoteError);
    ```

=== "Kotlin"

    ```kotlin
    // Can't open a conversation before authenticating.
    ZLinkStreamAssert.expectFailure(
        { agent.request(OpenConversationReq("unauthenticated"))
            .submit(OpenConversationRes::class.java) },
        ZLinkStreamErrorCode.RemoteError)
    ```

=== "Node/TypeScript"

    ```typescript
    // Can't open a conversation before authenticating.
    await zlinkStreamAssert.expectFailure(
      () => agent.request(openConversationReq('unauthenticated'))
        .submit<OpenConversationRes>(signal),
      ZlinkStreamErrorCode.RemoteError
    );
    ```


## 3. How To Handle Waiting For A Message

Most E2E flakiness has the same cause. **You act first, then start waiting**, and miss a
push that arrived in between.

Reverse the order. Register the wait first, then run the action that triggers that push.

=== "C#/.NET"

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

=== "C++"

    ```cpp
    // Register the wait first -- don't co_await it yet.
    auto status_sequence_task =
      customer.wait_for_sequence<delivery_status_notify_t> ()
        .expect ([&] (const auto &m) { return m.delivery_id == delivery_id
                                              && m.status == delivery_status_t::assigned; })
        .timeout (customer.options ().wait_timeout)
        .async ();

    // Then run the action that triggers the push.
    auto created = http.post ("/deliveries")
                     .body (create_delivery_req_t{
                       delivery_id, "customer-1", "Kitchen 12", "Customer Lobby"})
                     .fetch<create_delivery_res_t> ();

    // Receive the result last.
    auto status_sequence = co_await std::move (status_sequence_task);
    ```

=== "Java"

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

=== "Kotlin"

    ```kotlin
    // Register the wait first -- don't await it yet.
    val statusSequenceDeferred = customer.waitForSequence(DeliveryStatusNotify::class.java)
        .expect(DeliveryStatusNotify::class.java) { matchesStatus(it, deliveryId, DeliveryStatus.Assigned) }
        .timeout(customer.options().waitTimeout)
        .submit(DeliveryStatusNotify::class.java)

    // Then run the action that triggers the push.
    val created = http.post("/deliveries")
        .body(CreateDeliveryReq(deliveryId, "customer-1", "Kitchen 12", "Customer Lobby"))
        .fetch(CreateDeliveryRes::class.java)

    // Receive the result last.
    val statusSequence = statusSequenceDeferred.await()
    ```

=== "Node/TypeScript"

    ```typescript
    // Register the wait first -- don't await it yet.
    const statusSequencePromise = customer
      .waitForSequence<DeliveryStatusNotify>(PacketNames.deliveryStatusNotify)
      .expect((message) => matchesStatus(message, deliveryId, DeliveryStatus.Assigned))
      .timeout(customer.options.waitTimeoutMs)
      .submit(signal);

    // Then run the action that triggers the push.
    const created = await http.post('/deliveries')
      .body(createDeliveryReq(deliveryId, 'customer-1', 'Kitchen 12', 'Customer Lobby'))
      .fetch<CreateDeliveryRes>();

    // Receive the result last.
    const statusSequence = await statusSequencePromise;
    ```


If multiple clients need to confirm the same event, register a wait for each and receive
them together with `Task.WhenAll`.

=== "C#/.NET"

    ```csharp
    // Bingo -- once both players have joined the room starts, and both clients get the same push.
    var client1StartedTask = client1.WaitFor<BingoGameStartedNotify>().Async(ct).AsTask();
    var client2StartedTask = client2.WaitFor<BingoGameStartedNotify>().Async(ct).AsTask();

    await Task.WhenAll(client1StartedTask, client2StartedTask);
    ```

=== "C++"

    ```cpp
    // Bingo -- once both players have joined the room starts, and both clients get the same push.
    auto client1_started = client1.wait_for<bingo_game_started_notify_t> ().async ();
    auto client2_started = client2.wait_for<bingo_game_started_notify_t> ().async ();

    co_await std::move (client1_started);
    co_await std::move (client2_started);
    ```

=== "Java"

    ```java
    // Bingo -- once both players have joined the room starts, and both clients get the same push.
    var client1Started = client1.waitFor(BingoGameStartedNotify.class)
        .submit(BingoGameStartedNotify.class);
    var client2Started = client2.waitFor(BingoGameStartedNotify.class)
        .submit(BingoGameStartedNotify.class);

    CompletableFuture.allOf(
        client1Started.toCompletableFuture(), client2Started.toCompletableFuture()).join();
    ```

=== "Kotlin"

    ```kotlin
    // Bingo -- once both players have joined the room starts, and both clients get the same push.
    val client1Started = client1.waitFor(BingoGameStartedNotify::class.java)
        .submit(BingoGameStartedNotify::class.java)
    val client2Started = client2.waitFor(BingoGameStartedNotify::class.java)
        .submit(BingoGameStartedNotify::class.java)

    client1Started.await()
    client2Started.await()
    ```

=== "Node/TypeScript"

    ```typescript
    // Bingo -- once both players have joined the room starts, and both clients get the same push.
    const client1Started = client1
      .waitFor<BingoGameStartedNotify>(PacketNames.gameStartedNotify).submit(signal);
    const client2Started = client2
      .waitFor<BingoGameStartedNotify>(PacketNames.gameStartedNotify).submit(signal);

    await Promise.all([client1Started, client2Started]);
    ```


Don't use `Sleep` to line up timing. Express every wait through the timeout on
`WaitFor`/`ExpectNone`/`WaitForSequence`. `Sleep` fails on slow hardware and wastes time on
fast hardware.

## 4. A Complete Scenario Example

The `TicTacToe` sample is the shortest. Create a room over HTTP → both players connect and
authenticate → confirm the join push → make a move → confirm the opponent observes that
move, in that order.

=== "C#/.NET"

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

=== "C++"

    ```cpp
    task_t<void> run (const tictactoe_client_options_t &options)
    {
        // 1. Create a room through the gateway API and get the endpoint to connect to.
        auto api = zlink::http_client::client_builder_t (options.api_url)
                     .timeout (options.http_timeout)
                     .build ();
        auto room = api.post ("/games")
                      .body (create_game_http_req_t{options.game_name})
                      .fetch<create_game_http_res_t> ();
        ensure (room.play_endpoints.size () >= 2);

        // 2. Connect the two players to different Play nodes -- this verifies routing between nodes.
        auto client1 = create_stream_client (room.play_endpoints[0], options);
        auto client2 = create_stream_client (room.play_endpoints[1], options);

        // 3. Whoever connects first authenticates and enters the empty room.
        co_await client1.connect ().submit ();
        co_await client1.request (authenticate_req_t{options.x_actor_id})
          .submit<authenticate_res_t> ();
        auto join1 = co_await join_game (client1, room.room_id); // Register wait -> send -> receive (see §3)
        ensure (join1.state.status == tictactoe_status_t::waiting_for_players);

        // Being alone in the room, their own join notification shouldn't come back to them.
        co_await client1.expect_none<player_joined_notify_t> ()
          .within (std::chrono::milliseconds (250))
          .async ();

        // 4. Once the second player joins, the room starts and a push reaches the first player.
        co_await client2.connect ().submit ();
        co_await client2.request (authenticate_req_t{options.o_actor_id})
          .submit<authenticate_res_t> ();
        auto join2 = co_await join_game (client2, room.room_id);
        ensure (join2.state.status == tictactoe_status_t::in_progress);

        // 5. Making a move -- the response and the push delivered to the opponent should point to the same state.
        auto move = co_await client1.request (place_mark_req_t{0}).submit<place_mark_res_t> ();
        auto saw_move = co_await client2.wait_for<game_state_notify_t> ()
                          .where ([] (const auto &m) { return m.state.last_move_cell == 0; })
                          .async ();
        ensure (saw_move.payload.state.board == move.state.board);
    }
    ```

=== "Java"

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

=== "Kotlin"

    ```kotlin
    suspend fun run(options: TicTacToeClientOptions) {
        // 1. Create a room through the gateway API and get the endpoint to connect to.
        val api = ZLinkHttpClient.create(options.apiUrl).timeout(options.httpTimeout).build()
        val room = api.post("/games")
            .body(CreateGameHttpReq(options.gameName))
            .fetch(CreateGameHttpRes::class.java)
        ZLinkStreamAssert.ensure(room.playEndpoints.size >= 2, "play endpoints are missing.")

        // 2. Connect the two players to different Play nodes -- this verifies routing between nodes.
        val client1 = createStreamClient(room.playEndpoints[0], options)
        val client2 = createStreamClient(room.playEndpoints[1], options)

        // 3. Whoever connects first authenticates and enters the empty room.
        client1.connect().submit().await()
        client1.request(AuthenticateReq(options.xActorId)).submit(AuthenticateRes::class.java).await()
        val join1 = joinGame(client1, room.roomId) // Register wait -> send -> receive (see §3)
        ZLinkStreamAssert.ensure(
            join1.state.status == TicTacToeGameStatuses.WaitingForPlayers,
            "room should wait for the second player.")

        // Being alone in the room, their own join notification shouldn't come back to them.
        client1.expectNone(PlayerJoinedNotify::class.java).within(Duration.ofMillis(250)).submit().await()

        // 4. Once the second player joins, the room starts.
        client2.connect().submit().await()
        client2.request(AuthenticateReq(options.oActorId)).submit(AuthenticateRes::class.java).await()
        val join2 = joinGame(client2, room.roomId)
        ZLinkStreamAssert.ensure(
            join2.state.status == TicTacToeGameStatuses.InProgress, "room should start with two players.")

        // 5. Making a move -- the response and the push delivered to the opponent should point to the same state.
        val move = client1.request(PlaceMarkReq(0)).submit(PlaceMarkRes::class.java).await()
        val sawMove = client2.waitFor(GameStateNotify::class.java)
            .where(GameStateNotify::class.java) { it.payload().state.lastMoveCell == 0 }
            .submit(GameStateNotify::class.java).await()
        ZLinkStreamAssert.ensure(sawMove.payload().state.board == move.state.board, "board state mismatch.")
    }
    ```

=== "Node/TypeScript"

    ```typescript
    async function run(options: TicTacToeClientOptions, signal: AbortSignal): Promise<void> {
      // 1. Create a room through the gateway API and get the endpoint to connect to.
      const api = ZLinkHttpClient.create(options.apiUrl).timeout(options.httpTimeout).build();
      const room = await api.post('/games')
        .body(createGameHttpReq(options.gameName))
        .fetch<CreateGameHttpRes>();
      zlinkStreamAssert.ensure(room.playEndpoints.length >= 2, 'play endpoints are missing.');

      // 2. Connect the two players to different Play nodes -- this verifies routing between nodes.
      const client1 = createStreamClient(room.playEndpoints[0], options);
      const client2 = createStreamClient(room.playEndpoints[1], options);

      // 3. Whoever connects first authenticates and enters the empty room.
      await client1.connect(signal);
      await client1.request(authenticateReq(options.xActorId)).submit<AuthenticateRes>(signal);
      const join1 = await joinGame(client1, room.roomId, signal); // Register wait -> send -> receive (see §3)
      zlinkStreamAssert.ensure(
        join1.state.status === TicTacToeGameStatuses.WaitingForPlayers,
        'room should wait for the second player.');

      // Being alone in the room, their own join notification shouldn't come back to them.
      await client1.expectNone<PlayerJoinedNotify>(PacketNames.playerJoinedNotify)
        .within(250).run(signal);

      // 4. Once the second player joins, the room starts.
      await client2.connect(signal);
      await client2.request(authenticateReq(options.oActorId)).submit<AuthenticateRes>(signal);
      const join2 = await joinGame(client2, room.roomId, signal);
      zlinkStreamAssert.ensure(
        join2.state.status === TicTacToeGameStatuses.InProgress, 'room should start with two players.');

      // 5. Making a move -- the response and the push delivered to the opponent should point to the same state.
      const move = await client1.request(placeMarkReq(0)).submit<PlaceMarkRes>(signal);
      const sawMove = await client2.waitFor<GameStateNotify>(PacketNames.gameStateNotify)
        .where((message) => message.payload.state.lastMoveCell === 0)
        .submit(signal);
      zlinkStreamAssert.ensure(sawMove.payload.state.board === move.state.board, 'board state mismatch.');
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

=== "C#/.NET"

    ```csharp
    await using var client1  = CreateStreamClient(room.PlayEndpoints[0], options, "host", logger);
    await using var client2  = CreateStreamClient(room.PlayEndpoints[1], options, "guest", logger);
    await using var observer = CreateStreamClient(room.PlayEndpoints[1], options, "observer", logger);
    ```

=== "C++"

    ```cpp
    // The join response arrives as a push, not the request's reply -- register the wait first, then send.
    task_t<join_game_res_t> join_game (auto &connector, const std::string &room_id)
    {
        auto completion = connector.wait_for<join_game_res_t> ().async ();
        co_await connector.send (join_game_req_t{room_id}).submit ();
        co_return (co_await std::move (completion)).payload;
    }
    ```

=== "Java"

    ```java
    // The join response arrives as a push, not the request's reply -- register the wait first, then send.
    private static JoinGameRes joinGame(ZLinkStreamConnector connector, String roomId) {
        var completion = connector.waitFor(JoinGameRes.class).submit(JoinGameRes.class);
        connector.send(new JoinGameReq(roomId)).submit().toCompletableFuture().join();
        return completion.toCompletableFuture().join().payload();
    }
    ```

=== "Kotlin"

    ```kotlin
    // The join response arrives as a push, not the request's reply -- register the wait first, then send.
    private suspend fun joinGame(connector: ZLinkStreamConnector, roomId: String): JoinGameRes {
        val completion = connector.waitFor(JoinGameRes::class.java).submit(JoinGameRes::class.java)
        connector.send(JoinGameReq(roomId)).submit().await()
        return completion.await().payload()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    // The join response arrives as a push, not the request's reply -- register the wait first, then send.
    async function joinGame(
      connector: ZlinkStreamConnector, roomId: string, signal: AbortSignal): Promise<JoinGameRes> {
      const completion = connector.waitFor<JoinGameRes>(PacketNames.joinGameRes).submit(signal);
      await connector.send(joinGameReq(roomId)).submit();
      return (await completion).payload;
    }
    ```


The `Bingo` sample uses this composition as-is — it keeps two players and one spectator
together, and even confirms the win notification is delivered only to the spectator.

## 6. Run Scripts And Success Criteria

The run script is responsible for **starting the server, running the client, and cleaning
up afterward.**

=== "C#/.NET"

    ```bash
    start_server play-a  ".../TicTacToe.Server.Play.dll"  "${PLAY_A_CONFIG}"
    start_server play-b  ".../TicTacToe.Server.Play.dll"  "${PLAY_B_CONFIG}"
    start_server api-a   ".../TicTacToe.Server.Api.dll"   "${API_A_CONFIG}"

    wait_port play-a "${PLAY_A_STREAM_ENDPOINT}"   # Wait until the port opens. Doesn't use sleep.

    dotnet run --no-build --project Client/TicTacToe.Client.csproj -- \
      --config "${CLIENT_CONFIG}" >"${LOG_DIR}/client.log" 2>&1

    RUN_SUCCEEDED=1
    ```

=== "C++"

    ```bash
    start_server play-a "$PLAY_BIN" --config="$CONFIG_DIR/play-a.json"
    start_server play-b "$PLAY_BIN" --config="$CONFIG_DIR/play-b.json"
    start_server api-a  "$API_BIN"  --config="$CONFIG_DIR/api-a.json"

    wait_port play-a "$PLAY_A_ROUTE_ENDPOINT"   # Wait until the port opens. Doesn't use sleep.

    "$CLIENT_BIN" --config="$CONFIG_DIR/client.json" >"$LOG_DIR/client.log" 2>&1

    RUN_SUCCEEDED=1
    ```

=== "Java"

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

=== "Kotlin"

    ```bash
    # The kotlin sample uses the same runner as java and just picks the language.
    ZLINK_SAMPLE_LANGUAGES=kotlin \
      framework/languages/java/samples/run_samples.sh TicTacToe

    # Inside the runner it's the same procedure as java -- installDist -> start_server -> wait_port -> client.
    ```

=== "Node/TypeScript"

    ```bash
    # For Node, a runner script performs the same procedure instead of shell.
    node "${SCRIPT_DIR}/../run-sample.mjs" "${SCRIPT_DIR}/Runner/sample-runner.mjs"

    # sample-runner.mjs is responsible for starting the server, waiting for the port,
    # running the client, and cleaning up.
    # The success criterion is the same as the other languages -- the client's exit code.
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
