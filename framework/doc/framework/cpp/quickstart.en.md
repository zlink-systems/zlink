# C++ Quickstart — from Install to a First Request

!!! info "What you get from this chapter"

    You can install the packages and run a minimal project where two processes call each other.

The project lives at
[`framework/languages/cpp/quickstart/`](../../../languages/cpp/quickstart/). The code blocks
below are read from those files when the site is built. Without a location store, two processes
name each other's endpoint directly and exchange one request/reply.

## 0. Clone the examples repository

This chapter's project is `quickstart/` in the `zlink-cpp-examples` repository; `tutorial/`,
the program read by the feature guides, and `samples/` live beside it.

```bash
git clone https://github.com/zlink-systems/zlink-cpp-examples.git
cd zlink-cpp-examples/quickstart
```

`main` is the latest release plus the fixes merged since, with package versions pinned to that
release. An older release is the tag `vA.B.C` (`git checkout vA.B.C`). Send issues and PRs to
`zlink-systems/zlink`.

## 1. Installation

- CMake 3.20 or later, a C++20 compiler. The framework uses C++20 coroutines. The
  [IDE](#7-opening-it-in-an-ide) path reads the preset the bootstrap writes and needs 3.21 or later; `bootstrap.cmake` needs 3.24
- nlohmann_json, Boost, liblz4, libprotobuf, OpenSSL, opentelemetry-cpp

`zlink` is not in the official vcpkg registry or ConanCenter yet. This repository carries an
overlay port and Conan recipes as well, so there are three installation paths. **The GitHub
Release path is the one verified end to end**; the `bootstrap.cmake` in `quickstart/`, `tutorial/`
and `samples/` of `zlink-cpp-examples` automates it.

| Path | Where it stands |
|---|---|
| GitHub Release | Downloads and extracts this platform's one framework prebuilt. Core, the C++ binding, the framework libraries and `nlohmann_json` share one prefix |
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

`object_role` defaults to `none`; this configuration states it explicitly because it is a
channel-only node. A `routing_id` is required. Binding to `0.0.0.0` without `advertise_host` advertises the loopback of the same address
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

`bootstrap.cmake` replaces the installation in §1 and configures this project into `build/`. It
downloads this platform's framework prebuilt and extracts it into `.zlink/install/`; it builds
nothing. If `tutorial/` was bootstrapped first, reuse its prefix with
`cmake -DZLINK_ROOT=../tutorial/.zlink -P bootstrap.cmake`.

```bash
cd zlink-cpp-examples/quickstart
cmake -P bootstrap.cmake
cmake --build build --parallel

# Two terminals. Start the server first.
./build/quickstart_server
./build/quickstart_client

curl http://127.0.0.1:5083/hello/world
```

If you installed the three stages of §1 yourself, configure with
`cmake -S . -B build -DCMAKE_PREFIX_PATH=<framework install prefix>` instead of the bootstrap.
The response is `"hello, world"` with status 200.

## 7. Opening it in an IDE

It is a CMake project, so an IDE reads `CMakeLists.txt` and the preset as they are. `bootstrap.cmake`
records the exact arguments it configured this folder with (framework install prefix, C++
standard, generator) as the preset **`zlink`** in `CMakeUserPresets.json`, so **run
§6's `cmake -P bootstrap.cmake` once, open the folder and pick that preset** — that is all. No
environment variable is needed. The preset configures into the same `build/` as §6, so the IDE
continues from the terminal build. `CMakeUserPresets.json` is rewritten by every bootstrap and
ignored by git; it is not edited by hand — to change a value, run the bootstrap again. The
Windows preset is Release only: the prebuilt is built Release, and a Debug consumer cannot link
against it.

### 7.1 Visual Studio 2022 or 2026

The **Desktop development with C++** workload with its **C++ CMake tools for Windows** component is
required.

1. Run `cmake -P bootstrap.cmake` in a terminal (§6).
2. **File → Open → Folder** on `quickstart/`. No solution file is needed.
3. Pick **`zlink bootstrap (Release)`** in the configuration drop-down.
4. **Build → Build All.**
5. Choose `quickstart_server.exe` as the startup item and run it, then run `quickstart_client.exe`
   and check from a terminal.

    ```powershell
    curl http://127.0.0.1:5083/hello/world
    ```

### 7.2 Rider · CLion

The JetBrains IDEs (Rider with C++ support, CLion) also recognise the folder as a CMake project and
read the preset as a profile.

1. Run `cmake -P bootstrap.cmake` in a terminal (§6); run it inside WSL to build there.
2. **File → Open** on the `quickstart/` folder.
3. **Settings → Build, Execution, Deployment → CMake** lists the preset `zlink` as a profile. Enable
   it. The toolchain is Visual Studio on Windows, or the WSL toolchain when building under WSL.
4. After the CMake load, run configurations `quickstart_server` and `quickstart_client` appear.
   Run `quickstart_server`, then `quickstart_client`, and check from a terminal.

    ```bash
    curl http://127.0.0.1:5083/hello/world
    ```

Stop with the IDE's Stop button.

## 8. What to check when the first run fails

| Symptom | What to check |
| --- | --- |
| `find_package` fails | Check that `cmake -P bootstrap.cmake` ended with `bootstrap complete` and that `.zlink/install/lib/cmake/zlink_framework/` exists. If you installed by hand, check that `CMAKE_PREFIX_PATH` points at the framework install prefix |
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
