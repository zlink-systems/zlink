**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Axmol sample

This Axmol 2.3.0 C++ client connects to the shared Engine Lobby server. `EngineLobbyScene` owns
the connector and calls `dispatch()` from a scheduled update. It displays the result of
`PingReq` → `PingRes` → `JoinReq` → `JoinRes` → `ChatMsg` → `ChatNotify` in the center of the screen
and in the log.
Before connecting, it subscribes to `ChatNotify`, the Engine Lobby push.

The [engine-lobby sample contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md) owns packet names and JSON fields.

## Prerequisites

- Axmol 2.3.0 source tree (`AX_ROOT`)
- C++20 compiler and CMake 3.20 or newer
- ZLink C++ framework package 0.22.0 (`zlink_framework` CMake config)
- `nlohmann_json` CMake package
- Connector source from `framework/languages/cpp/connector/engines/axmol`

Copy the connector source into `third_party/zlink_axmol_connector/` in the monorepo. Place the
same-version source there in the exported mirror. CMake links the public `zlink::stream_connector`
target.

```bash
mkdir -p third_party/zlink_axmol_connector
cp -a ../../cpp/connector/engines/axmol/. third_party/zlink_axmol_connector/
```

## Run the server and client

Start the shared server from `../Server` in the same checkout.

```bash
cd ../Server
./run_sample.sh build
./run_sample.sh run
cat .run/stream.port
```

The default endpoint is `ws://127.0.0.1:22700` in `Source/EngineLobbyScene.cpp`. Change that
constant if the server prints another port. Build on Linux with Axmol and the ZLink package
installed:

```bash
cd ../Axmol
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/zlink/package
cmake --build build --parallel 8
./build/EngineLobby
```

Confirm that the screen and log show `joined as axmol-player (...)`, followed by
`axmol-player: hello from Axmol`. Then run `../Server/run_sample.sh stop`. The `connect`, `pump`,
`handler`, and `lifecycle` markers identify source boundaries for the engine integration guide.

## Minimum source validation

Without Axmol, `Validation/axmol.h` substitutes only the display and scheduler types. This
command compiles the actual client source, the public Axmol connector header, and JSON calls in
one translation unit:

```bash
g++-13 -std=c++20 -Wall -Wextra -Werror -fsyntax-only \
  -I Validation -I Source \
  -I ../../cpp/connector/engines/axmol/include \
  Validation/ProtocolCompileCheck.cpp
```

This does not verify an Axmol engine link, scheduler execution, connector link, or server
communication. Axmol is absent from this workspace, so an engine build and run were not performed.

The scene subscribes to `ChatNotify` before connecting, then displays the push delivered through
the adapter callback.
