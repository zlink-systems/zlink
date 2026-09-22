# 15. E2E Testing — Verifying the Whole System with a Client

!!! info "What you get from this chapter"

    You can verify registration, connection, push, and reply order in E2E scenarios with real clients.
    The verification examples in this chapter follow the sample scenarios in the language-specific example repositories.

## 1. Where E2E Testing Is Needed

No matter how tightly you write handler unit tests, some things stay unverified: whether
registration actually took effect, whether routing between two nodes is correct, whether a
push reaches other participants in the room. These can only be verified by **starting real
processes and checking over a real connection.**

At this point, teams usually implement a separate test-only client, rewriting the code to
open a socket, assemble frames, and wait for a response for every scenario. ZLink doesn't need
that work. **The client library your real users use is itself the verification tool.** An
E2E test comes down to just this much code.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:doc-e2e-connect-request"

=== "C++"

    --8<-- "framework/languages/cpp/samples/TicTacToe/Client/tictactoe_client_scenario.hpp:doc-e2e-connect-request"

=== "Java"

    --8<-- "framework/languages/java/samples/java/TicTacToe/Client/src/main/java/systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java:doc-e2e-connect-request"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt:doc-e2e-connect-request"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts:doc-e2e-connect-request"


Because **the connector itself provides the wait functions verification needs**, like
`WaitFor`, you don't implement a separate test harness. Every sample in this repository is
verified this way.

**Distinguish what E2E does and doesn't cover.** E2E confirms things like registration,
routing, push, and lifecycle — **items that only surface when multiple processes run
together.** Branches or calculations inside a handler are far faster and more precise to
verify with a unit test, so they don't belong in E2E.

## 2. The Libraries Used for Verification

The two libraries used for verification don't overlap in role.

| | `Zlink.HttpClient` | `Zlink.Stream.Connector` |
| --- | --- | --- |
| What it verifies | The management/gateway HTTP API | A STREAM server node |
| When to use it | Things that **finish with a single request-response exchange**, like creating a room, querying, or admin commands | Things that require a live connection, including confirmation of **server-initiated pushes** |
| Representative call | `Post(...).Body(...).Fetch<T>()` | `Connect` · `Request` · `WaitFor` · `ExpectNone` |

Most scenarios chain the two together — create a target over HTTP, then connect to STREAM
using the endpoint returned in that response.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:doc-e2e-create-room"

=== "C++"

    --8<-- "framework/languages/cpp/samples/TicTacToe/Client/tictactoe_client_scenario.hpp:doc-e2e-create-room"

=== "Java"

    --8<-- "framework/languages/java/samples/java/TicTacToe/Client/src/main/java/systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java:doc-e2e-create-room"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt:doc-e2e-create-room"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts:doc-e2e-create-room"


When `DispatchMode` is `Immediate`, the connector handles receiving on its own, so the
scenario code never runs a separate pump. Environments that must pump manually to match a
frame loop, like a game engine, are covered by the Stream Connector guide.

Each library's guide covers its full usage.

- The HTTP Client guide — request construction, body, auth/TLS, retry, and error handling,
  across 13 chapters
- The Stream Connector guide — per-runtime integration (Unity, Godot). Server-side STREAM
  registration is covered by [STREAM](23-stream.en.md).

## 3. Verification Functions and Usage

Most scenarios are expressed with the verification functions the connector provides.

| What's verified | Function used |
| --- | --- |
| Send a request and check the response | `Request(req)` — the response type is specified on the terminal |
| Confirm a push the server sends first arrives | `WaitFor<TNotify>()` |
| Confirm a push does **not** arrive | `ExpectNone<TNotify>().Within(window)` |
| Confirm pushes arrive in a **fixed order** | `WaitForSequence<TNotify>().Expect(...).Expect(...)` |
| Confirm a request **fails** | `ExpectFailure(...)` |

**The terminal call follows the language** — `.NET` uses `Async`, C++ uses `async`,
Java/Node use `submit`, and Kotlin uses `await`
([Async Execution Policy](../../../common/spec/server/01-execution/README.en.md)).

Value comparison uses `Ensure(condition, message)`. The message is required, and on
failure the scenario ends with an exception carrying that message.

### 3.1 Confirming a Push Arrives

Specify a condition with `Where(...)` to **wait for the first message matching that
condition.** Other, nonmatching pushes may arrive without affecting the scenario.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:doc-e2e-wait-filter"

=== "C++"

    --8<-- "framework/languages/cpp/samples/TicTacToe/Client/tictactoe_client_scenario.hpp:doc-e2e-wait-filter"

=== "Java"

    --8<-- "framework/languages/java/samples/java/TicTacToe/Client/src/main/java/systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java:doc-e2e-wait-filter"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt:doc-e2e-wait-filter"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts:doc-e2e-wait-filter"


### 3.2 Confirming a Push Doesn't Arrive

