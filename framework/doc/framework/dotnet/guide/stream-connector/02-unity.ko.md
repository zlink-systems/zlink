# 02 — Unity (네이티브 빌드)

[← 목차](INDEX.ko.md) | [이전: 개요](01-overview.ko.md) | [다음: Godot C# →](03-godot-csharp.ko.md)

---

이 문서는 Unity client에서 `Systems.Zlink.Stream.Connector`를 사용하는 방법을 설명한다.
Unity 전용 connector package는 따로 두지 않는다. Unity도 일반 `.NET` connector를 그대로
쓰고, Unity main thread에서 `Dispatch.Async()`를 호출해 사용자 callback을 실행한다.

> **WebGL 빌드는 이 문서의 대상이 아니다.** Unity WebGL은 브라우저 샌드박스에서 실행되므로
> OS 소켓을 열 수 없고, `.NET` connector를 사용할 수 없다. WebGL은 TypeScript connector를
> jslib interop으로 호출한다.
> [Node/TypeScript connector 가이드](../../../node/guide/stream-connector/01-overview.ko.md)를 본다.

## 기본 원칙

Unity 객체는 main thread 밖에서 직접 다루면 안 된다. 그래서 connector의 기본 dispatch mode는
`Manual`이다. 이 모드에서는 `Dispatch.Async()`를 호출한 thread에서 `On(...)` handler,
error event, disconnect event와 request callback을 실행한다.

Unity에서는 `MonoBehaviour.Update()`에서 `Dispatch.Async()`를 호출한다. 그러면 그 frame에
쌓인 callback이 Unity main thread에서 실행된다.

비동기 실행과 coroutine adapter의 공통 의미는
[framework 공통 정책](../../../common/spec/05-async-execution-policy.ko.md)을 따른다.
Unity에서도 connector의 public API는 일반 `.NET`과 같은 `Task` / `ValueTask` 기반
비동기 API다. `Connect.Async()`, `Close.Async()`, `Dispatch.Async()`,
`Request(...).Async<TReply>(...)`, `WaitFor(...).Async(...)` 같은 호출을 그대로 사용한다.

`Send(...)`는 응답을 기다리지 않는 one-way 호출이다. 정상 완료 값을 반환하지 않는
`Async()`로 실행한다. 응답이 필요하면 `Request(...)`를 쓴다.

## MonoBehaviour 예시

```csharp
using System;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using UnityEngine;

public sealed class ZlinkStreamClientBehaviour : MonoBehaviour
{
    private IZlinkStreamConnector? _connector;

    private async void Start()
    {
        _connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("wss://example.com/stream")
        });

        _connector.ConnectionStateChanged += (change, _) =>
        {
            Debug.Log($"ZLink stream state: {change.Current}");
            return ValueTask.CompletedTask;
        };

        _connector.On("game.update", (message, _) =>
        {
            Debug.Log($"packet: {message.Name}, bytes: {message.Payload.Payload.Length}");
            return ValueTask.CompletedTask;
        });

        await _connector.Connect.Async();
    }

    private async void Update()
    {
        if (_connector is not null)
        {
            await _connector.Dispatch.Async();
        }
    }

    private async void OnDestroy()
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

`Update()`에서 `Dispatch.Async()`를 호출하지 않으면 handler와 event는 실행되지 않는다.
`PendingDispatchCount`로 아직 처리하지 않은 callback 수를 확인한다.

## 일시 정지 처리

모바일에서는 앱이 background로 내려가기도 한다. 연결을 유지할지 닫을지는 application 정책이다.
짧은 전환을 허용하려면 기본 reconnect 정책을 그대로 두고 명시적으로 닫고 싶으면
`OnApplicationPause`에서 `Close.Async()`를 호출한다.

```csharp
private async void OnApplicationPause(bool paused)
{
    if (paused && _connector is not null)
    {
        await _connector.Close.Async();
    }
}
```

## 코루틴을 쓰는 프로젝트

최신 Unity에서는 `async` / `await`를 쓸 수 있으므로 코루틴이 필수는 아니다. 기존 코드가
`StartCoroutine(...)` 중심이라면 아래처럼 얇은 helper를 application 안에 둘 수 있다.
이 helper는 `Dispatch.Async()` 전용 기능이 아니라, connector의 awaitable 호출을 Unity
frame 흐름에 맞추는 application adapter 예시다.

```csharp
using System.Collections;

private IEnumerator DispatchCoroutine()
{
    if (_connector is null)
    {
        yield break;
    }

    var task = _connector.Dispatch.Async().AsTask();
    while (!task.IsCompleted)
    {
        yield return null;
    }

    task.GetAwaiter().GetResult();
}
```

이 helper는 Unity 사용법일 뿐 connector 계약이 아니다. connector 자체는 Unity 타입에
의존하지 않는다. framework나 connector는 공통 정책에 따라 Unity coroutine 전용 public
API나 blocking sync API를 따로 제공하지 않는다.
