**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Unreal sample

This Unreal Engine 5 C++ project connects to the same Engine Lobby server as the Unity sample. A
single `AZLinkClientActor` owns the C++ stream connector, pumps it from `Tick`, performs
`Ping` → `Join` → `Chat`, and reports the `ChatNotify` result on screen and in the log.

The [`engine-lobby` sample contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md)
owns the packet names and JSON fields. This project adds no engine-specific aliases or fields.

## Unreal and connector versions

- Unreal Engine 5.4
- C++20
- The `ZLinkStreamConnector` plugin from
  `framework/languages/cpp/connector/engines/unreal`

This workspace has no Unreal Editor. The project descriptor is fixed to 5.4, but a complete Editor
build was not run. When moving to another Unreal Engine 5 minor version, verify project conversion
and the plugin build with that version's Unreal Build Tool.

## Install the connector

Before opening the project, prepare the connector plugin and a native package for the current
target under `Plugins/ZLinkStreamConnector/`.

From a monorepo checkout, copy the plugin source into the project:

```powershell
New-Item -ItemType Directory -Force Plugins | Out-Null
Copy-Item -Recurse -Force ../../cpp/connector/engines/unreal Plugins/ZLinkStreamConnector
```

Then follow the plugin's `ThirdParty/README.md` to package the C++ connector into
`Plugins/ZLinkStreamConnector/ThirdParty/ZLink/`. When running Unreal Build Tool,
`ZLINK_UNREAL_COMPILER_ID` and `ZLINK_UNREAL_COMPILER_VERSION` must match the toolchain recorded in
the manifest. In the exported mirror, place the same-version plugin source/package at that path.

## Run the server

Start the shared server from `../Server` in the same checkout:

```bash
./run_sample.sh build
./run_sample.sh run
cat .run/stream.port
```

If the printed port differs from the default `22700`, change the `Endpoint` default in
`EngineLobbyClientActor.h`.

## Run the project

1. Open `EngineLobby.uproject` in Unreal Editor 5.4 and build the C++ module.
2. Open an empty level and select **Play**. `EngineLobbyGameMode` spawns one
   `AZLinkClientActor`.
3. Confirm that the screen changes from `joined as unreal-player (...)` to
   `unreal-player: hello from Unreal`.
4. Confirm the same status in the Output Log.
5. Stopping Play removes the delegates and closes the connector.
6. Run `./run_sample.sh stop` in the server directory.

The `connect`, `pump`, `handler`, and `lifecycle` markers in `EngineLobbyClientActor.cpp` are the
source boundaries consumed by the engine integration guide.

## Minimum source validation

Without Unreal Editor, compile one translation unit that combines the sample's pure packet-name
header, the C++ connector public header, and the Unreal adapter public header:

```bash
g++-13 -std=c++20 -fsyntax-only \
  -I Source/EngineLobby/Public \
  -I ../../cpp/connector/core/include \
  -I ../../cpp/connector/engines/unreal/Source/ZLinkStreamConnector/Public \
  -I ../../../../bindings/cpp/include \
  Validation/ProtocolCompileCheck.cpp
```

This checks that the contract packet names, `packet_t`, manual-dispatch option, and Unreal adapter
type share a valid C++20 compile surface. Unreal reflection, generated headers, module linking,
Editor execution, and live server communication require a runner with Unreal Editor/Build Tool.
