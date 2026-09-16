# Unity WebGL adapter check

A Unity project that exists only to build `com.zlink.stream-connector.webgl` into
a real WebGL player and run it. It is **not a sample**. Nothing here is an
example of how to use the connector; the guide and the package README own that.

`framework/languages/node/test/browser/unity-webgl-emscripten.test.js` already
links the adapter's `.jslib` and both `.jspre` files with emcc and drives them in
Chromium against a real STREAM server, with a C harness standing in for IL2CPP.
That covers the link, the reverse function pointer and the heap marshalling
without Unity. Four things are left that only Unity can answer, and this project
exists for exactly those four:

| Question | Where the answer shows up |
|---|---|
| IL2CPP turns `[AOT.MonoPInvokeCallback]` into a working reverse call | the run reaches `connected`: every connector event arrives through that callback |
| `"includePlatforms": ["WebGL"]` selects the adapter assembly | `adapterAssembly` in the reported result |
| Unity imports the package from a `file:` path with this layout | the project compiles at all, plus the `.meta` files Unity writes into the package |
| Unity's plugin pipeline links both `.jspre` and the `.jslib` | `linkedPlugins` in the reported result, asked of the running player |

## Layout

| Path | Role |
|---|---|
| `Packages/manifest.json` | the adapter as a local `file:` package, relative to `Packages/` |
| `Assets/Zlink/` | the WebGL-only check assembly: connect, request/reply, `WaitFor`, `On` + `Update` pump, close |
| `Assets/Editor/` | the Editor-only batchmode build entry point |
| `Assets/Plugins/WebGL/ZlinkVerificationReport.jslib` | the check's own result channel, and the control for "did Unity link plugins at all" |
| `Assets/link.xml` | preserves the check's entry point, and only that |

The scenario follows `test/browser/unity-webgl-emscripten.test.js` step for step,
including the `WaitFor` that consumes the push the request produced. The server
answers every request with a push as well as a reply, so without it two pushes
are outstanding and the handler takes the older one - spec 32 section 10 keeps a
push in the received-message queue until a handler or a wait surface takes it.

The check assembly is `WebGL`-only, exactly like the adapter's, so the Editor
cannot see its types. That is why the player entry point is a
`[RuntimeInitializeOnLoadMethod]` and the built scene is empty: an Editor script
cannot add a component whose type it cannot name.

## Running it

CI only - `.github/workflows/framework-unity-webgl.yml`, `workflow_dispatch`. A
Unity licence is required and comes from repository secrets, so a fork pull
request cannot run it and the workflow says so instead of failing quietly.

```
Unity -batchmode -quit -nographics -buildTarget WebGL \
  -projectPath framework/languages/unity/webgl-adapter-check \
  -executeMethod Zlink.Verification.Editor.ZlinkWebGlBuild.Build \
  -zlinkOutput Builds -zlinkOptimizations BuildTimes,RuntimeSpeed
```

`-zlinkOutput` is relative to the project folder, and `-zlinkOptimizations`
defaults to both levels when it is not passed at all - a flag passed with no
value is an error rather than a silent fallback. Each level lands in
`Builds/<level>/`, with `Builds/build-summary.json` covering all of them, and
`framework/languages/node/test/browser/support/unity-player/drive-player.js`
serves one of those folders and drives it in Chromium.

## Why two optimization levels

`BuildTimes` is emcc `-O1`, the level the adapter README tells consumers to build
at. `RuntimeSpeed` is `-O2`, where emscripten runs JSDCE over the concatenated
`--pre-js` content. Emscripten 3.1.38's JSDCE reads `node.id.name` for every
variable declarator, which is `undefined` for a destructuring pattern, so the
declaration registers a binding named `undefined` and is deleted as unused. The
player links and then dies on the first delivered message.

Unity 6000.0 through 6000.4 bundle `3.1.38-unity`, whose
`tools/acorn-optimizer.js` is upstream 3.1.38's byte for byte apart from the
`require` path for acorn. Running that fork's JSDCE over the committed bundle by
hand does delete `const { message, signal } = queued`.

Unity's own builds do not. In both players the `.jspre` content arrives verbatim
- comments, indentation and all five destructuring declarations - while
emscripten's generated JavaScript beside it is whitespace-minified, at both
optimization levels. The optimizer runs and never sees the plugin content. The
workflow reads the link arguments out of `Library/Bee` to say why.

That is what the two levels are here to compare, so the comparison stays: the
driver asserts the outcome it expects for each rather than accepting whatever
happens.
