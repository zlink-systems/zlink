**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Cocos Creator sample

This Cocos Creator web client connects to the shared Engine Lobby server. `EngineLobbyClient`
performs `PingReq` → `PingRes` → `JoinReq` → `JoinRes` → `ChatMsg` and receives `ChatNotify`.
`EngineLobbyBehaviour` pumps connector callbacks from Cocos `update()` and displays status in a
Label. Packet names and JSON fields follow the [shared engine-lobby contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md).

## Target and transport

The execution target is **Cocos Creator 3.8 web**. The project uses the package root of
`@zlink-systems/stream-connector` `0.22.0` with `ws://` or `wss://` endpoints. The browser's
`WebSocket` handles the connection and framing; the connector's built-in `zlinkStreamJsonCodec`
handles typed payloads. `Manual` dispatch runs callbacks from Cocos `update()`.

The TypeScript connector's supported product environment is the browser family. Although Cocos
Creator native provides a `WebSocket` API, the contract does not assign this package to native
builds. The common connector spec assigns Cocos native to the **C++ Axmol adapter**, which is not
a package that can be installed directly into a Cocos Creator native project. This project does
not claim native build or player support. Native support needs a separate Creator adapter contract
and implementation.

## Download and install

Node.js 22 and npm are required. Running the scene also requires Cocos Creator 3.8 Editor.

```bash
npm install
```

## Typecheck

Check the engine-independent client class against the connector package declarations without
the Editor.

```bash
npm run typecheck
```

This compiles `EngineLobbyClient.ts` only. The Editor must verify the `cc` import in
`EngineLobbyBehaviour.ts` and scene serialization.

## Protobuf messaging example

This Engine Lobby sample's server and client use JSON. A client example for a Protobuf server is
provided separately in the [StreamClient Protobuf tutorial](../../node/tutorial/StreamClient/README.en.md).
It receives generated `Ping` and `Pong` pushes through one codec and checks the `rank` in a `Pong` reply. `npm run protobuf:check` runs it with a verification WebSocket peer and prints
`protobuf: Ping=hello, Pong.rank=3, reply.rank=7`.

The [Node Protobuf messaging guide](../../../doc/framework/node/guide/stream-connector/40-protobuf.en.md)
explains code generation and codec configuration. Browser clients use the package root of
`@zlink-systems/framework-codec-protobuf`, with matching connector and codec package versions.
Both dependencies are pinned to `0.28.0`; this verification uses the local connector and codec fixes from #1503. The server must also send Protobuf using the same schema and
packet names; changing only the client codec does not adapt the existing JSON server.
This is not a verification result for Protobuf in the Cocos scene.

Pass the generated reply class to `submit(Pong)`. For callback delivery, use `submitCallback(Pong, callback)`.
The type argument in `submit<Pong>()` alone does not pass a reply class to the decoder.
[Protobuf codecs and types](../../../doc/framework/node/guide/stream-connector/41-protobuf-codecs.en.md)
explains selecting multiple receiving types through handler constructors.

## Run the server

Use `../Server` in the monorepo, or prepare the `zlink-engine-server` mirror. Follow its README
install, build, and run steps, then read `.run/stream.port` after `./run_sample.sh run` reports
readiness. Docker and the .NET 8 SDK are required.

## Run the scene

1. Open this directory as a project in Cocos Creator 3.8.
2. Open `assets/EngineLobby.scene`. Its Canvas has `EngineLobbyBehaviour`, which creates the status Label.
3. Set **Endpoint** to `ws://127.0.0.1:<stream.port>` on the Canvas component and start the browser preview.
4. Confirm the Label changes from `joined as cocos-player (...)` to
   `cocos-player: hello from Cocos Creator`. Use the Server README stop command afterward.

If the server runs on another host, use an address the browser can reach. Use a `wss://` endpoint
with a valid certificate from an HTTPS page.

## Headless verification

With the server ready, use Node.js 22 `WebSocket` as a stand-in for browser transport to run two
instances of the same client class. This does not make Node a supported product target.

```bash
ENGINE_LOBBY_ENDPOINT="ws://127.0.0.1:<stream.port>" npm run probe
```

The probe checks the distinct Alice and Bob `actorId` values, Ping/Join replies, and identical
`ChatNotify(actorId, name, text)` values on both clients. Success prints
`cocos-engine-lobby-probe=ok`.

## Cocos Creator 3.8.8 verification

The Cocos Creator 3.8.8 CLI built the included scene against the locally rebuilt connector for
web-desktop and exited with code 36,
which the [official CLI documentation](https://docs.cocos.com/creator/3.8/manual/zh/editor/publish/publish-in-command-line.html)
defines as a successful build. In headless Chromium, the player received `JoinRes` and invoked
the `ChatNotify` handler with `{"actorId":"00000004","name":"cocos-player","text":"hello from Cocos Creator"}`.
The server recorded the client connection. The connector now uses `Array.from(...)` when taking
snapshots of iterable callback collections, including under the Cocos build transformation.
The published `0.23.0` package has not been updated with this correction.
