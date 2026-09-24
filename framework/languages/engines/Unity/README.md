**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Unity sample

One `ZLinkClient` MonoBehaviour connects to the Engine Lobby server with the same game code in
native and WebGL players. It sends `JoinReq` and `ChatMsg` at startup, pumps the connector from `Update`,
and replaces the centered UI text when `ChatNotify` arrives.

## Unity version

The project is pinned to **Unity 6 LTS 6000.0.83f1**. The WebGL UPM manifest allows Unity 2021.3
or later, but the repository's real WebGL adapter verification project also uses 6000.0.83f1, so
the sample and its verification target one editor version.

## Install packages

`Packages/manifest.json` pins these packages:

- NuGetForUnity `v4.5.0`
- `com.zlink.stream-connector.webgl` at `framework-node/v0.23.0`
- Unity UI `2.0.0`

`Assets/packages.config` declares `Zlink.Stream.Connector` `0.23.0` for the Editor and native
desktop/mobile/console players. If the first open reports compile errors before that package is
present, choose **Ignore**, not Safe Mode, and run **NuGet → Restore Packages**.

Both connector assemblies define the same public types, so each build target must include exactly
one:

1. Select the restored `Systems.Zlink.Stream.Connector.dll` in the Project window.
2. In the Plugin Inspector, disable **WebGL** compatibility and enable only the Editor and required
   native platforms.
3. If the native connector is consumed as source instead of a DLL, put
   `"excludePlatforms": ["WebGL"]` in its asmdef.
4. Do not change the WebGL UPM package asmdef; it already has
   `"includePlatforms": ["WebGL"]`.

## Run the scene

1. Build and start the server from `../Server`.

   ```bash
   ./run_sample.sh build
   ./run_sample.sh run
   cat .run/stream.port
   ```

2. Open `Assets/Scenes/EngineLobby.unity` in Unity.
3. Set the `ZLinkClient` GameObject's **Endpoint** to `ws://127.0.0.1:<port>` using the printed
   port.
4. Enter Play mode. The text changes from `joined as ...` to
   `unity-player: hello from Unity`.
5. After leaving Play mode, run `./run_sample.sh stop` in the server directory.

The `connect`, `pump`, `handler`, and `lifecycle` snippet markers in `ZLinkClient.cs` are the source
boundaries read by the engine integration guide.

## Build a native player

1. In **File → Build Profiles**, choose a Windows, macOS, Linux, Android, or iOS native profile.
2. Confirm in the Plugin Inspector that `Systems.Zlink.Stream.Connector.dll` supports that profile
   and does not support WebGL.
3. Build the single `EngineLobby` scene.
4. Run the player where it can reach the server endpoint and confirm the UI text changes to the
   chat notification.

## Build a WebGL player

1. Select **File → Build Profiles → Web**, then choose **Switch Platform**.
2. Confirm the native `Systems.Zlink.Stream.Connector.dll` is excluded and the
   `Systems.Zlink.Stream.Connector.WebGL` assembly is included.
3. Build the `EngineLobby` scene into `Build/`.
4. Serve the build through an HTTP server and open it in a browser; do not use `file://`.
5. Confirm that `ChatNotify` replaces the UI text. This result covers the IL2CPP reverse callback,
   UPM import, jslib linkage, and main-thread pump together.

## Unity 6000.0.83f1 verification

The licensed Editor imports the project after correcting the NuGetForUnity UPM ID. Native player
compilation remains blocked: `Zlink.Stream.Connector` 0.23.0 contains only a `net8.0` assembly,
which Unity Mono rejects with CS1705 (`System.Runtime` 8.0.0.0 versus 4.1.2.0).

The WebGL UPM package now contains stable `.meta` files for its assets. Its assembly definition
includes the Editor so that Unity resolves the sample component while building the scene. The
project also declares Unity's JSON serialization module. Using the local UPM source, the WebGL
player built in batch mode and
completed the lobby flow in headless Chromium against the engine server: `JoinRes` reported
`actorId=00000005 name=unity-player`, and `ChatNotify` reported
`actorId=00000005 name=unity-player text=hello from Unity`. The server recorded the connection.
The pinned Git package version has not been updated with these changes.
