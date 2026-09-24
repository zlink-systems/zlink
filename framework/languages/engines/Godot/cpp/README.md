**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Godot C++ sample

This Godot 4.4.1 GDExtension scene uses the public C++ Godot stream connector adapter.
`EngineLobbyNode` calls `dispatch()` from `_process`, so callbacks run on the Godot main thread.
Messages follow `PingReq` → `PingRes` → `JoinReq` → `JoinRes` → `ChatMsg` → `ChatNotify`.
Packet and JSON fields follow the [shared Engine Lobby contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md).

## Dependencies and installation

- Godot 4.4.1, matching `godot-cpp`, a C++20 compiler, and CMake 3.24 or later
- Linux: GCC 13 or later, with Ninja or Make
- Windows: Visual Studio 2022 C++ workload and PowerShell 7

This repository contains the Godot project. The platform prebuilt from the C++ framework
[v0.25.0 release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.25.0)
includes Core, the C++ binding, the stream connector, and their CMake packages. Build only
the existing Godot adapter from that release's source archive against the prebuilt. A monorepo
checkout and a separate package manager are unnecessary.

Run these commands from `cpp/` in this repository.

**Linux · WSL — bash**

```bash
mkdir -p .zlink
touch .zlink/.gdignore
base=https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v0.25.0
for asset in zlink-framework-cpp-0.25.0-linux-x64.tar.gz zlink-framework-cpp-0.25.0.tar.gz; do
  curl -fL "$base/$asset" -o ".zlink/$asset"
  curl -fL "$base/$asset.sha256" -o ".zlink/$asset.sha256"
  (cd .zlink && sha256sum -c "$asset.sha256")
  tar -xzf ".zlink/$asset" -C .zlink
done
git clone --branch godot-4.4.1-stable --depth 1 https://github.com/godotengine/godot-cpp.git .zlink/godot-cpp
```

**Windows — PowerShell 7**

```powershell
New-Item -ItemType Directory -Force .zlink | Out-Null
New-Item -ItemType File -Force .zlink/.gdignore | Out-Null
$base = 'https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v0.25.0'
foreach ($asset in @('zlink-framework-cpp-0.25.0-windows-x64.tar.gz', 'zlink-framework-cpp-0.25.0.tar.gz')) {
  curl.exe -fL "$base/$asset" -o ".zlink/$asset"
  curl.exe -fL "$base/$asset.sha256" -o ".zlink/$asset.sha256"
  $expected = ((Get-Content ".zlink/$asset.sha256" -Raw).Trim() -split '\s+')[0]
  if ((Get-FileHash ".zlink/$asset" -Algorithm SHA256).Hash -ine $expected) { throw "SHA256 mismatch: $asset" }
  tar -xzf ".zlink/$asset" -C .zlink
}
git clone --branch godot-4.4.1-stable --depth 1 https://github.com/godotengine/godot-cpp.git .zlink/godot-cpp
```

## Build

**Linux · WSL — bash**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/.zlink/zlink-framework-cpp-0.25.0-linux-x64" \
  -DGODOT_CPP_ROOT="$PWD/.zlink/godot-cpp" \
  -DZLINK_FRAMEWORK_CPP_SOURCE_ROOT="$PWD/.zlink/zlink-framework-cpp-0.25.0"
cmake --build build --target engine_lobby_godot -j8
```

**Windows — PowerShell 7**

```powershell
cmake -S . -B build `
  "-DCMAKE_PREFIX_PATH=$PWD/.zlink/zlink-framework-cpp-0.25.0-windows-x64" `
  "-DGODOT_CPP_ROOT=$PWD/.zlink/godot-cpp" `
  "-DZLINK_FRAMEWORK_CPP_SOURCE_ROOT=$PWD/.zlink/zlink-framework-cpp-0.25.0"
cmake --build build --config Release --target engine_lobby_godot --parallel 8
```

The GDExtension library and Windows runtime DLLs are placed in `bin/`.
`engine_lobby.gdextension` loads the platform library.

## Run and verify

1. Clone the separate [`zlink-engine-server` repository](https://github.com/zlink-systems/zlink-engine-server)
   and start the server using its [README](https://github.com/zlink-systems/zlink-engine-server/blob/main/README.md).
   Read its `.run/stream.port`.
2. If the port differs from 22700, set the `EngineLobbyNode` `endpoint` in `EngineLobby.tscn`
   to `ws://127.0.0.1:<stream.port>`.
3. Import the project once in Godot 4.4.1 and run the scene. Check the `Status` label and
   these Godot output lines. On Linux, set `GODOT` to the absolute path of the downloaded
   Godot 4.4.1 executable.

**Linux · WSL — bash**

```bash
GODOT=/path/to/Godot_v4.4.1-stable_mono_linux.x86_64
"$GODOT" --headless --editor --path . --quit-after 120
"$GODOT" --headless --path . --quit-after 600
```

**Windows — PowerShell 7**

```powershell
$godot = 'C:\path\to\Godot_v4.4.1-stable_mono_win64_console.exe'
$import = Start-Process -FilePath $godot -ArgumentList @('--headless', '--editor', '--path', '.', '--quit-after', '120') -WorkingDirectory $PWD -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput 'godot-import.log' -RedirectStandardError 'godot-import.err.log'
if ($import.ExitCode -ne 0) { throw "Godot import failed: $($import.ExitCode)" }
$scene = Start-Process -FilePath $godot -ArgumentList @('--headless', '--path', '.', '--quit-after', '600') -WorkingDirectory $PWD -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput 'godot-scene.log' -RedirectStandardError 'godot-scene.err.log'
if ($scene.ExitCode -ne 0) { throw "Godot scene failed: $($scene.ExitCode)" }
Get-Content godot-scene.log
```

```text
PingRes sentAtUnixMs=1000
JoinRes name=godot-player actorId=<server-assigned-id>
ChatNotify godot-player: hello from Godot C++
```

If the Godot output contains `Engine Lobby failed:`, inspect that error. The server log must
also show a new `client connected: <sessionId>` line while the scene runs.
Stop the server using its own README instructions.
