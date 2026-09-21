[English](./README.md) | [한국어](./README.ko.md)

# C++ Quickstart

The smallest project: two processes that call each other once over a channel, with no location
store — the client names the server's endpoint directly. The site page
`framework/doc/framework/cpp/quickstart.ko.md` reads its code blocks from these files.

This directory is `quickstart/` in the `zlink-cpp-examples` repository. It installs the same way
as `tutorial/` and `samples/`: one `bootstrap.cmake`, the published Core prebuilt, and the
binding and framework source archives from GitHub Releases plus vcpkg.

| | Purpose |
|---|---|
| **quickstart** (here) | Install through the first reply. Adds no features |
| tutorial (`tutorial/`) | Adds features one at a time. The feature guides read this code |
| samples (`samples/`) | Applications with a complete business flow |

## Prerequisites

The same as the tutorial's, minus Docker (this project uses no Redis):

| Tool | Windows | Linux / WSL |
|---|---|---|
| C++20 compiler | Visual Studio 2022 17.4 or later with the **Desktop development with C++** workload | GCC 13 or later |
| CMake | 3.24 or later | 3.24 or later |
| Ninja | not needed | recommended; Makefiles are used when it is absent |
| vcpkg | the copy Visual Studio installs is found automatically; for a separate clone set `VCPKG_ROOT` | `git clone https://github.com/microsoft/vcpkg` and `./bootstrap-vcpkg.sh`; set `VCPKG_ROOT` unless it lives at `$HOME/vcpkg` |

vcpkg builds the third-party libraries from source, so **the first install takes about 20
minutes**. If `tutorial/` or `samples/` was bootstrapped already, reuse its tree instead of
building again: `cmake -DZLINK_ROOT=../tutorial/.zlink -P bootstrap.cmake`.

## Download and install

Clone the [`zlink-cpp-examples`](https://github.com/zlink-systems/zlink-cpp-examples) repository.
Every command below runs inside its `quickstart/` directory.

`bootstrap.cmake` does the install — it is the first line of the [Build](#build) block. It
downloads this platform's Core prebuilt, the C++ binding source and the framework source from
GitHub Releases, builds the binding and the framework into `.zlink/install/`, and configures this
project into `build/`. Only the framework version is written in the script; the Core and binding
versions come from the framework archive. From the second run on it reuses what it downloaded
and built. To start over, delete `.zlink/` and `build/`.

## Build

```bash title="linux"
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
cmake -P bootstrap.cmake
cmake --build build --parallel
```

```powershell title="windows"
cmake -P bootstrap.cmake
cmake --build build --config Release --parallel
```

Two executables come out — under `build\Release\` on Windows, `build/` on Linux.

## Run

The server listens on `tcp://0.0.0.0:7301`; the client listens on `7302`, connects to the
server, and serves `GET /hello/{name}` on `http://127.0.0.1:5083`.

```bash title="linux"
./build/quickstart_server > server.log 2>&1 &
./build/quickstart_client > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5083/hello/world > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:5083/hello/world
```

```powershell title="windows"
Start-Process -NoNewWindow .\build\Release\quickstart_server.exe -RedirectStandardOutput server.out -RedirectStandardError server.log
Start-Process -NoNewWindow .\build\Release\quickstart_client.exe -RedirectStandardOutput client.out -RedirectStandardError client.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5083/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
$answer
```

## Verify

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5083/hello/world | grep -q '"hello, world"'
echo "quickstart=ok"
```

```powershell title="windows"
if ((curl.exe -s http://127.0.0.1:5083/hello/world) -notmatch '"hello, world"') { throw 'quickstart failed' }
Write-Output 'quickstart=ok'
```

The answer is `"hello, world"` with status 200.

## Troubleshooting

The install-time symptoms (`vcpkg was not found`, baseline errors, download failures, missing
compiler, `RuntimeLibrary` mismatch, `STATUS_DLL_NOT_FOUND`) are the tutorial's; see its
README. Specific to this project:

| Symptom | Cause and fix |
|---|---|
| `bind: Address already in use` / `Only one usage of each socket address` | Another process holds 7301, 7302 or 5083 — a `quickstart_server` or `quickstart_client` left from an earlier run |
| `curl: (7) Failed to connect to 127.0.0.1 port 5083` | The client is not up yet, or died. Read `client.log` |
| The call ends with no target | The server is not up, or the client's `peer_connections().connect(...)` names a different endpoint than the server's `listen(...)` |

## Project layout

| Directory | Contents |
|---|---|
| `Shared/` | The message contract (`hello_t`, `greeting_t`) both processes compile |
| `Server/` | Registers the `greeting` channel handler and listens on `7301` |
| `Client/` | Connects to the server, exposes `GET /hello/{name}`, calls `greeting` |
| `bootstrap.cmake` | The install script; identical to the tutorial's and the samples' |
| `CMakePresets.json` | Presets for Visual Studio; `CMAKE_PREFIX_PATH` comes from `ZLINK_PREFIX` |

## What to carry into your own project

- `bootstrap.cmake` and the `find_package(zlink_framework CONFIG REQUIRED)` +
  `target_link_libraries(... zlink::framework)` pair in `CMakeLists.txt`.
- `Shared/messages.hpp`'s `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` pattern for message contracts.
- `Server/main.cpp`'s `add_route_mesh(...).listen(...).set_object_role(none).set_routing_id(...)`
  block — every call is required for a channel-only node.
- `Client/main.cpp`'s `peer_connections().connect(...)` and the HTTP handler shape. A real
  service moves from manual connection to a location store (Redis); the tutorial's Location step
  shows that.
