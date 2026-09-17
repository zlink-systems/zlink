# ZLink C++ quickstart

The same scenario as `common/guide/server/02-getting-started.ko.md` §2 "minimal
example -- two processes call each other", built and actually run with only
**published GitHub Release source archives** -- no repository source, no vcpkg
overlay port, no local package cache. Two processes exchange endpoints
directly (no Redis, no location store) and complete one request/reply.

## Why this took three builds, not one `dotnet add package`

C++ has no package registry for this stack yet (no vcpkg/ConanCenter entry
for Core; PR not merged). The only path a repository-external user has today
is compiling three GitHub Release *source* archives in dependency order, each
producing its own install prefix that the next stage points at:

| Stage | Release tag | Archive | Produces |
| --- | --- | --- | --- |
| 1. Core | `core/v1.1.0` | `zlink-1.1.0-source.tar.gz` | `libzlink.so`, `zlinkConfig.cmake` |
| 2. C++ binding | `cpp/v1.1.0` | `zlink-cpp-1.1.0.tar.gz` | `libzlink_cpp.a`, `zlink_cppConfig.cmake` |
| 3. C++ framework | `framework-cpp/v0.12.0` | `zlink-framework-cpp-0.12.0.tar.gz` | `libzlink_framework.a`, `zlink_frameworkConfig.cmake` |

Versions are pinned `EXACT` end to end: the binding archive's
`CMakeLists.txt` requires Core `1.1.0 EXACT`, and the framework archive's
`CMakeLists.txt` requires the binding `1.1.0 EXACT`. All three tags published
on 2026-09-15 satisfy that chain -- no version had to be forced.

