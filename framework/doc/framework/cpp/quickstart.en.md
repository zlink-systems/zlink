# C++ Quickstart — from Install to a First Request

!!! info "What you get from this chapter"

    You can install the packages and run a minimal project where two processes call each other.

The project lives at
[`framework/languages/cpp/quickstart/`](../../../languages/cpp/quickstart/). The code blocks
below are read from those files when the site is built. Without a location store, two processes
name each other's endpoint directly and exchange one request/reply.

## 0. Downloading the tutorial

This chapter builds the smallest project from scratch. **To run the finished tutorial instead**,
one archive is all you need — there is no reason to clone the whole repository.

!!! tip "Download the tutorial"

    [:material-download: **zlink-tutorial-cpp.zip**](https://github.com/zlink-systems/zlink/releases/latest/download/zlink-tutorial-cpp.zip){ .md-button .md-button--primary }


The address does not depend on the platform: Windows and WSL fetch the same file. Unpacking it
leaves the project under `zlink-tutorial-cpp/`, whose `bootstrap.cmake` replaces the three-archive
install of section 1.3 below: one `cmake -P bootstrap.cmake` installs the framework and configures
the project. Neither the repository nor Python is needed. The procedure and its troubleshooting
are in the `README.md` inside.

For the current main, take just that directory out of the repository.

```bash
git clone --filter=blob:none --sparse https://github.com/zlink-systems/zlink.git
cd zlink
git sparse-checkout set framework/languages/cpp/tutorial
```

## 1. Installation

- CMake 3.20 or later, a C++20 compiler. The framework uses C++20 coroutines. The
  [Visual Studio 2022](#7-visual-studio-2022) path reads `CMakePresets.json` and needs 3.21 or later
- nlohmann_json, Boost, liblz4, libprotobuf, OpenSSL, opentelemetry-cpp

`zlink` is not in the official vcpkg registry or ConanCenter yet. This repository carries an
overlay port and Conan recipes as well, so there are three installation paths. **The GitHub
Release path is the one verified end to end**; the tutorial and samples archives' `bootstrap.cmake`
automates it.

| Path | Where it stands |
|---|---|
| GitHub Release | One Core prebuilt and two source archives installed in order. Verified end to end on Windows and Linux |
| vcpkg overlay port | Refreshed by `sync-recipes` at every release. Outside what this page verifies |
| Conan recipe | Refreshed by `sync-recipes` at every release. Outside what this page verifies |

The commands of the archive path are in the project's
[`README.md`](../../../languages/cpp/quickstart/README.md).

### 1.1 vcpkg

!!! note "Outside the verified path"

    This path is refreshed at every release but is not the one this page verified. Use
    [GitHub Release](#13-github-release) for a first install.

```bash
git clone https://github.com/zlink-systems/zlink.git
vcpkg install zlink zlink-cpp zlink-framework \
  --overlay-ports=zlink/vcpkg/ports --triplet=x64-linux
```

The consumer project uses the vcpkg toolchain.

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

### 1.2 Conan

!!! note "Outside the verified path"

    This path is refreshed at every release but is not the one this page verified. Use
    [GitHub Release](#13-github-release) for a first install.

```bash
git clone https://github.com/zlink-systems/zlink.git
conan create zlink/core/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
```

Put `zlink-framework/0.16.0` in the consumer's `conanfile.txt` and run `conan install`.

### 1.3 GitHub Release

Three archives built and installed in order — `core/vX.Y.Z` → `cpp/vX.Y.Z` →
`framework-cpp/vA.B.C`. The order and the exact commands are in the project's
[`README.md`](../../../languages/cpp/quickstart/README.md).

On this path the third-party dependencies are installed by hand. `nlohmann_json`, Boost, liblz4,
libprotobuf and OpenSSL come from distribution packages; `opentelemetry-cpp` has none and is
built from source. The framework links only `opentelemetry-cpp::api`, so
`-DOTELCPP_WITH_API_ONLY=ON` is a header-only build.

### 1.4 Targets to add when you need them

| Target | When to add it |
| --- | --- |
| `zlink::framework_locations_redis` | When using the Redis location store for auto-connect ([Location](guide/server/25-location.en.md)) |
| `zlink::framework_codec_protobuf` · `_messagepack` | To use instead of the default JSON codec ([Handlers and Message Processing](guide/server/31-handler-dispatch.en.md#3-codecs--turning-a-payload-into-bytes)) |
| `zlink::stream_connector` | When building an external client (a game client, mobile) ([STREAM](guide/server/23-stream.en.md)) |
| `zlink::http_client` | When the server calls out over HTTP ([HTTP Client guide](guide/http-client/README.en.md)) |

The license differs by layer — core/binding is MPL-2.0, framework is FSL-1.1-ALv2, and
`zlink::http_client` is Apache-2.0. There is no cost to building and selling a service
([Where ZLink Applies](guide/server/17-alternative.en.md#8-license--the-cost-of-using-it)).

## 2. The consumer's CMake

Once the three-stage install is done, this is all the consumer writes.

```cmake title="CMakeLists.txt"
--8<-- "framework/languages/cpp/quickstart/CMakeLists.txt"
```

## 3. Shared contract

Message types need `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`. The default JSON serializer finds
`to_json`/`from_json` by ADL, so a bare struct does not serialize.

```cpp title="Shared/messages.hpp"
--8<-- "framework/languages/cpp/quickstart/Shared/messages.hpp"
```

## 4. The handling side

`object_role` defaults to `server`, which requires a location store; this configuration sets
it to `none`. A `routing_id` is required. Binding to `0.0.0.0` without `advertise_host` advertises the loopback of the same address
family (`127.0.0.1`). In containers or multi-host deployments where remote
processes cannot use that loopback, set `advertise_host` to a reachable address.

```cpp title="Server/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Server/main.cpp"
```

## 5. The calling side

An HTTP handler does not receive route parameters as arguments. It takes an
`http_request_t` and reads `request.route_values`.

```cpp title="Client/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Client/main.cpp"
```

## 6. Run

```bash
cd framework/languages/cpp/quickstart
cmake -S . -B build -DCMAKE_PREFIX_PATH=<framework install prefix>
cmake --build build

# Two terminals. Start the server first.
./build/quickstart_server
./build/quickstart_client

curl http://127.0.0.1:5083/hello/world
```

The response is `"hello, world"` with status 200.

## 7. Visual Studio 2022

On Windows the project can be opened in Visual Studio instead of running the CMake commands
directly. It
needs the **Desktop development with C++** workload and its **C++ CMake tools for Windows**
component.

1. Complete the three-stage install first. The order is the same on Windows; run it from
   PowerShell.
2. Put the framework install prefix in an environment variable. Visual Studio reads it as the
   preset's `CMAKE_PREFIX_PATH`.

    ```powershell
    $env:ZLINK_PREFIX = "C:\zlink\install\framework"
    ```

3. **File → Open → Folder** on `framework/languages/cpp/quickstart`. No solution file is
   needed; Visual Studio reads `CMakePresets.json`.
4. Pick **`Visual Studio 2022 (x64)`** in the configuration dropdown, or
   **`Visual Studio 2022 (x64, vcpkg toolchain)`** if the dependencies came from vcpkg — that
   one reads `VCPKG_ROOT`.
5. **Build → Build All.**
6. Select `quickstart_server` as the startup item and run it, then start the client from a
   separate Developer PowerShell. Only one startup item can be debugged at a time, so the two
   processes are not both launched from Visual Studio.

    ```powershell
    .\build\vs2022\Debug\quickstart_client.exe
    curl http://127.0.0.1:5083/hello/world
    ```

The presets live in the project's `CMakePresets.json`. To change a value, add a
`CMakeUserPresets.json` next to it rather than editing that file — the user file is not
version controlled.

```json title="CMakePresets.json"
--8<-- "framework/languages/cpp/quickstart/CMakePresets.json"
```

## 8. What to check when the first run fails

| Symptom | What to check |
| --- | --- |
| `find_package` fails | Check that `CMAKE_PREFIX_PATH` points at the framework install prefix and that all three install stages completed |
| The server requires a location store | Check that `set_object_role` is set to `none` |
| Startup fails | Check that both processes name the same mesh and that `routing_id` is set |
| A message does not serialize | Check that the message type carries `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` |
| A call ends with no target | Check that the receiving side registered that channel name in the server role, and that the two processes are connected as peers |

## 9. What to carry over

| File | Content |
|---|---|
| `CMakeLists.txt` | `find_package(zlink_framework CONFIG REQUIRED)` and linking `zlink::framework` |
| `Shared/messages.hpp` | `packet_name` and `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` |
| `Server/main.cpp` | `add_route_mesh` → `listen` → `set_object_role(none)`, `set_routing_id`, `set_advertise_host` → handler registration |
| `Client/main.cpp` | The client role, `peer_connections().connect(...)`, `request_to_channel(...)` |

## 10. What to read next

These two processes connect by writing each other's endpoint directly. Keeping the calling code
unchanged while servers are added or restarted at another address needs automatic connection, and
that is covered by [Location](guide/server/25-location.en.md).

- To go over the concepts first — [Core Concepts](guide/server/03-concepts.en.md)
- The path that calls by name — [Channel Messaging](guide/server/20-channel-messaging.en.md)
- State objects called by id — [Spot](guide/server/21-spot.en.md) · [Actor](guide/server/22-actor.en.md)
- To see a complete business flow — [Picking a Sample](guide/server/14-samples.en.md)
