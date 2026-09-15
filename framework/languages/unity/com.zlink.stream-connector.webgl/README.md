# ZLink Stream Connector (Unity WebGL)

UPM adapter that gives a Unity WebGL build the same C# STREAM connector surface as the
native `Zlink.Stream.Connector` NuGet package.

A WebGL build runs in the browser sandbox, where `System.Net.Sockets` and
`System.Net.WebSockets` do not work. This package does not reimplement the wire; it
embeds the browser bundle of `@zlink-systems/stream-connector` and provides the jslib
and C# call boundary that reaches it
([stream-connector spec §11](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/spec/stream-connector/32-stream-connector.md)).

## Install

Package Manager → **Add package from git URL**:

```
https://github.com/zlink-systems/zlink.git?path=framework/languages/unity/com.zlink.stream-connector.webgl#framework-node/v0.14.0
```

Always pin the tag. `framework-node/v<version>` is the tag of the
`@zlink-systems/stream-connector` release the embedded bundle came from, so the adapter
and the bundle move together.

Offline or vendored projects use `com.zlink.stream-connector.webgl-<version>.tgz` from
the same GitHub release and **Add package from tarball**.

## Using it alongside the native connector

Both assemblies define `Systems.Zlink.Stream.Connector.Contracts`. Only one may compile
per build target:

- This package's assembly declares `"includePlatforms": ["WebGL"]`.
- Exclude `Systems.Zlink.Stream.Connector.dll` from the WebGL platform in the Plugin
  Inspector (or set `"excludePlatforms": ["WebGL"]` on its asmdef if you consume it as
  source).

The Editor and native players then use the NuGet assembly, the WebGL player uses this
one, and the game code is the same text in both:

```csharp
using Systems.Zlink.Stream.Connector.Contracts;

_connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri("wss://example.com/stream")
});

_connector.On("game.update", (message, _) => { /* ... */ return default; });
await _connector.Connect.Async();
```

## Pump on the main thread

The dispatch mode default is `Manual`, and WebGL is single-threaded: nothing moves
unless the game pumps.

```csharp
private async void Update()
{
    if (_connector is not null) await _connector.Dispatch.Async();
}
```

`Dispatch` reads the transport and then runs the registered `On`, error, disconnect and
request callbacks. The wait surfaces (`WaitFor`, `ExpectNone`, `WaitForSequence`) read
the transport themselves and consume the unread queue without running any registered
callback, so they work whether or not `Update` is pumping.

## Differences from the native package

| Member | WebGL behaviour |
|---|---|
| `Endpoint` | `ws://` and `wss://` only. `tcp://` and `tls://` fail at `Create` with `ConfigurationError` - the browser sandbox cannot open an OS socket. |
| `SkipServerCertificateValidation` | Rejected at `Create`. The browser owns certificate validation for `wss://` and exposes no way to skip it. |
| `CompressionCodec` | Rejected at `Create`. The compression codec runs in the JavaScript connector; select the algorithm with `Compression`. |
| `PayloadCodec` | Required by the typed `Send<T>` / `Request<T>` / `WaitFor<T>` extensions, which throw `NotSupportedException` without it. Unity ships no `System.Text.Json`, so there is no JSON fallback. The encoded API on `IZlinkStreamConnector` needs no codec. |
| `PendingDispatchCount` | Callbacks waiting for the next `Dispatch`. |

Everything else - `Connect`/`Close`/`Dispatch`, `Send`/`Request`, `On`, `WaitFor`,
`ExpectNone`, `WaitForSequence`, `ReceivedCount`, `IsConnected`, `State`, `Options`,
`ErrorReceived`, `Disconnected`, `ConnectionStateChanged`, `SetDiagnosticsLevel` - has
the native signature and the native meaning.

## What is verified where

The jslib boundary is covered by
`framework/languages/node/test/contract/unity-webgl-jslib.test.js`, which loads the
`.jslib` and `.jspre` files into an emscripten stand-in and drives them against a real
STREAM server: connect, send, request, `on`, dispatch, close, the `tcp://` configuration
error, buffer ownership and the nested-pump guard.

`framework/languages/node/test/browser/unity-webgl-emscripten.test.js` links the same
files the way Unity does - `.jslib` through `emcc --js-library`, both `.jspre` through
`--pre-js` - and runs the result in Chromium against that server. It stands in for the
IL2CPP side with a C harness that declares the same `[DllImport("__Internal")]`
signatures and hands `ZlinkStreamSetEventSink` a real wasm function pointer, so the
link, the function-pointer callback and the `_malloc`/`_free`/`HEAPU8` marshalling are
exercised for real. It needs an emsdk install and skips without one.

Unity itself is not in CI. Check these by hand after changing the package:

1. **WebGL build passes.** Build a WebGL player with the package installed and no
   compile errors from `Systems.Zlink.Stream.Connector.WebGL`.
2. **The plugins are linked.** The built `Build/*.framework.js` contains
   `ZlinkStreamWebGlRuntime` and `ZlinkStreamConnectorBundle`. If it does not, the
   `.jspre` and `.jslib` files were not picked up - check that their WebGL platform box
   is ticked in the Plugin Inspector.
3. **A real connection works.** Run the player against a STREAM server over `ws://` (or
   `wss://` with a certificate the browser trusts) and confirm connect, request/reply and
   a push handler.
4. **The main-thread pump works.** With `Dispatch.Async()` called from `Update()`,
   handlers run on the Unity main thread (touch a `Transform` inside a handler; it must
   not throw), and no callback arrives while `Update` is not running.
5. **The Editor still uses the native package.** Entering Play mode in the Editor must
   compile and run against `Zlink.Stream.Connector`, not this assembly.

## License

FSL-1.1-ALv2. See `LICENSE`.
