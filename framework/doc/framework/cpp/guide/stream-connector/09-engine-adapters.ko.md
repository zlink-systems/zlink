# 엔진 어댑터

[← E2E 클라이언트](08-e2e-client.ko.md) | [목차](INDEX.ko.md) | [다음: 패키징 →](10-packaging.ko.md)

---

엔진 어댑터는 core connector를 private 구현으로 소유하고, 각 엔진의 타입과 thread 규칙에 맞는 표면만 노출한다. core의 `result_t<T>`, `connector_t` 같은 타입은 어댑터 public header에 드러나지 않는다. Main thread 전달, 이름별 구독, 요청 완료의 packet 이름은 [C++ 계약 §7](../../../common/spec/stream-connector/languages/cpp/03-stream-connector.ko.md#7-엔진-어댑터)을 따른다.

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

Engine Lobby 샘플의 Actor는 connector를 만들고 수신 delegate를 연결한 뒤, 서버 push인 `ChatNotify`를 이름으로 구독하고 연결한다.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:connect-call"
```

`Tick`에서 connector를 pump하면 delegate가 Game Thread에서 실행된다.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:pump"
```

### Blueprint에서 사용

`Connect`, `Close`, `SendJson`, `RequestJson`, `Subscribe`, `Dispatch`는 모두 `BlueprintCallable`이다. `OnPacketReceived`, `OnRequestCompleted`는 `BlueprintAssignable` delegate다.

PIE 종료, map unload, game instance shutdown에서 `Close()`가 자동 호출된다.

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

### 기본 사용법 (C++)

Engine Lobby 샘플의 node는 수신 callback을 등록하고 `ChatNotify`를 구독한 뒤 연결한다.

```cpp title="Godot/cpp/src/engine_lobby_node.cpp"
--8<-- "framework/languages/engines/Godot/cpp/src/engine_lobby_node.cpp:connect"
```

Godot main thread의 `_process`에서 connector를 pump한다.

```cpp title="Godot/cpp/src/engine_lobby_node.cpp"
--8<-- "framework/languages/engines/Godot/cpp/src/engine_lobby_node.cpp:pump"
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

Engine Lobby 샘플의 Scene은 수신 callback을 등록하고 `ChatNotify`를 구독한 뒤 연결한다.

```cpp title="Axmol/Source/EngineLobbyScene.cpp"
--8<-- "framework/languages/engines/Axmol/Source/EngineLobbyScene.cpp:connect"
```

Axmol scheduler update에서 connector를 pump한다.

```cpp title="Axmol/Source/EngineLobbyScene.cpp"
--8<-- "framework/languages/engines/Axmol/Source/EngineLobbyScene.cpp:pump"
```

---

## 공통 엔진 어댑터 API 대응표

| 동작 | Unreal | Godot | Axmol |
|------|--------|-------|-------|
| 연결 | `Connect(Endpoint)` | `connect(endpoint)` | `connect(endpoint)` |
| 종료 | `Close()` | `close()` | `close()` |
| 단방향 송신 | `SendJson(Name, Json)` | `send_json(name, json)` | `send_json(name, json)` |
| 요청/응답 | `RequestJson(Name, Json, Timeout)` | `request_json(name, json, timeout)` | `request_json(name, json, timeout)` |
| push 구독 | `Subscribe(PacketName)` | `subscribe(packet_name)` | `subscribe(packet_name)` |
| dispatch | `Dispatch()` (Tick에서 호출) | `dispatch()` (프레임에서 호출) | `dispatch()` (프레임에서 호출) |
| push 수신 | `OnPacketReceived` delegate | `on_packet` callback | `on_packet` callback |
| 상태 변경 | `OnConnectionStateChanged` delegate | `on_connection_state_changed` callback | `on_connection_state_changed` callback |
