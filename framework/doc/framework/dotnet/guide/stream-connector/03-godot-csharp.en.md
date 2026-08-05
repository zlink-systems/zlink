# 03 — Godot C#

[← Table Of Contents](INDEX.en.md) | [Previous: Unity](02-unity.en.md)

---

This document explains how to use `Systems.Zlink.Stream.Connector` from a Godot 4 C# project
(Mono/.NET build). There's no dedicated Godot connector package. It uses the general `.NET`
connector as-is, calling `Dispatch.Async()` on the Godot main thread to run the user callbacks.

> **A Godot project using C++ GDExtension should see
> [C++ Guide 09 — Engine Adapters](../../../cpp/guide/stream-connector/09-engine-adapters.en.md)
> instead of this document.**
> **For a Web build, see the [Node/TypeScript Connector Guide](../../../node/guide/stream-connector/01-overview.en.md).**

## Basic Principle

Godot's `Node` and the scene tree must not be touched off the main thread. So the connector's
default dispatch mode is `Manual`. In this mode, the `On(...)` handler, error event, disconnect
event, and request callback all run on the thread that called `Dispatch.Async()`.

In Godot, `Dispatch.Async()` is called in `Node._Process(double)`. That runs the callbacks
accumulated for that frame on the Godot main thread.

## Node Example

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

If `Dispatch.Async()` isn't called in `_Process()`, the handler and events don't run. Check
`PendingDispatchCount` to see how many callbacks haven't been processed yet.

## Wiring Up With A Godot Signal

To notify the whole scene of an incoming packet, emit a signal inside the handler. Since the handler
already runs on the main thread, no additional marshalling is needed.

```csharp
[Signal]
public delegate void GameUpdatedEventHandler(string packetName);

// Inside _Ready()
_connector.On("game.update", (message, _) =>
{
    EmitSignal(SignalName.GameUpdated, message.Name);
    return ValueTask.CompletedTask;
});
```

## Pausing And Shutdown

Switching Godot's `Node.ProcessMode` to `Disabled` stops `_Process()`, so dispatch also stops. The
connection is kept, but callbacks pile up in the queue. To close the connection too, explicitly call
`Close.Async()`.

If `Close.Async()` and `DisposeAsync()` aren't called in `_ExitTree()`, the background receive loop
is left behind.

## What Is Not A Contract

The code in this document is Godot usage, not a connector contract. The connector itself doesn't
depend on Godot types. The common meaning of asynchronous execution follows the
[Common Framework Policy](../../../common/spec/05-async-execution-policy.en.md).
