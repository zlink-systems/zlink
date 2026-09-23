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
https://github.com/zlink-systems/zlink.git?path=framework/languages/unity/com.zlink.stream-connector.webgl#framework-node/v0.16.0
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

If the JavaScript boundary itself fails while draining events, the call that was pumping
throws an `InvalidOperationException` naming the reason. That is not one of the
`ZlinkStreamErrorCode` values - the connection is fine, the adapter is not - and it is
raised rather than logged so a broken boundary cannot look like an idle one.

## Build setting: emscripten optimization

Any **Code Optimization** level works. Unity never hands these `.jspre` files to
emscripten: it links with `--pre-js` against a placeholder and merges the plugin text
into the emitted `framework.js` in a later build step, so emscripten's JavaScript
optimizer never sees this package.

The optimizer is worth naming anyway, because a toolchain that does pass this content to
it breaks. Emscripten 3.1.38 - what Unity 2023.2 and later bundle, as `3.1.38-unity` -
has a dead-code pass that deletes destructuring declarations from `--pre-js` content at
`-O2` and above. The link succeeds and the player then fails at runtime, the first
symptom being `ReferenceError: message is not defined` when a message is delivered. The
embedded bundle is built for es2019 so that the optimizer's parser and its pre-ES2020
tree converter both accept it.

`framework/languages/node/test/browser/unity-webgl-emscripten.test.js` links the plugins
with emcc directly, where that pass does run, and reports which levels they survive.

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
`ErrorReceived`, `Disconnected`, `ConnectionStateChanged`, `OnRequestSending`,
`OnReplyReceived` - has
the native signature and the native meaning.

## What is verified where

The jslib boundary is covered by
`framework/languages/node/test/contract/unity-webgl-jslib.test.js`, which loads the
`.jslib` and `.jspre` files into an emscripten stand-in and drives them against a real
STREAM server: connect, send, request, `on`, dispatch, close, the `tcp://` configuration
error, buffer ownership and the nested-pump guard.

`framework/languages/node/test/browser/unity-webgl-emscripten.test.js` links the same
files with a real emcc - the `.jslib` through `--js-library`, both `.jspre` through
`--pre-js` - and runs the result in Chromium against that server. It stands in for the
IL2CPP side with a C harness that declares the same `[DllImport("__Internal")]`
signatures and hands `ZlinkStreamSetEventSink` a real wasm function pointer, so the
link, the function-pointer callback and the `_malloc`/`_free`/`HEAPU8` marshalling are
exercised for real. It needs an emsdk install and skips without one. Note that plain
emcc puts `--pre-js` content through the JavaScript optimizer and Unity does not; that
difference is what the optimization section above describes.

`framework/languages/unity/webgl-adapter-check` is a Unity project that builds a real
WebGL player with this package installed and drives it in Chromium against the same
server, run by `.github/workflows/framework-unity-webgl.yml`. It covers what only Unity
can answer: that IL2CPP turns `[AOT.MonoPInvokeCallback]` into a reverse call that
delivers, that `"includePlatforms": ["WebGL"]` selects this assembly, that Unity imports
the package from a `file:` path with this layout, and that Unity's build pipeline links
both `.jspre` files and the `.jslib`. It builds at two Code Optimization levels and runs
both, and it asserts that the committed plugin text reaches the player unrewritten. A
Unity licence lives in repository secrets, so a fork pull request skips the workflow and
says so.

Two things are still checked by hand, because the project in CI is a connector check and
not a game:

1. **The main-thread pump reaches game objects.** Touch a `Transform` inside an `On`
   handler with `Dispatch.Async()` called from `Update()`; it must not throw.
2. **The Editor still uses the native package.** Entering Play mode in the Editor must
   compile and run against `Zlink.Stream.Connector`, not this assembly. The check project
   installs no NuGet assembly, so nothing in CI exercises the two side by side.

## License

FSL-1.1-ALv2. See `LICENSE`.