You can't confirm something never arrives without an observation window, so `Within(...)`
must be specified. Omitting it is an error.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:doc-e2e-expect-none"

=== "C++"

    --8<-- "framework/languages/cpp/samples/TicTacToe/Client/tictactoe_client_scenario.hpp:doc-e2e-expect-none"

=== "Java"

    --8<-- "framework/languages/java/samples/java/DeliveryDispatch/Client/src/main/java/systems/zlink/samples/deliverydispatch/client/DeliveryDispatchClientScenario.java:doc-e2e-expect-none"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt:doc-e2e-expect-none"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts:doc-e2e-expect-none"


### 3.3 Confirming Push Order

In a flow where state changes in stages, the contract isn't whether something arrives but
its **order.**

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Client/DeliveryDispatchClientScenario.cs:doc-e2e-sequence"

=== "C++"

    --8<-- "framework/languages/cpp/samples/DeliveryDispatch/Client/delivery_dispatch_client_scenario.hpp:doc-e2e-sequence"

=== "Java"

    --8<-- "framework/languages/java/samples/java/DeliveryDispatch/Client/src/main/java/systems/zlink/samples/deliverydispatch/client/DeliveryDispatchClientScenario.java:doc-e2e-sequence"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Client/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/client/Program.kt:doc-e2e-sequence"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/DeliveryDispatch.Ts/Client/deliverydispatch-client-scenario.ts:doc-e2e-sequence"


### 3.4 Confirming a Request Fails

Whether a request with no permission or an out-of-order request **gets rejected** is also
part of the contract. Verifying only the success path leaves this path unverified.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/SupportChat/Client/SupportChatClientScenario.cs:doc-e2e-failure"

=== "C++"

    --8<-- "framework/languages/cpp/samples/SupportChat/Client/supportchat_client_scenario.hpp:doc-e2e-failure"

=== "Java"

    --8<-- "framework/languages/java/samples/java/SupportChat/Client/src/main/java/systems/zlink/samples/supportchat/client/Program.java:doc-e2e-failure"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/SupportChat/Client/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/client/SupportChatClientScenario.kt:doc-e2e-failure"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/SupportChat.Ts/Client/supportchat-client-scenario.ts:doc-e2e-failure"


## 4. How to Handle Waiting for a Message

Most E2E flakiness has the same cause. **You act first, then start waiting**, and miss a
push that arrived in between.

Reverse the order. Register the wait first, then run the action that triggers that push.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/DeliveryDispatch/Client/DeliveryDispatchClientScenario.cs:doc-e2e-sequence"

=== "C++"

    --8<-- "framework/languages/cpp/samples/DeliveryDispatch/Client/delivery_dispatch_client_scenario.hpp:doc-e2e-sequence"

=== "Java"

    --8<-- "framework/languages/java/samples/java/DeliveryDispatch/Client/src/main/java/systems/zlink/samples/deliverydispatch/client/DeliveryDispatchClientScenario.java:doc-e2e-sequence"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Client/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/client/Program.kt:doc-e2e-sequence"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/DeliveryDispatch.Ts/Client/deliverydispatch-client-scenario.ts:doc-e2e-sequence"


If multiple clients need to confirm the same event, register a wait for each and receive
them together with `Task.WhenAll`.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/Bingo/Client/BingoClientScenario.cs:doc-e2e-multi-wait"

=== "C++"

    --8<-- "framework/languages/cpp/samples/Bingo/Client/bingo_client_scenario.hpp:doc-e2e-multi-wait"

=== "Java"

    --8<-- "framework/languages/java/samples/java/Bingo/Client/src/main/java/systems/zlink/samples/bingo/client/BingoClientScenario.java:doc-e2e-multi-wait"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/Bingo/Client/src/main/kotlin/systems/zlink/samples/kotlin/bingo/client/BingoClientScenario.kt:doc-e2e-multi-wait"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/Bingo.Ts/Client/bingo-client-scenario.ts:doc-e2e-multi-wait"


Don't use `Sleep` to line up timing. Express every wait through the timeout on
`WaitFor`/`ExpectNone`/`WaitForSequence`. `Sleep` fails on slow hardware and wastes time on
fast hardware.

## 5. A Complete Scenario Example

The `TicTacToe` sample is the shortest. Create a room over HTTP → both players connect and
authenticate → confirm the join push → make a move → confirm the opponent observes that
move, in that order.

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:doc-e2e-scenario"

=== "C++"

    --8<-- "framework/languages/cpp/samples/TicTacToe/Client/tictactoe_client_scenario.hpp:doc-e2e-scenario"

=== "Java"

    --8<-- "framework/languages/java/samples/java/TicTacToe/Client/src/main/java/systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java:doc-e2e-scenario"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt:doc-e2e-scenario"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts:doc-e2e-scenario"


**Choose verification points by this rule.** Don't just check a request's own response —
also check *whether another client observes the same fact.* Making **the result that
actually reaches the user**, not server-internal state, the contract, is the point of E2E.

