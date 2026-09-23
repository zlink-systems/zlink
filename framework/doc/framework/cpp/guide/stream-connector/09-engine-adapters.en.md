# Engine Adapters

[← E2E Client](08-e2e-client.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Packaging →](10-packaging.en.md)

---

An engine adapter owns the core connector as a private implementation, exposing only a surface that
fits each engine's types and thread rules. Core types like `result_t<T>`, `connector_t` aren't
revealed in the adapter's public header. See the [C++ contract §7](../../../common/spec/stream-connector/languages/cpp/03-stream-connector.en.md#7-engine-adapters)
for main thread delivery, name based subscriptions, and request completion packet names.

---

## Unreal Engine

### Distribution

Distributed in `.uplugin` format. Placed under `Plugins/ZLinkStreamConnector/`.

```
Plugins/
└── ZLinkStreamConnector/
    ├── ZLinkStreamConnector.uplugin
    └── Source/
        ├── ZLinkStreamConnector/      ← Runtime module
        └── ZLinkStreamConnectorTests/ ← Automation Test module
```

### Basic Usage

The Engine Lobby sample's Actor creates the connector, binds the receive delegates, subscribes to the server push `ChatNotify` by name and connects.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:connect-call"
```

Pumping the connector from `Tick` runs the delegates on the Game Thread.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:pump"
```

### Using It From Blueprint

`Connect`, `Close`, `SendJson`, `RequestJson`, `Subscribe`, `Dispatch` are all `BlueprintCallable`.
`OnPacketReceived`, `OnRequestCompleted` are `BlueprintAssignable` delegates.

`Close()` is called automatically on PIE shutdown, map unload, and game instance shutdown.

### Running Automation Tests

```bash
UnrealEditor-Cmd MyProject.uproject \
  -ExecCmds="Automation RunTests ZLink.StreamConnector; Quit" \
  -unattended -NullRHI
```

---

## Godot 4

### Distribution

Distributed as a GDExtension source package. Placed under the Godot project's
`addons/zlink_stream_connector/`.

The `.gdextension` file registers the built shared library.

### Basic Usage (C++)

The Engine Lobby sample's node registers the receive callback, subscribes to `ChatNotify` and connects.

```cpp title="Godot/cpp/src/engine_lobby_node.cpp"
--8<-- "framework/languages/engines/Godot/cpp/src/engine_lobby_node.cpp:connect"
```

It pumps the connector from `_process` on the Godot main thread.

```cpp title="Godot/cpp/src/engine_lobby_node.cpp"
--8<-- "framework/languages/engines/Godot/cpp/src/engine_lobby_node.cpp:pump"
```

---

## Axmol Engine

### Distribution

Distributed as a CMake source package. Include it in the Axmol project's `CMakeLists.txt`, or
install core via vcpkg/Conan.

```cmake
add_subdirectory(third_party/zlink_axmol_connector)
target_link_libraries(${APP_NAME} PRIVATE zlink_axmol_connector)
```

### Basic Usage

The Engine Lobby sample's Scene registers the receive callback, subscribes to `ChatNotify` and connects.

```cpp title="Axmol/Source/EngineLobbyScene.cpp"
--8<-- "framework/languages/engines/Axmol/Source/EngineLobbyScene.cpp:connect"
```

It pumps the connector from the Axmol scheduler update.

```cpp title="Axmol/Source/EngineLobbyScene.cpp"
--8<-- "framework/languages/engines/Axmol/Source/EngineLobbyScene.cpp:pump"
```

---

## Common Engine Adapter API Mapping

| Operation | Unreal | Godot | Axmol |
|------|--------|-------|-------|
| Connect | `Connect(Endpoint)` | `connect(endpoint)` | `connect(endpoint)` |
| Terminate | `Close()` | `close()` | `close()` |
| One-way send | `SendJson(Name, Json)` | `send_json(name, json)` | `send_json(name, json)` |
| Request/reply | `RequestJson(Name, Json, Timeout)` | `request_json(name, json, timeout)` | `request_json(name, json, timeout)` |
| Subscribe to push | `Subscribe(PacketName)` | `subscribe(packet_name)` | `subscribe(packet_name)` |
| Dispatch | `Dispatch()` (called from Tick) | `dispatch()` (called per frame) | `dispatch()` (called per frame) |
| Push receive | `OnPacketReceived` delegate | `on_packet` callback | `on_packet` callback |
| Status change | `OnConnectionStateChanged` delegate | `on_connection_state_changed` callback | `on_connection_state_changed` callback |
