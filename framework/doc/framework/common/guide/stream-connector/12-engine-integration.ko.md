# 게임 엔진 통합

!!! info "이 장을 읽고 나면"

    엔진과 빌드 대상에 맞는 connector를 고르고, 설치부터 packet 송수신과 배포까지 다루는
    가이드와 실행 가능한 Engine Lobby sample을 찾을 수 있다.

게임 client는 STREAM server에 연결한다. Unity는 빌드 대상에 따라 connector가 달라지고,
Unreal과 Godot C#은 각각 C++와 .NET connector를 사용한다. 다음 표의 가이드는 설치, 연결,
송수신과 main thread 규칙을 다룬다. 배포 열은 엔진별 package와 빌드 절차를 가리킨다.

| 엔진과 빌드 대상 | connector | 사용 가이드 | 배포 |
|---|---|---|---|
| Unity native | `.NET` | [Unity native](../../../dotnet/guide/stream-connector/08-unity.ko.md) | [Unity sample의 native build](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.ko.md) |
| Unity WebGL | TypeScript connector와 UPM 어댑터 | [Unity WebGL](../../../node/guide/stream-connector/09-unity-webgl.ko.md) | [Unity sample의 WebGL build](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.ko.md) |
| Unreal | C++ connector의 Unreal plugin | [엔진 어댑터](../../../cpp/guide/stream-connector/09-engine-adapters.ko.md) | [C++ 배포](../../../cpp/guide/stream-connector/10-packaging.ko.md) |
| Godot C# | `.NET` | [Godot C#](../../../dotnet/guide/stream-connector/09-godot-csharp.ko.md) | 이 장에는 배포 절차가 없다 |

## 1. Engine Lobby sample

[Engine Lobby 계약](../../../common/sample/engine-lobby/README.ko.md)은 server와 client가 주고받는
packet을 정의한다. [.NET server](https://github.com/zlink-systems/zlink-engine-server),
[Unity project](https://github.com/zlink-systems/zlink-unity-examples),
[Unreal project](https://github.com/zlink-systems/zlink-unreal-examples)는 각각 별도 저장소에도
제공된다. 이 저장소의 원본과 실행 순서는
[server](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Server/README.ko.md),
[Unity](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.ko.md),
[Unreal](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unreal/README.ko.md) README에 있다.

Unity sample은 같은 client source를 native와 WebGL 빌드에 사용한다. 다음 코드는
연결한 뒤 `JoinReq`로 응답을 받고 `ChatMsg`를 보낸다.

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:connect"
```

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:send"
```

`On<ChatNotify>` callback은 `Update()`에서 `Dispatch.Async()`를 실행할 때 처리된다.

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:receive"
```

Unreal client는 plugin을 만든 뒤 연결하고, `PingReq`의 응답을 받으면 lobby 참가를 시작한다.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:connect-call"
```

`JoinReq`는 응답을 기다리고, `ChatMsg`는 단방향으로 보낸다.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:send"
```

`Tick()`에서 connector를 pump하면 `ChatNotify` delegate가 화면을 갱신한다.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:receive"
```
