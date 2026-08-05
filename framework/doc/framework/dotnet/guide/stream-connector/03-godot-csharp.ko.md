# 03 — Godot C#

[← 목차](INDEX.ko.md) | [이전: Unity](02-unity.ko.md)

---

이 문서는 Godot 4의 C# 프로젝트(Mono/.NET 빌드)에서 `Systems.Zlink.Stream.Connector`를
사용하는 방법을 설명한다. Godot 전용 connector package는 따로 두지 않는다. 일반 `.NET`
connector를 그대로 쓰고, Godot main thread에서 `Dispatch.Async()`를 호출해 사용자 callback을
실행한다.

> **C++ GDExtension을 쓰는 Godot 프로젝트는 이 문서가 아니라
> [C++ 가이드 09 — 엔진 어댑터](../../../cpp/guide/stream-connector/09-engine-adapters.ko.md)를 본다.**
> **Web 빌드는 [Node/TypeScript connector 가이드](../../../node/guide/stream-connector/01-overview.ko.md)를 본다.**

## 기본 원칙

Godot의 `Node`와 scene tree는 main thread 밖에서 다루면 안 된다. 그래서 connector의 기본
dispatch mode는 `Manual`이다. 이 모드에서는 `Dispatch.Async()`를 호출한 thread에서
`On(...)` handler, error event, disconnect event와 request callback을 실행한다.

Godot에서는 `Node._Process(double)`에서 `Dispatch.Async()`를 호출한다. 그러면 그 frame에 쌓인
callback이 Godot main thread에서 실행된다.

## Node 예시

```csharp
using System;
using System.Threading.Tasks;
using Godot;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

public partial class ZlinkStreamClientNode : Node
{
    private IZlinkStreamConnector? _connector;

    public override async void _Ready()
    {
        _connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("wss://example.com/stream")
        });

        _connector.ConnectionStateChanged += (change, _) =>
        {
            GD.Print($"ZLink stream state: {change.Current}");
            return ValueTask.CompletedTask;
        };

        _connector.On("game.update", (message, _) =>
        {
            GD.Print($"packet: {message.Name}, bytes: {message.Payload.Payload.Length}");
            return ValueTask.CompletedTask;
        });

        await _connector.Connect.Async();
    }

    public override async void _Process(double delta)
    {
        if (_connector is not null)
        {
            await _connector.Dispatch.Async();
        }
    }

    public override async void _ExitTree()
    {
        if (_connector is not null)
        {
            await _connector.Close.Async();
            await _connector.DisposeAsync();
            _connector = null;
        }
    }
}
```

`_Process()`에서 `Dispatch.Async()`를 호출하지 않으면 handler와 event는 실행되지 않는다.
`PendingDispatchCount`로 아직 처리하지 않은 callback 수를 확인한다.

## Godot signal로 연결하기

수신 packet을 scene 전체에 알리려면 handler 안에서 signal을 emit한다. handler는 이미 main
thread에서 실행되므로 추가 marshalling이 필요 없다.

```csharp
[Signal]
public delegate void GameUpdatedEventHandler(string packetName);

// _Ready() 안에서
_connector.On("game.update", (message, _) =>
{
    EmitSignal(SignalName.GameUpdated, message.Name);
    return ValueTask.CompletedTask;
});
```

## 일시 정지와 종료

Godot의 `Node.ProcessMode`를 `Disabled`로 바꾸면 `_Process()`가 멈추므로 dispatch도 멈춘다.
연결은 유지되지만 callback은 queue에 쌓인다. 연결까지 닫으려면 `Close.Async()`를 명시적으로
호출한다.

`_ExitTree()`에서 `Close.Async()`와 `DisposeAsync()`를 호출하지 않으면 백그라운드 receive
loop가 남는다.

## 계약이 아닌 것

이 문서의 코드는 Godot 사용법이며 connector 계약이 아니다. connector 자체는 Godot 타입에
의존하지 않는다. 비동기 실행의 공통 의미는
[framework 공통 정책](../../../common/spec/05-async-execution-policy.ko.md)을 따른다.
