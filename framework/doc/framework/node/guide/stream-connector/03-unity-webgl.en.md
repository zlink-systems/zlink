# 03 — Unity WebGL

[← Table Of Contents](INDEX.en.md) | [Previous: Browser](02-browser.en.md)

---

A Unity WebGL build runs in the browser sandbox, so it cannot open an OS socket and cannot
use the `.NET` connector. The `com.zlink.stream-connector.webgl` UPM package embeds this
TypeScript connector's browser bundle and supplies the jslib and C# call boundary Unity
needs. **There is no separate wire runtime** — it uses the same protocol and codecs as a
browser client.

The formal contract is owned by the
[Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md)
and the
[TypeScript Public Contract](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.en.md).
This chapter covers usage.

## Which connector to use

| Build target | Connector |
|---|---|
| Editor, desktop/mobile/console players | NuGet `Zlink.Stream.Connector` |
| WebGL player | UPM `com.zlink.stream-connector.webgl` |

The C# surface is the same on both. The namespace
`Systems.Zlink.Stream.Connector.Contracts`, the entry point
`ZlinkStreamConnectorFactory.Create(options)`, and the return type
`IZlinkStreamConnector` are identical, so game code does not branch on the build target.

## Install

Package Manager → **Add package from git URL**:

```
https://github.com/zlink-systems/zlink.git?path=framework/languages/unity/com.zlink.stream-connector.webgl#framework-node/v0.14.0
```

Always pin the tag. `framework-node/v<version>` is the tag of the
`@zlink-systems/stream-connector` release the embedded browser bundle came from, so the
adapter and the bundle move together.

Offline projects take `com.zlink.stream-connector.webgl-<version>.tgz` from the same
release and install it with **Add package from tarball**.

## Keeping both assemblies in one project

The two packages define the same type names. Only one may compile per build target.

- The UPM package's asmdef declares `"includePlatforms": ["WebGL"]`.
- Untick the WebGL platform for the NuGet `Systems.Zlink.Stream.Connector.dll` in the
  Plugin Inspector. If you consume it as source, put `"excludePlatforms": ["WebGL"]` on
  its asmdef.

## Connecting and pumping

The dispatch mode default is `Manual` and WebGL is single-threaded: nothing moves unless
the game pumps on the main thread.

```csharp
using System;
using Systems.Zlink.Stream.Connector.Contracts;
using UnityEngine;

public sealed class ZlinkStreamClientBehaviour : MonoBehaviour
{
    private IZlinkStreamConnector _connector;

    private async void Start()
    {
        _connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("wss://game.example.com/stream")
        });

        _connector.On("MatchAssigned", (message, _) =>
        {
            Debug.Log($"packet: {message.Name}, bytes: {message.Payload.Payload.Length}");
            return default;
        });

        await _connector.Connect.Async();
    }

    private async void Update()
    {
        if (_connector is not null) await _connector.Dispatch.Async();
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

`Dispatch.Async()` reads the transport and then runs the registered `On` handlers and the
error, disconnect, and request callbacks. `WaitFor`, `ExpectNone`, and `WaitForSequence`
read the transport themselves and consume the received queue without running any
registered callback, so they work whether or not `Update` is pumping.

## Differences from the native build

| Member | WebGL behaviour |
|---|---|
| `Endpoint` | `ws://` and `wss://` only. `tcp://` and `tls://` fail at `Create` with `ConfigurationError` |
| `SkipServerCertificateValidation` | Rejected at `Create`. The browser owns `wss://` certificate validation and exposes no way to skip it |
| `CompressionCodec` | Rejected at `Create`. The compression codec lives in the JavaScript connector; pick the algorithm with `Compression` |
| `PayloadCodec` | Required by typed `Send<T>`, `Request<T>`, and `WaitFor<T>`. Unity ships no `System.Text.Json`, so there is no JSON default and these throw `NotSupportedException` without it. The encoded API on `IZlinkStreamConnector` needs no codec |
| `PendingDispatchCount` | Callbacks waiting for the next `Dispatch` |

Every other member keeps the native package's name, signature, and meaning.

## What to check in Unity yourself

The jslib boundary is verified automatically against a real STREAM server by
`framework/languages/node/test/contract/unity-webgl-jslib.test.js`. Unity is not in CI, so
check the following by hand after changing the package.

1. The WebGL build finishes with no compile errors.
2. The built `Build/*.framework.js` contains `ZlinkStreamWebGlRuntime` and
   `ZlinkStreamConnectorBundle`. If it does not, check the WebGL platform box on the
   `.jspre` and `.jslib` files.
3. The player connects to a real STREAM server and connect, request/reply, and a push
   handler all work.
4. With the pump in `Update()`, handlers run on the Unity main thread (touching a
   `Transform` inside a handler does not throw).
5. Editor Play mode compiles and runs against the NuGet package.

## Related documents

- [02 — Browser](02-browser.en.md) — codec injection, dispatch, and flow delivery
- [.NET Stream Connector Guide 02 — Unity (native build)](../../../dotnet/guide/stream-connector/02-unity.en.md)
- [Install](../../../install.en.md)