**Answer to the question this job exists to answer:** three source builds,
about **6-7 minutes total on a 20-core machine** (Core ~50s, binding ~5s,
opentelemetry-cpp API-only ~instant, framework ~110s, this quickstart app
~10s), plus a fourth build for a third-party dependency (opentelemetry-cpp)
that ships in no Linux distribution package and has no on/off switch in the
framework's own dependency table. That is the number that should inform
whether Core gets bundled into the binding archive (#377): a C++ user pays
for a from-source build pipeline every time, not just a slow download.

## The three paths that exist today, tried in practice (2026-09-15)

A C++ user has three theoretical ways to obtain this stack. This repository
has its own `vcpkg/ports/{zlink,zlink-cpp,zlink-framework}` overlay ports and
`{core,bindings/cpp,framework/languages/cpp}/packaging/conan` recipes, so
"no official registry entry" does not automatically mean "unusable" -- an
overlay port or a locally exported recipe can work without a registry PR.
Both were tried end to end, not just read.

| Path | Result | Where it breaks |
| --- | --- | --- |
| vcpkg overlay port (`--overlay-ports=vcpkg/ports`) | **Does not work** | 4 independent bugs stacked in the 3 ports; the last is a structural mismatch, not a data-entry error |
| Conan recipe (`conan export`/`create` on the 3 `packaging/conan` dirs) | **Does not work** | 4 independent bugs; one recipe's own docstring calls itself a draft |
| GitHub Release source archive (three-stage build below) | **Works** | Verified end to end, see the rest of this file |

### Path 1: vcpkg overlay port

Commands (vcpkg 2026-07-27, overlay port pointed at this repo's `vcpkg/ports`,
triplet `x64-linux`, install root under `/tmp`):

```bash
vcpkg install zlink --overlay-ports=<repo>/vcpkg/ports --triplet=x64-linux ...
vcpkg install zlink-cpp --overlay-ports=<repo>/vcpkg/ports --triplet=x64-linux ...
vcpkg install zlink-framework --overlay-ports=<repo>/vcpkg/ports --triplet=x64-linux ...
```

- **`zlink` (Core) port: builds.** `core/v0.18.0` downloads, configures, and
  installs in ~1.2 min. The port pins `zlink.vcpkg.json` version `0.18.0`,
  which matches an existing release tag.
- **`zlink-cpp` port: fails as published.** `vcpkg/ports/zlink-cpp/portfile.cmake`
  pins a `SHA512` for `cpp/v1.1.0`'s tarball that does not match the archive
  actually published at that URL today:
  ```
  error: download from .../cpp/v1.1.0/zlink-cpp-1.1.0.tar.gz had an unexpected hash
  note: Expected: 5c77a326...
  note: Actual  : d278785a...
  ```
  After hand-correcting that hash (scratch copy of the port, repository file
  untouched), the same portfile hardcodes `-DZLINK_CPP_CORE_VERSION=0.17.5`,
  which does not match `0.18.0`, the Core version the sibling `zlink` port
  line actually installs:
  ```
  CMake Error at CMakeLists.txt:126 (message):
    Core release prefix has neither an exact zlink 0.17.5 package config
    nor package provenance: <installed-dir>
  ```
  Correcting that pin to `0.18.0` as well, the port builds in ~6s.
- **`zlink-framework` port: fails as published, and cannot be made to work
  by editing the port alone.** `vcpkg.json` pins version `0.11.1`; the
  portfile downloads `framework-cpp/v0.11.1`, a release tag that does not
  exist (`error: curl operation failed with response code 404`; the current
  release is `framework-cpp/v0.12.0`, published the same day). After
  hand-bumping the version, URL, and `SHA512` to `0.12.0` and adding the two
  options the port is missing (`-DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX`
  and `..._CORE_PREFIX=${CURRENT_INSTALLED_DIR}` -- without them the build
  silently falls back to a repository-relative dev path,
  `.artifacts/wsl/install/...`, that does not exist in a vcpkg tree), the
  build reaches a wall an overlay port cannot patch around:
  ```
  CMake Error at CMakeLists.txt:758 (message):
    The installed Core package has no CMake config under
    <installed-dir>/lib/cmake/zlink
  ```
  `framework/languages/cpp/CMakeLists.txt`'s install step looks for the local
  Core package's CMake config directly under `<prefix>/lib/cmake/zlink`, but
  vcpkg's own packaging policy (`vcpkg_cmake_config_fixup`) relocates every
  package's CMake config to `<prefix>/share/<pkg>`. No portfile edit changes
  that vcpkg policy or that CMakeLists.txt lookup path -- fixing this needs a
  code change in the framework's own install() rules. **The known #384 defect
  (`libzlink.a` not installed under `BUILD_STATIC=ON`) was never reached** --
  this path fails three steps earlier.
- Total command time across all three ports, including the failed attempts:
  under 3 minutes (only the two source downloads and the two successful
  builds take real time; every failure is caught at configure time).

### Path 2: Conan recipe (local export, no ConanCenter PR)

Conan 2.32.0. All three recipes export cleanly (`conan export <dir>`), so the
recipes themselves are syntactically fine; the problems are in what they
point at and what they assume.

```bash
conan create core/packaging/conan --version 0.17.5 --build=missing -o "zlink/*:shared=True"
conan create bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan install framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
```

- **Core (`zlink/0.17.5`): builds and its own `test_package` passes**, ~42s
  total. Caveat: `core/packaging/conan/conandata.yml` has **no entry for the
  real current Core release, `1.1.0`** -- the newest version this recipe can
  build at all is `0.18.0`. `1.1.0` cannot be requested through this recipe
  today regardless of anything else.
- **C++ binding (`zlink-cpp/1.1.0`): fails as published, same root cause as
  vcpkg's.** `bindings/cpp/packaging/conan/conanfile.py`'s own docstring:
  *"Draft: the public source asset must be completed before conan create
  works."* Its `conandata.yml` sha256 for `cpp/v1.1.0` does not match the
  published archive:
  ```
  ConanException: sha256 hash failed for 'zlink-cpp-1.1.0.tar.gz' file.
  Provided hash: e581c85f76c9...
  Computed hash: 9b762c924238...
  ```
  After correcting the hash (scratch copy), a second, independent bug
  surfaces: the recipe's `requirements()` hardcodes `self.requires("zlink/0.17.5", ...)`,
  but never tells the archive's CMake build that Core is `0.17.5` -- the
  archive's `CMakeLists.txt` defaults to requiring an exact Core `1.1.0`, so
  configure fails:
  ```
  CMake Error at CMakeLists.txt:222 (message):
    No zlink core runtime found for this platform.  Expected an installed
    Core 1.1.0 package, ...
  ```
  This is the same defect class as vcpkg's `ZLINK_CPP_CORE_VERSION`
  mismatch, just baked into the recipe instead of exposed as a stray CMake
  option -- Conan's `CMakeDeps` generator has no way to fix it since the
  archive's CMakeLists.txt does not consult it for this check.
- **Framework (`zlink-framework/0.11.1`): fails as published, same root cause
  as vcpkg's, and unreachable behind the C++ binding bug above.**
  `framework/languages/cpp/packaging/conan/conanfile.py`'s own docstring:
  *"Draft CMake-only recipe; requires the complete prepared C++ source
  archive."* Its `conandata.yml` points at `framework-cpp/v0.11.1`, the same
  nonexistent release tag the vcpkg port points at (confirmed 404). A
  180s-bounded `conan install` on the unmodified recipe never even reaches
  that download: every other third-party dependency (Boost, nlohmann_json,
  OpenSSL, lz4, redis-plus-plus, libuv) resolves from ConanCenter's prebuilt
  binaries and Protobuf 5.27.0 builds from source in about two minutes, but
  the graph then tries to build `zlink-cpp/1.1.0` as a transitive dependency
  and fails on the exact Core-version mismatch described above.
- Total command time across the three recipes, including failed attempts and
  the one from-source Protobuf build: under 5 minutes.

### Why this matters

Both non-archive paths fail on the **same two classes of bug**, independently
in each packaging system: a stale checksum for the `cpp/v1.1.0` asset, and a
Core-version pin (`0.17.5` in one place, `1.1.0` in another, `0.18.0` for what
actually gets built) that was never reconciled end to end. The vcpkg
`zlink-framework` port additionally points at a release tag that was never
published, exactly like the Conan `zlink-framework` recipe. Neither problem
is specific to vcpkg or Conan -- both packaging systems are exposing the same
underlying inconsistency in this repository's own version bookkeeping across
`core/packaging/conan`, `bindings/cpp/packaging/conan`,
`framework/languages/cpp/packaging/conan`, and `vcpkg/ports/*`. Until those
are reconciled with whatever the GitHub Release pipeline actually publishes
(currently `core 0.18.0/1.0.0/1.1.0`, `cpp 1.1.0`, `framework-cpp 0.12.0`),
neither overlay port nor local Conan export is usable, independent of any
registry PR being merged.

## Prerequisites

- A C++20 compiler. Used here: GCC 13.3.0. The framework's `CMakeLists.txt`
  sets `cxx_std_20` on every target and `cmake_minimum_required(VERSION 3.20)`.
- CMake >= 3.20, Ninja (or any CMake generator).
- Third-party libraries the framework's `find_package` calls require, and how
  they were satisfied on a stock Ubuntu 24.04 (noble) machine:

  | Library | Source | Why |
  | --- | --- | --- |
  | `nlohmann_json` | `apt install nlohmann-json3-dev` | default JSON codec; cannot be turned off |
  | `Boost` (headers + asio/beast) | `apt install libboost-all-dev` | HTTP/transport |
  | `OpenSSL` | already present (`openssl`) | Stream Connector TLS (on by default) |
  | `lz4` | `apt install liblz4-dev` | STREAM compression (on by default) |
  | `Protobuf` | `apt install libprotobuf-dev protobuf-compiler` | protobuf codec extension, always built |
  | `opentelemetry-cpp` **api** component | built from source, see below | observability; **install.ko.md says this cannot be turned off**, and it is in nobody's apt repo |
  | `redis++` | not installed | `find_package(redis++ CONFIG QUIET)` -- optional, this quickstart does not use locations |

  `opentelemetry-cpp` is the one real gap: it is not packaged for Debian/Ubuntu
  and vcpkg is not part of this recipe (the brief for this job forbids it).
  The framework only *links* `opentelemetry-cpp::api`, which is header-only,
  so building opentelemetry-cpp with `-DOTELCPP_WITH_API_ONLY=ON` finishes in
  under a second (no sdk, no exporters, no protobuf/grpc pulled in) and still
  produces a valid `opentelemetry-cpp-config.cmake`:

  ```bash
  git clone --branch v1.29.0 --depth 1 https://github.com/open-telemetry/opentelemetry-cpp.git otel-src
  cmake -S otel-src -B otel-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PWD/install/otel" -DOTELCPP_WITH_API_ONLY=ON -DBUILD_TESTING=OFF
  cmake --build otel-build -j"$(nproc)"
  cmake --install otel-build
  ```

## Build -- all three stages, in order

```bash
ROOT=$PWD/.zlink-cpp-quickstart-deps   # any scratch directory outside this repo
mkdir -p "$ROOT"/{src,install}

# Stage 1: Core
curl -sL -o core.tar.gz \
  https://github.com/zlink-systems/zlink/releases/download/core/v1.1.0/zlink-1.1.0-source.tar.gz
mkdir -p "$ROOT/src/core" && tar xzf core.tar.gz -C "$ROOT/src/core"
cmake -S "$ROOT/src/core/core" -B "$ROOT/build/core" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ROOT/install/core" \
  -DBUILD_TESTS=OFF \
  -DBUILD_STATIC=OFF   # see "packaging gap" below -- required for stage 3 to configure
cmake --build "$ROOT/build/core" -j"$(nproc)"
cmake --install "$ROOT/build/core"

# Stage 2: C++ binding
curl -sL -o cpp.tar.gz \
  https://github.com/zlink-systems/zlink/releases/download/cpp/v1.1.0/zlink-cpp-1.1.0.tar.gz
mkdir -p "$ROOT/src/cpp" && tar xzf cpp.tar.gz -C "$ROOT/src/cpp" --strip-components=1
cmake -S "$ROOT/src/cpp" -B "$ROOT/build/cpp" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ROOT/install/cpp" \
  -DZLINK_CPP_CORE_PACKAGE_PREFIX="$ROOT/install/core"
cmake --build "$ROOT/build/cpp" -j"$(nproc)"
cmake --install "$ROOT/build/cpp"

# opentelemetry-cpp (api-only) -- see Prerequisites above, install into "$ROOT/install/otel"

# Stage 3: C++ framework
curl -sL -o framework.tar.gz \
  https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v0.12.0/zlink-framework-cpp-0.12.0.tar.gz
mkdir -p "$ROOT/src/framework" && tar xzf framework.tar.gz -C "$ROOT/src/framework" --strip-components=1
cmake -S "$ROOT/src/framework" -B "$ROOT/build/framework" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ROOT/install/framework" \
  -DCMAKE_PREFIX_PATH="$ROOT/install/otel" \
  -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX="$ROOT/install/cpp" \
  -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX="$ROOT/install/core" \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_UNREAL=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_GODOT=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_AXMOL=OFF
cmake --build "$ROOT/build/framework" -j"$(nproc)"
cmake --install "$ROOT/build/framework"
```

The four `ZLINK_STREAM_CONNECTOR_BUILD_*`/`BUILD_TESTS`/`BUILD_CROSS_LANGUAGE`
flags are all **ON by default** in the archive, which pulls in GTest, a
protobuf schema pipeline, and game-engine adapter stubs (Unreal/Godot/Axmol)
that a minimal quickstart does not need; turning them off is required to keep
this stage from failing on missing engine SDKs and test-only deps
(`opentelemetry-cpp::sdk`, `GTest`).

### Packaging gap found while doing this: `-DBUILD_STATIC=OFF` is not optional here

Stage 3's install step **copies** Core's and the binding's CMake config
directories wholesale into the framework's own install prefix (so a consumer
only has to add `install/framework` and `install/otel` to
`CMAKE_PREFIX_PATH` -- see below). But it only copies Core's *shared*-library
artifacts (`libzlink.so*`), while the copied `zlinkTargets.cmake` still
declares an imported `libzlink-static` target pointing at `libzlink.a`. With
a default Core build (`BUILD_STATIC=ON`, the archive's own default),
`find_package(zlink_framework CONFIG REQUIRED)` in **any** downstream project
fails immediately with:

```
The imported target "libzlink-static" references the file
".../install/framework/lib/libzlink.a"
but this file does not exist.
```

This is not a version mismatch (all three versions line up exactly); it is a
gap in what stage 3's `install()` rules copy versus what the copied
`zlinkTargets.cmake` still declares. `-DBUILD_STATIC=OFF` on the Core build
sidesteps it (the binding already prefers the shared `libzlink` target over
`libzlink-static` when both exist, so nothing else changes). Worth reporting
for #377: this defect exists independent of whether Core ends up bundled
into the binding archive.

## Build and run this project

Once all three stages above are installed:

```bash
cd framework/languages/cpp/quickstart
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$ROOT/install/framework;$ROOT/install/otel"
cmake --build build -j"$(nproc)"

export LD_LIBRARY_PATH="$ROOT/install/framework/lib"
./build/quickstart_server &
./build/quickstart_client &
sleep 1
curl http://127.0.0.1:5083/hello/world
```

## Expected output

```
$ curl http://127.0.0.1:5083/hello/world
"hello, world"
```

HTTP 200, exactly as measured (see the job report for the verbatim command
transcript).

## What to carry into your own project

- The three-stage `CMAKE_PREFIX_PATH` recipe above (only `install/framework`
  and `install/otel` are needed at *consume* time -- stage 3's install step
  already re-exports Core's and the binding's CMake packages into its own
  prefix, once the `BUILD_STATIC=OFF` workaround is applied).
- `Shared/messages.hpp`'s `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` pattern for
  message contracts.
- `Server/main.cpp`'s `add_route_mesh(...).listen(...).set_object_role
  (zlink::framework::object_role_t::none).set_routing_id(...)
  .set_advertise_host(...)` block -- every one of those four calls is
  required for a channel-only mesh node that is not an Object Client/Server
  (see "Differences from guide §2" below).
- `Client/main.cpp`'s HTTP handler shape: constructor-injected
  `zlink::framework::route_client_t &`, `handle(const http_request_t&)`
  returning `task_t<http_response_t>`, and `request.route_values` for
  `{name}`-style path segments.

## Differences from guide §2 (needed to actually run)

The guide's snippet is intentionally minimal prose. Five things had to be
added to get a real binary past `app.run()`:

1. **`set_object_role(object_role_t::none)`.** A freshly built
   `mesh_node_builder_t`'s runtime state defaults its object role to
   `server` (`framework/src/runtime/mesh/mesh_node_runtime.hpp:143`), not
   `none`. Any route mesh that registers a channel therefore trips
   `validate_object_store_configuration`'s check ("Object Client and Object
   Server MeshNodes require a Location Store",
   `framework/src/runtime/host/app.cpp:222`) at `app.run()` unless the object
   role is explicitly cleared. The guide's snippet never calls
   `.objects()`, so it silently inherits the `server` default and would
   throw at startup exactly as this quickstart did on the first run.
2. **`set_routing_id(...)`.** Every mesh node needs a routing id; without
   one, `app.run()` throws `"MeshNode routing id is required"` before any
   socket opens.
3. **`set_advertise_host(...)`.** Listening on a wildcard bind host
   (`tcp://0.0.0.0:PORT`, exactly what the guide's snippet uses) throws
   `std::invalid_argument("MeshNode wildcard bind host requires an
   advertise host")` unless an explicit advertise host is also set.
4. **JSON glue for the message types.** `hello_t`/`greeting_t` need
   `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` (or hand-written `to_json`/
   `from_json`) before `serializer_registry_t::get<T>()`'s default JSON path
   will serialize them; a bare aggregate struct as shown in the guide is not
   enough (`framework/include/.../codecs/serializer.hpp`'s
   `is_json_serializer_compatible_v` requires ADL `to_json`/`from_json`).
5. **The HTTP handler's parameter shape.** The guide shows
   `handle(const std::string &name)` with the path segment bound directly as
   a function parameter. The actual `http_options_builder_t::add_route`
   dispatch only supports two handler shapes: a typed `request_type`/
   `reply_type` pair (JSON body), or a raw `handle(const http_request_t&)`
   that reads `request.route_values.at("name")` itself -- there is no
   per-parameter route binding. This quickstart uses the raw shape, matching
   every real handler found in `framework/languages/cpp/samples/**`.

None of these are version mismatches -- they are gaps between the guide's
illustrative snippet and the handler/builder API that ships in
`framework-cpp/v0.12.0`.

## Ports used

This quickstart intentionally avoids `tcp://0.0.0.0:7101`/`7102` and
`http://0.0.0.0:5080`/`5081` (the ports the guide and the .NET quickstart
use), since other quickstarts and samples in this repository run those same
ports concurrently on a shared machine. This one uses `7301`/`7302` for the
mesh and `5083` for HTTP -- pick free ports for your own project.
