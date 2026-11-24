# ZLink connector examples

[한국어](README.ko.md)

Run one local WebSocket server and connect from a browser, C#, or Java. All three examples use
the same server and need Node.js 22 or later to start it.

## Server

In `server/`, run `./run-server.sh` on Linux or `run-server.cmd` on Windows (PowerShell 7
required). Wait for `READY ws://127.0.0.1:7301`, then run a client in a second terminal.
Press Ctrl+C to stop the server. The server prints the path of its log under
`server/.logs/run-*/`.

Set `CONNECTOR_EXAMPLE_BIND` before starting the server to change the address. Use the endpoint
printed by the server in the browser's endpoint field or set `CONNECTOR_EXAMPLE_ENDPOINT` before
running a terminal client.

## Clients

| Client         | Directory            | Commands                                                                                               |
| -------------- | -------------------- | ------------------------------------------------------------------------------------------------------ |
| Browser        | `client/typescript/` | Run `npm ci` and `npm run browser`, then open [localhost:8000](http://127.0.0.1:8000/). Click **Run**. |
| TypeScript E2E | `client/typescript/` | Run `npm ci` and `npm run e2e` with Node.js 22 or later.                                               |
| C#             | `client/csharp/`     | Run `dotnet run` with the .NET 8 SDK.                                                                  |
| Java           | `client/java/`       | Run `./gradlew --max-workers=2 run` with JDK 25. On Windows use `gradlew.bat --max-workers=2 run`.     |

In Unity Editor and native builds, install the pinned `Zlink.Stream.Connector` package with
NuGetForUnity and copy `ConnectorExample.cs` into the project. Call and await
`ConnectorExample.RunAsync(endpoint, line => Debug.Log(line))` from a Unity script.
See the [engine setup guide](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/)
for platform setup, including the separate WebGL integration.

## Messages

1. Connect and use one-way `send` for `Publish`. The server sends an independent `Notice` push.
2. Send `Reconnect` to have the server drop the current session. Wait for the connector to
   report its next connected state.
3. Make an `Echo` request. `score` is a signed 64-bit integer in a decimal JSON string,
   `nickname` is required and accepts `null`, and `label` may be omitted and defaults to
   `"default"`. If supplied, `label` must be a string.
4. Request `Leave`, await `Left`, then close. Closing right after `send` gives no guarantee
   that the server handled the message.

The browser sends `{"score":"9223372036854775807","nickname":null}`. The C# and Java
typed codecs encode their native `long` values as strings. The TypeScript E2E also sends
`{"score":42,"nickname":null}`. The connector reports `remoteError` with the message
`PayloadDecodeFailed: framework payload is not valid JSON.` The JSON syntax is valid, but
the typed server decoder rejects the numeric `score`. The E2E also verifies rejection when
`nickname` is missing or `label` is `null`.

[Message value rules](https://docs.zlink.systems/node/guide/server/20-channel-messaging/#12-message-value-rules)
