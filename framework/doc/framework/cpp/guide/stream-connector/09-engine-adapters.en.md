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

```cpp
#include "ZLinkStreamConnector.h"

UCLASS()
class AMyGameMode : public AGameModeBase
{
    GENERATED_BODY()

    UPROPERTY()
    UZLinkStreamConnector* Connector;

    void BeginPlay() override
    {
        Connector = NewObject<UZLinkStreamConnector>(this);
        Connector->OnPacketReceived.AddDynamic(this, &AMyGameMode::HandlePacket);
        Connector->OnRequestCompleted.AddDynamic(this, &AMyGameMode::HandleReply);
        Connector->Connect(TEXT("tcp://game.example.com:7000"));
        Connector->Subscribe(TEXT("chat.notify"));
    }

    void Tick(float DeltaSeconds) override
    {
        Connector->Dispatch();
    }

    UFUNCTION()
    void HandlePacket(FName PacketName, const FString& JsonPayload)
    {
        // Runs on the Game Thread
    }
};
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

```cpp
#include "zlink_godot_stream_connector.hpp"

zlink::godot_stream_connector::stream_connector_t connector;
void start()
{
    connector.on_packet([](const zlink::godot_stream_connector::packet_t& packet) {
        // Handle packet.name and packet.payload.
    });
    connector.connect("tcp://game.example.com:7000");
    connector.subscribe("chat.notify");
}

void frame() { connector.dispatch(); } // Call on the Godot main thread.
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

```cpp
#include "zlink_axmol_stream_connector.hpp"

zlink::axmol_stream_connector::stream_connector_t connector;
void start()
{
    connector.on_packet([](const zlink::axmol_stream_connector::packet_t& packet) {
        // Handle packet.name and packet.payload.
    });
    connector.connect("tcp://game.example.com:7000");
    connector.subscribe("chat.notify");
}

void frame() { connector.dispatch(); } // Call on the Axmol main thread.
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
