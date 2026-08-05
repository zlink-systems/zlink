---
title: "15. E2E Testing — Verifying The Whole System With A Client · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/15-e2e-testing.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 14. Picking A Sample — Start With The Example Closest To Your Problem](14-samples.en.md) | [Next: 16. Options — Setting List And Defaults](16-options.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/15-e2e-testing.en.md) · **C++** · [Java](../../../java/guide/server/15-e2e-testing.en.md) · [Kotlin](../../../kotlin/guide/server/15-e2e-testing.en.md) · [Node/TypeScript](../../../node/guide/server/15-e2e-testing.en.md)
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

```cpp
co_await client.connect ().submit ();                                     // A real connection
auto auth = co_await client.request (authenticate_req_t{actor_id})       // A real request
              .submit<authenticate_res_t> ();
auto push = co_await other.wait_for<player_joined_notify_t> ().async (); // Confirms a real push arrived
ensure (push.payload.actor_id == auth.player.actor_id);
```

Because **the connector itself provides the wait functions verification needs**, like
`wait_for`, you don't implement a separate test harness. Every sample in this repository is
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
| Representative call | `Post(...).Body(...).Fetch<T>()` | `connect` · `request` · `wait_for` · `ExpectNone` |

Most scenarios chain the two together — create a target over HTTP, then connect to STREAM
using the endpoint carried in that response.

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

```cpp
auto joined = co_await client1.wait_for<player_joined_notify_t> ()
                .where (&player_joined_notify_t::actor_id, options.o_actor_id)
                .async ();
ensure (joined.payload.mark == tictactoe_marks_t::o);
```

### Confirming A Push Doesn't Arrive

You can't confirm something never arrives without an observation window, so `Within(...)`
must be specified. Omitting it is an error.

```cpp
// The player who just joined shouldn't receive their own join notification.
co_await client2.expect_none<player_joined_notify_t> ()
  .within (std::chrono::milliseconds (250))
  .async ();
```

### Confirming Push Order

In a flow where state changes in stages, the contract isn't whether something arrives but
its **order.**

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

### Confirming A Request Fails

Whether a request with no permission or an out-of-order request **gets rejected** is also
part of the contract. Verifying only the success path leaves this path unverified.

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

## 3. How To Handle Waiting For A Message

Most E2E flakiness has the same cause. **You act first, then start waiting**, and miss a
push that arrived in between.

Reverse the order. Register the wait first, then run the action that triggers that push.

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

If multiple clients need to confirm the same event, register a wait for each and receive
them together with `Task.WhenAll`.

```cpp
// Bingo -- once both players have joined the room starts, and both clients get the same push.
auto client1_started = client1.wait_for<bingo_game_started_notify_t> ().async ();
auto client2_started = client2.wait_for<bingo_game_started_notify_t> ().async ();

co_await std::move (client1_started);
co_await std::move (client2_started);
```

Don't use `Sleep` to line up timing. Express every wait through the timeout on
`wait_for`/`ExpectNone`/`WaitForSequence`. `Sleep` fails on slow hardware and wastes time on
fast hardware.

## 4. A Complete Scenario Example

The `TicTacToe` sample is the shortest. Create a room over HTTP → both players connect and
authenticate → confirm the join push → make a move → confirm the opponent observes that
move, in that order.

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

```cpp
// The join response arrives as a push, not the request's reply -- register the wait first, then send.
task_t<join_game_res_t> join_game (auto &connector, const std::string &room_id)
{
    auto completion = connector.wait_for<join_game_res_t> ().async ();
    co_await connector.send (join_game_req_t{room_id}).submit ();
    co_return (co_await std::move (completion)).payload;
}
```

The `Bingo` sample uses this composition as-is — it keeps two players and one spectator
together, and even confirms the win notification is delivered only to the spectator.

## 6. Run Scripts And Success Criteria

The run script is responsible for **starting the server, running the client, and cleaning
up afterward.**

```bash
start_server play-a "$PLAY_BIN" --config="$CONFIG_DIR/play-a.json"
start_server play-b "$PLAY_BIN" --config="$CONFIG_DIR/play-b.json"
start_server api-a  "$API_BIN"  --config="$CONFIG_DIR/api-a.json"

wait_port play-a "$PLAY_A_ROUTE_ENDPOINT"   # Wait until the port opens. Doesn't use sleep.

"$CLIENT_BIN" --config="$CONFIG_DIR/client.json" >"$LOG_DIR/client.log" 2>&1

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
- **`wait_for` returns a different message** → you waited on type alone, with no condition.
  Narrow it with `Where(...)` to the event this scenario is actually waiting for.
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
