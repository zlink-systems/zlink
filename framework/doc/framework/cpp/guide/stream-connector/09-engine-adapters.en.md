# 09 — Engine Adapters

[← E2E Client](08-e2e-client.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Packaging →](10-packaging.en.md)

---

An engine adapter owns the core connector as a private implementation, exposing only a surface that
fits each engine's types and thread rules. Core types like `result_t<T>`, `connector_t` aren't
revealed in the adapter's public header.

**Common principles:**
- A callback always runs on the engine's main thread.
- The core callback is put into an adapter queue, and pulled out on the engine's Tick/Update/
  `Dispatch()` to deliver to the delegate.
- No dependency on exceptions or coroutines.
- Core private headers (`connector/core/src/runtime/`) are not included.

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

`Connect`, `Close`, `SendJson`, `RequestJson`, `Dispatch` are all `BlueprintCallable`.
`OnPacketReceived`, `OnRequestCompleted` are `BlueprintAssignable` delegates.

### Thread Rules

Regardless of which thread a core callback comes from, `UObject` is never touched directly. The
adapter puts it into the adapter queue and broadcasts the delegate on `Dispatch()` or a Game
Thread-scheduled path. `Close()` is called automatically on PIE shutdown, map unload, and game
instance shutdown.

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

### Basic Usage (GDScript)

```gdscript
extends Node

var connector: ZLinkStreamConnector

func _ready():
    connector = ZLinkStreamConnector.new()
    connector.packet_received.connect(_on_packet)
    connector.connect_to_server("tcp://game.example.com:7000")

func _process(_delta):
    connector.dispatch()

func _on_packet(packet_name: StringName, payload: String):
    # Runs on the Godot main thread
    pass
```

### Thread Rules

A signal is emitted only on the Godot main thread. The adapter puts the core callback into the
adapter queue and emits the signal on `dispatch()` or a main-thread update.

### Headless Testing

```bash
godot --headless --path TestProject --script res://run_tests.gd
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

class GameScene : public ax::Scene
{
    ZLinkAxmolStreamConnector* _connector;

    bool init() override
    {
        _connector = ZLinkAxmolStreamConnector::create();
        _connector->setPacketCallback([this](const std::string& name, const std::string& json) {
            // Runs on the Axmol main thread
        });
        _connector->connect("tcp://game.example.com:7000");
        this->schedule([this](float) { _connector->dispatch(); }, "update");
        return true;
    }
};
```

### Thread Rules

A core background callback is delivered to the Axmol main thread via
`ax::Scheduler::runOnAxmolThread` before running the user callback.

---

## Common Engine Adapter API Mapping

| Operation | Unreal | Godot | Axmol |
|------|--------|-------|-------|
| Connect | `Connect(Endpoint)` | `connect_to_server(url)` | `connect(endpoint)` |
| Terminate | `Close()` | `close()` | `close()` |
| One-way send | `SendJson(Name, Json)` | `send_json(name, json)` | `sendJson(name, json)` |
| Request/reply | `RequestJson(Name, Json, Timeout)` | `request_json(name, json, timeout)` | `requestJson(name, json, timeout)` |
| Dispatch | `Dispatch()` (called from Tick) | `dispatch()` (called from `_process`) | `dispatch()` (called from schedule) |
| Push receive | `OnPacketReceived` delegate | `packet_received` signal | packet callback |
| Status change | `OnConnectionStateChanged` delegate | `connection_state_changed` signal | state callback |
