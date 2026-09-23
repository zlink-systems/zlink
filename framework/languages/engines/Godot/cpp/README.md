**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Godot C++ sample

This Godot 4.4 GDExtension scene uses the existing C++ Godot stream connector adapter. Its
`EngineLobbyNode` calls `dispatch()` in `_process`, so callbacks update the `Status` label on the
Godot main thread. The intended flow is `PingReq` → `PingRes` → `JoinReq` → `JoinRes` → `ChatMsg` →
`ChatNotify`. Packet names and JSON fields follow the [shared Engine Lobby contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md).
The node subscribes to the `ChatNotify` push before connecting.

## Dependencies and build

- Godot 4.4 editor and matching `godot-cpp` 4.4 source and headers
- C++20 compiler, CMake, and the C++ connector's package dependencies
- This monorepo checkout, including `framework/languages/cpp`

Build from Linux with the same Godot minor version for the editor and `godot-cpp`:

```bash
cmake -S . -B build \
  -DGODOT_CPP_ROOT=/path/to/godot-cpp \
  -DZLINK_FRAMEWORK_CPP_ROOT=/path/to/zlink/framework/languages/cpp \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
cmake --build build --target engine_lobby_godot -j8
```

The resulting shared library goes into `bin/`, where `engine_lobby.gdextension` loads it. This
project currently declares a Linux x86_64 library; other platforms need a matching library entry
and build. The C++ connector is linked from source in the same build, including its WebSocket
transport.

## Run

1. Start [`../../Server`](../../Server) and read `.run/stream.port`. Its `run_sample.sh` provisions
   Redis when Docker is available.
2. Open `project.godot` in Godot 4.4. Set the `EngineLobbyNode` `endpoint` to
   `ws://127.0.0.1:<stream.port>` if the assigned port differs from 22700.
3. Run the scene. The label should change from a joined status to
   `godot-player: hello from Godot C++`.
4. Stop the server with `./run_sample.sh stop` if you started it through that runner.

## Current validation and blockers

The C++ node and GDExtension registration translation units compile with GCC 13 against the real
Godot adapter header and a thin stand-in for Godot C++ headers. A deliberately misspelled
`dispatch()` call fails that compile check. Godot, `godot-cpp`, and a runnable GDExtension are not
installed here, so an editor build, scene run, and rendered label were not verified.

The node subscribes to `ChatNotify` before connecting. The adapter delivers that push through the
registered packet callback.

The C# variant under [`../csharp`](../csharp) did run against the real shared server in WSL: two
clients verified Ping, Join, and both ChatNotify payloads. A connection to an unused port failed as
expected. This check does not exercise the C++ adapter or the Godot scene.
