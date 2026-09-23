[English](README.md) | **한국어**

# ZLink Engine Lobby Godot sample

Godot 4 C# scene이 공용 Engine Lobby server에 연결한다. `EngineLobbyNode`는 frame마다
connector callback을 Godot main thread에서 처리하고, `PingReq` → `JoinReq` → `ChatMsg` 흐름의
`ChatNotify`를 화면에 표시한다.

Packet 이름과 JSON field는 [Engine Lobby 공통 계약](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)을 따른다.

## Version과 package

- Godot 4.4.1 .NET edition
- .NET 8 SDK와 `Godot.NET.Sdk` 4.4.1
- `Zlink.Stream.Connector` 0.22.0

`EngineLobby.csproj`가 두 NuGet package를 복원한다. Godot Web export는 이 .NET project의
검증 대상이 아니다.

## 설치

```bash
dotnet restore EngineLobby.csproj
```

## 빌드

```bash
dotnet build EngineLobby.csproj -c Release
dotnet build Validation/EngineLobbyValidation.csproj -c Release
```

## Scene 실행

1. 공용 `zlink-engine-server` sample을 실행하고 `stream.port` 값을 읽는다. Monorepo에서는
   `../../Server`에서 `./run_sample.sh build`와 `./run_sample.sh run`을 실행한다.
2. Godot 4.4.1 .NET editor에서 `project.godot`를 열고 C# project를 build한다.
3. `EngineLobby.tscn`의 root node에서 `Endpoint`를
   `ws://127.0.0.1:<stream.port>`로 설정하고 scene을 실행한다.
4. 화면이 `joined as godot-player (...)`를 거쳐 `godot-player: hello from Godot`로
   바뀌는지 확인한다. 이때 `_Process`가 `Dispatch.Async()`를 호출한다.
5. Monorepo server를 사용했다면 `../../Server`에서 `./run_sample.sh stop`을 실행한다.

## Headless connector 검증

`Validation`은 scene이 사용하는 `EngineLobbyClient.cs`를 그대로 compile한다. 준비된 server의
WebSocket endpoint를 인자로 주면 client 두 개로 Ping, Join과 양쪽 ChatNotify payload를 확인한다.

```bash
dotnet run --project Validation/EngineLobbyValidation.csproj -c Release -- \
  ws://127.0.0.1:<stream.port>
```

성공 출력은 `godot-engine-lobby-validation=ok`다. 이 검증은 Godot scene tree, editor의 C#
assembly load와 실제 화면 표시를 실행하지 않는다.

## 이 머신에서 확인한 범위

WSL에서 Godot C# project와 headless 검증 project를 build했다. 공용 .NET server에 연결한
client 두 개의 Ping, Join, ChatNotify payload를 확인했다. Godot editor가 설치되지 않아
scene 실행과 화면 갱신은 확인하지 못했다.
