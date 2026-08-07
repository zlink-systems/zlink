# 10 — Packaging

[← Engine Adapters](09-engine-adapters.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Performance Testing →](11-performance.en.md)

---

## Deployment Units

| Artifact | vcpkg Package Name | Conan Package Name | Distribution Format |
|--------|----------------|----------------|-----------|
| core connector | `zlink-stream-connector` | `zlink-stream-connector` | CMake, vcpkg, Conan |
| e2e client | `zlink-stream-e2e-client` | `zlink-stream-e2e-client` | CMake, vcpkg, Conan |
| Unreal plugin | — | — | source plugin + generated ThirdParty package |
| Godot adapter | — | — | source GDExtension |
| Axmol adapter | — | — | source package |

---

## vcpkg

### Installation

Default install (TCP, JSON):

```bash
vcpkg install zlink-stream-connector
```

To add features, use bracket notation:

```bash
vcpkg install "zlink-stream-connector[tls,websocket,lz4]"
```

Installing the e2e client together:

```bash
vcpkg install zlink-stream-connector zlink-stream-e2e-client
```

### Supported Features

| Feature | Content | Dependency |
|---------|------|--------|
| `tls` | TLS over TCP / WSS transport | OpenSSL |
| `websocket` | WebSocket / WSS transport | (includes Boost.Beast) |
| `lz4` | LZ4 packet compression | LZ4 |

### CMakeLists.txt

```cmake
find_package(zlink-stream-connector CONFIG REQUIRED)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

---

## Conan

### conanfile.txt

```ini
[requires]
zlink-stream-connector/0.10.0

[options]
zlink-stream-connector/*:with_tls=True
zlink-stream-connector/*:with_websocket=True
zlink-stream-connector/*:with_lz4=True
```

### conanfile.py

```python
from conan import ConanFile

class MyGameConan(ConanFile):
    requires = "zlink-stream-connector/0.10.0"
    options = {"zlink-stream-connector/*:with_tls": True,
               "zlink-stream-connector/*:with_websocket": True}
```

### Conan Options

| Option | Default | Meaning |
|--------|--------|------|
| `with_tls` | `False` | TLS/WSS transport |
| `with_websocket` | `False` | WebSocket/WSS transport |
| `with_lz4` | `True` | LZ4 compression |

---

## CMake FetchContent

Builds directly from source, with no vcpkg/Conan.

```cmake
include(FetchContent)

FetchContent_Declare(zlink_stream_connector
    GIT_REPOSITORY https://github.com/zlink-systems/zlink.git
    GIT_TAG        main
    SOURCE_SUBDIR  framework/languages/cpp/connector/core
)
set(ZLINK_STREAM_CONNECTOR_WITH_TLS        ON  CACHE BOOL "")
set(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET  ON  CACHE BOOL "")
set(ZLINK_STREAM_CONNECTOR_WITH_LZ4        ON  CACHE BOOL "")

FetchContent_MakeAvailable(zlink_stream_connector)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

### CMake Build Options

| Option | Default | Meaning |
|--------|--------|------|
| `ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT` | `ON` | includes the e2e client build |
| `ZLINK_STREAM_CONNECTOR_WITH_TLS` | `ON` | TLS/WSS transport |
| `ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET` | `ON` | WebSocket/WSS transport |
| `ZLINK_STREAM_CONNECTOR_WITH_LZ4` | `ON` | LZ4 compression |
| `ZLINK_STREAM_CONNECTOR_BUILD_UNREAL` | `ON` | builds the Unreal adapter |
| `ZLINK_STREAM_CONNECTOR_BUILD_GODOT` | `OFF` | builds the Godot adapter |
| `ZLINK_STREAM_CONNECTOR_BUILD_AXMOL` | `OFF` | builds the Axmol adapter |

---

## CMake Target Composition

| Target | Public Include | Purpose |
|--------|---------------|------|
| `zlink::stream_connector` | `zlink/stream_connector.hpp` | core connector |
| `zlink::stream_e2e_client` | `zlink/stream_e2e_client.hpp` | e2e/perf scenario |

The two targets are mutually independent. `zlink::stream_e2e_client` uses
`zlink::stream_connector` as a public or private dependency.

---

## Engine Adapter Distribution

The Unreal, Godot, and Axmol adapters are distributed as source packages, not through vcpkg/Conan.

| Adapter | Distribution Method | Description |
|--------|---------|------|
| Unreal | source plugin (`.uplugin`) + generated native package | placed under `Plugins/ZLinkStreamConnector/` with `ThirdParty/ZLink/` |
| Godot | source GDExtension | placed under `addons/zlink_stream_connector/` |
| Axmol | CMake source | `third_party/` or FetchContent |

Each adapter either includes the core connector as an internal ThirdParty or references an installed
CMake package. A user of an adapter package doesn't need to install the server framework package.

Unreal Build Tool cannot consume the CMake target graph directly, so
`Tools/package-third-party.cmake` creates a native package from a configured CMake build.
The command below stages the Unreal adapter, C++ binding, Core runtime and Core CMake package,
selected OpenSSL/LZ4 libraries, and a relative manifest under `ThirdParty/ZLink/`.

```bash
cmake \
  -DZLINK_UNREAL_BUILD_DIR=/absolute/path/to/framework/languages/cpp/build \
  -DZLINK_UNREAL_OUTPUT_DIR=/absolute/path/to/ThirdParty/ZLink \
  -DZLINK_UNREAL_CONFIGURATION=Release \
  -P framework/languages/cpp/connector/engines/unreal/Tools/package-third-party.cmake
```

`ZLinkStreamConnector.Build.cs` reads the manifest, validates the include paths,
link libraries, runtime files, and system libraries, and registers them with the Unreal module.
Set `ZLINK_UNREAL_THIRDPARTY_ROOT` to use a package outside the plugin directory. Windows
system libraries such as `ws2_32` and `mswsock`, together with the TLS system dependencies,
are recorded in the manifest. This supplies build/package dependencies without adding
platform branches to the Asio transport runtime. The script installs only the
`StreamConnector` CMake component, records platform/architecture/configuration/compiler
metadata, and rejects a package whose target does not match the Unreal build. The installed
CMake export uses package-relative dependency paths and includes the Windows Core import
library, so a clean consumer does not depend on the producer's build directory.

Before invoking Unreal Build Tool, set `ZLINK_UNREAL_COMPILER_ID` and
`ZLINK_UNREAL_COMPILER_VERSION` to the actual compiler values selected by the Unreal target.
Both values must match the manifest. They are required so a package built with a different
compiler or CRT is rejected instead of being linked silently.
The module uses the `ReadOnlyTargetRules` `Platform`, `Architecture`, and `Configuration`
properties; run the package through the Unreal version's actual UBT to verify that API surface.

---

## Dependency Summary

| Dependency | Purpose | Default Inclusion |
|--------|------|-----------|
| Boost.Asio | asynchronous I/O runtime | always |
| Boost.Beast | WebSocket | `WITH_WEBSOCKET` |
| nlohmann/json | JSON codec | always |
| OpenSSL | TLS/WSS | `WITH_TLS` |
| LZ4 | compression (system LZ4 or fallback source) | `WITH_LZ4` |
| msgpack-cxx | MessagePack codec | `WITH_MESSAGEPACK` |
| protobuf | Protobuf codec | `WITH_PROTOBUF` |
