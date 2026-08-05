# 09 — 엔진 어댑터

[← E2E 클라이언트](08-e2e-client.ko.md) | [목차](INDEX.ko.md) | [다음: 패키징 →](10-packaging.ko.md)

---

엔진 어댑터는 core connector를 private 구현으로 소유하고, 각 엔진의 타입과 thread 규칙에 맞는 표면만 노출한다. core의 `result_t<T>`, `connector_t` 같은 타입은 어댑터 public header에 드러나지 않는다.

**공통 원칙:**
- callback은 반드시 engine main thread에서 실행한다.
- core callback을 adapter queue에 넣고, engine Tick/Update/`Dispatch()`에서 꺼내 delegate로 전달한다.
- 예외와 coroutine에 의존하지 않는다.
- core private header(`connector/core/src/runtime/`)를 include하지 않는다.

---

## Unreal Engine

### 배포

`.uplugin` 형식으로 배포한다. `Plugins/ZLinkStreamConnector/` 아래에 배치한다.

```
Plugins/
└── ZLinkStreamConnector/
    ├── ZLinkStreamConnector.uplugin
    └── Source/
        ├── ZLinkStreamConnector/      ← Runtime module
        └── ZLinkStreamConnectorTests/ ← Automation Test module
```

### 기본 사용법

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
        // Game Thread에서 실행됨
    }
};
```

### Blueprint에서 사용

`Connect`, `Close`, `SendJson`, `RequestJson`, `Dispatch`는 모두 `BlueprintCallable`이다. `OnPacketReceived`, `OnRequestCompleted`는 `BlueprintAssignable` delegate다.

### Thread 규칙

core callback이 어느 thread에서 오든 `UObject`를 직접 만지지 않는다. 어댑터가 adapter queue에 넣고 `Dispatch()` 또는 Game Thread 예약 경로에서 delegate를 broadcast한다. PIE 종료, map unload, game instance shutdown에서 `Close()`가 자동 호출된다.

### Automation Test 실행

```bash
UnrealEditor-Cmd MyProject.uproject \
  -ExecCmds="Automation RunTests ZLink.StreamConnector; Quit" \
  -unattended -NullRHI
```

---

## Godot 4

### 배포

GDExtension source package로 배포한다. Godot project의 `addons/zlink_stream_connector/` 아래에 배치한다.

`.gdextension` 파일이 빌드된 shared library를 등록한다.

### 기본 사용법 (GDScript)

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
    # Godot main thread에서 실행됨
    pass
```

### Thread 규칙

signal은 Godot main thread에서만 emit한다. 어댑터가 core callback을 adapter queue에 넣고 `dispatch()` 또는 main thread update에서 signal을 emit한다.

### Headless 테스트

```bash
godot --headless --path TestProject --script res://run_tests.gd
```

---

## Axmol Engine

### 배포

CMake source package로 배포한다. Axmol project `CMakeLists.txt`에 포함하거나 vcpkg/Conan으로 core를 설치한다.

```cmake
add_subdirectory(third_party/zlink_axmol_connector)
target_link_libraries(${APP_NAME} PRIVATE zlink_axmol_connector)
```

### 기본 사용법

```cpp
#include "zlink_axmol_stream_connector.hpp"

class GameScene : public ax::Scene
{
    ZLinkAxmolStreamConnector* _connector;

    bool init() override
    {
        _connector = ZLinkAxmolStreamConnector::create();
        _connector->setPacketCallback([this](const std::string& name, const std::string& json) {
            // Axmol main thread에서 실행됨
        });
        _connector->connect("tcp://game.example.com:7000");
        this->schedule([this](float) { _connector->dispatch(); }, "update");
        return true;
    }
};
```

### Thread 규칙

core background callback은 `ax::Scheduler::runOnAxmolThread`를 통해 Axmol main thread로 전달된 뒤 user callback을 실행한다.

---

## 공통 엔진 어댑터 API 대응표

| 동작 | Unreal | Godot | Axmol |
|------|--------|-------|-------|
| 연결 | `Connect(Endpoint)` | `connect_to_server(url)` | `connect(endpoint)` |
| 종료 | `Close()` | `close()` | `close()` |
| 단방향 송신 | `SendJson(Name, Json)` | `send_json(name, json)` | `sendJson(name, json)` |
| 요청/응답 | `RequestJson(Name, Json, Timeout)` | `request_json(name, json, timeout)` | `requestJson(name, json, timeout)` |
| dispatch | `Dispatch()` (Tick에서 호출) | `dispatch()` (`_process`에서 호출) | `dispatch()` (schedule에서 호출) |
| push 수신 | `OnPacketReceived` delegate | `packet_received` signal | packet callback |
| 상태 변경 | `OnConnectionStateChanged` delegate | `connection_state_changed` signal | state callback |