## 6. Verifying with Multiple Clients

A single scenario can create several clients. Splitting roles verifies contracts a single
client can't confirm.

- **Two players** — whether one's action reaches the other, and conversely, that it
  **doesn't reach themselves**
- **A spectator** — whether a notification reaches a non-participant connection, and
  conversely, that a participant-only notification doesn't
- **Two connected to different nodes** — whether routing and location resolution between
  nodes actually work

=== "C#/.NET"

    --8<-- "framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:doc-e2e-multi-client"

=== "C++"

    --8<-- "framework/languages/cpp/samples/TicTacToe/Client/tictactoe_client_scenario.hpp:doc-e2e-multi-client"

=== "Java"

    --8<-- "framework/languages/java/samples/java/TicTacToe/Client/src/main/java/systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java:doc-e2e-multi-client"

=== "Kotlin"

    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt:doc-e2e-multi-client"

=== "Node/TypeScript"

    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts:doc-e2e-multi-client"


The `Bingo` sample uses this composition as-is — it brings together two players and one
spectator, and even confirms the win notification is delivered only to the spectator.

## 7. Run Scripts and Success Criteria

The run script is responsible for **starting the server, running the client, and cleaning
up afterward.**

=== "C#/.NET"

    ```bash
    start_server play-a  ".../TicTacToe.Server.Play.dll"  "${PLAY_A_CONFIG}"
    start_server play-b  ".../TicTacToe.Server.Play.dll"  "${PLAY_B_CONFIG}"
    start_server api-a   ".../TicTacToe.Server.Api.dll"   "${API_A_CONFIG}"

    # Wait until the port opens. Doesn't use sleep.
    wait_port play-a "${PLAY_A_STREAM_ENDPOINT}"

    dotnet run --no-build --project Client/TicTacToe.Client.csproj -- \
      --config "${CLIENT_CONFIG}" >"${LOG_DIR}/client.log" 2>&1

    RUN_SUCCEEDED=1
    ```

=== "C++"

    ```bash
    start_server play-a "$PLAY_BIN" --config="$CONFIG_DIR/play-a.json"
    start_server play-b "$PLAY_BIN" --config="$CONFIG_DIR/play-b.json"
    start_server api-a  "$API_BIN"  --config="$CONFIG_DIR/api-a.json"

    # Wait until the port opens. Doesn't use sleep.
    wait_port play-a "$PLAY_A_ROUTE_ENDPOINT"

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

    # Wait until the port opens. Doesn't use sleep.
    wait_port "${PLAY_A_ROUTE_ENDPOINT}"

    "$(app_bin Client Client)" --api-url "http://127.0.0.1:${api_a_http_port}" \
      >"${log_dir}/client.log" 2>&1

    RUN_SUCCEEDED=1
    ```

=== "Kotlin"

    ```bash
      framework/languages/java/samples/kotlin/TicTacToe/run_sample.sh

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
- **Clean up with `trap`** so that a scenario failing partway through doesn't leave started
  processes, temp directories, or containers behind to affect the next run.

Even if the client passes, also check that **the server logs have no errors.** Sometimes
the client observes normal behavior while the server records a dispatch error.

```bash
if grep -R -q "dispatch-error" "${LOG_DIR}"; then
  echo "Unexpected dispatch-error in sample logs." >&2
  exit 1
fi
```

## 8. Common Problems

- **A push isn't received, causing intermittent failure** → check that the wait was
  registered before the action ([How to Handle Waiting for a Message](#4-how-to-handle-waiting-for-a-message)). Starting
  the wait afterward misses a push that arrived in between.
- **`ExpectNone` ends in an error** → `Within(...)` wasn't specified. You can't confirm
  something never arrives without an observation window, so the window is required
  explicitly.
- **`WaitFor` returns a different message** → you waited on type alone, with no condition.
  Narrow it with `Where(...)` to the event this scenario is actually waiting for.
- **It passes locally but fails only in CI** → check for remaining timing dependencies
  implemented with `sleep`. Express every wait through a wait function with an explicit timeout.
- **The client passes but the server log has an error** → the script doesn't check server
  logs for errors ([Run Scripts and Success Criteria](#7-run-scripts-and-success-criteria)).
- **It connects but the push never arrives** → in an environment that needs manual
  pumping, like engine integration, `Dispatch` wasn't run (see the Stream Connector guide).

## 9. Related Documents

- Which sample to look at first: [14-samples](14-samples.en.md)
- Server-side STREAM registration and sessions: [STREAM](23-stream.en.md)
- Full HTTP client usage: the HTTP Client guide
- Engine integration and manual pumping: the Stream Connector guide
- The connector's formal contract:
  [per-language Stream Connector public contract](../../../common/spec/stream-connector/README.en.md)
- What each sample verifies: [common sample document](../../../common/sample/README.en.md)
