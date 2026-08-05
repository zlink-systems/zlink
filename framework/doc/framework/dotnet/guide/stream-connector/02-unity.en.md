# 02 — Unity (Native Build)

[← Table Of Contents](INDEX.en.md) | [Previous: Overview](01-overview.en.md) | [Next: Godot C# →](03-godot-csharp.en.md)

---

This document explains how to use `Systems.Zlink.Stream.Connector` from a Unity client. There's no
dedicated Unity connector package. Unity also uses the general `.NET` connector as-is, calling
`Dispatch.Async()` on the Unity main thread to run the user callbacks.

> **A WebGL build is not the target of this document.** Unity WebGL runs in the browser sandbox, so
> it can't open an OS socket and can't use the `.NET` connector. WebGL calls the TypeScript
> connector through jslib interop. See the
> [Node/TypeScript Connector Guide](../../../node/guide/stream-connector/01-overview.en.md).

## Basic Principle

Unity objects must not be touched directly off the main thread. So the connector's default dispatch
mode is `Manual`. In this mode, the `On(...)` handler, error event, disconnect event, and request
callback all run on the thread that called `Dispatch.Async()`.

In Unity, `Dispatch.Async()` is called in `MonoBehaviour.Update()`. That runs the callbacks
accumulated for that frame on the Unity main thread.

The common meaning of asynchronous execution and the coroutine adapter follows the
[Common Framework Policy](../../../common/spec/05-async-execution-policy.en.md). Even in Unity, the
connector's public API is a `Task`/`ValueTask`-based asynchronous API, the same as general `.NET`.
Calls like `Connect.Async()`, `Close.Async()`, `Dispatch.Async()`,
`Request(...).Async<TReply>(...)`, `WaitFor(...).Async(...)` are used as-is.

`Send(...)` is a one-way call that doesn't wait for a response. It's run with `Async()`, which
returns no normal completion value. If a response is needed, use `Request(...)`.

## MonoBehaviour Example

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

If `Dispatch.Async()` isn't called in `Update()`, the handler and events don't run. Check
`PendingDispatchCount` to see how many callbacks haven't been processed yet.

## Handling A Pause

On mobile, the app can go to the background. Whether to keep the connection or close it is an
application policy. To allow a brief transition, leave the default reconnect policy as-is; to close
it explicitly, call `Close.Async()` from `OnApplicationPause`.

```csharp
private async void OnApplicationPause(bool paused)
{
    if (paused && _connector is not null)
    {
        await _connector.Close.Async();
    }
}
```

## Projects That Use Coroutines

In recent Unity, `async`/`await` can be used, so coroutines aren't required. If existing code is
centered on `StartCoroutine(...)`, you can place a thin helper like below inside the application.
This helper isn't a `Dispatch.Async()`-specific feature — it's an example of an application adapter
that fits the connector's awaitable call into Unity's frame flow.

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

This helper is just Unity usage, not a connector contract. The connector itself doesn't depend on
Unity types. Per the common policy, the framework or connector doesn't separately provide a
Unity-coroutine-specific public API or a blocking sync API.
