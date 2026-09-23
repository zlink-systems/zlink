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

## Run the server

Use `../Server` in the monorepo, or prepare the `zlink-engine-server` mirror. Follow its README
install, build, and run steps, then read `.run/stream.port` after `./run_sample.sh run` reports
readiness. Docker and the .NET 8 SDK are required.

## Run the scene

1. Open this directory as a project in Cocos Creator 3.8.
2. Create a 2D scene with a Label under its Canvas.
3. Attach `EngineLobbyBehaviour` to an empty Node and assign the Label to **Status Label**.
4. Set **Endpoint** to `ws://127.0.0.1:<stream.port>` and start the browser preview.
5. Confirm the Label changes from `joined as cocos-player (...)` to
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

## Verified on this machine

`npm run typecheck`, the .NET server build, and `npm run probe` with a dedicated Redis process and
server passed in WSL. The Server Docker runner could not run because Docker integration was not
available. Cocos Creator Editor was not installed, so project import, scene setup, web build, and
browser player execution remain unverified.
