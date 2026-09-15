# C++ Quickstart — from an empty project to a first request

> **Contract owner for this chapter** — none. The formal API contract is in the
> [C++ spec](../common/spec/server/languages/cpp/README.en.md).

The project lives at
[`framework/languages/cpp/quickstart/`](../../../languages/cpp/quickstart/). The code blocks
below are read from those files when the site is built.

Without a location store, two processes name each other's endpoint directly and exchange one
request/reply. The next step is
[Installation and first run](guide/server/02-getting-started.en.md).

## Installation paths

C++ has three, and they reach the same place — pick by what the project already uses.

| Path | When |
|---|---|
| vcpkg | A project already on vcpkg |
| Conan | A project already on Conan |
| GitHub Release | No package manager. Three source archives built in order |

`zlink` is not in the official vcpkg registry or ConanCenter yet. Use the overlay port and
the recipes this repository ships.

### vcpkg

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

### Conan

```bash
git clone https://github.com/zlink-systems/zlink.git
conan create zlink/core/packaging/conan --version 1.1.0 --build=missing -s compiler.cppstd=gnu20
conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
```

Put `zlink-framework/0.14.0` in the consumer's `conanfile.txt` and run `conan install`.

### GitHub Release

Three archives built and installed in order — `core/vX.Y.Z` → `cpp/vX.Y.Z` →
`framework-cpp/vA.B.C`. The order and the exact commands are in the project's
[`README.md`](../../../languages/cpp/quickstart/README.md).

This path supplies the third-party dependencies itself. `nlohmann_json`, Boost, liblz4,
libprotobuf and OpenSSL come from distribution packages; `opentelemetry-cpp` has none and is
built from source. The framework links only `opentelemetry-cpp::api`, so
`-DOTELCPP_WITH_API_ONLY=ON` is a header-only build.

## Prerequisites

- CMake 3.20 or later, a C++20 compiler

## 1. The consumer's CMake

Once the three-stage install is done, this is all the consumer writes.

```cmake title="CMakeLists.txt"
--8<-- "framework/languages/cpp/quickstart/CMakeLists.txt"
```

## 2. Shared contract

Message types need `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`. The default JSON serializer finds
`to_json`/`from_json` by ADL, so a bare struct does not serialize.

```cpp title="Shared/messages.hpp"
--8<-- "framework/languages/cpp/quickstart/Shared/messages.hpp"
```

## 3. The handling side

`object_role` defaults to `server`, which requires a location store; this configuration sets
it to `none`. A `routing_id` is required, and so is `advertise_host` when binding to a
wildcard host.

```cpp title="Server/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Server/main.cpp"
```

## 4. The calling side

An HTTP handler does not receive route parameters as arguments. It takes an
`http_request_t` and reads `request.route_values`.

```cpp title="Client/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Client/main.cpp"
```

## 5. Run

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

## 6. Visual Studio 2022

On Windows the project can be opened in Visual Studio instead of driving CMake by hand. It
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

## What to carry over

| File | Content |
|---|---|
| `CMakeLists.txt` | `find_package(zlink_framework CONFIG REQUIRED)` and linking `zlink::framework` |
| `Shared/messages.hpp` | `packet_name` and `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` |
| `Server/main.cpp` | `add_route_mesh` → `listen` → `set_object_role(none)`, `set_routing_id`, `set_advertise_host` → handler registration |
| `Client/main.cpp` | The client role, `peer_connections().connect(...)`, `request_to_channel(...)` |

Replacing the manual connection with a location store is covered by
[10. Location](guide/server/10-location.en.md).
