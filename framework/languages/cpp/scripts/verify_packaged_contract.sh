#!/usr/bin/env bash
# Packaged-contract gate (plan §8.2.1, C++ row).
#
# Installs only the Framework, StreamConnector and FrameworkDependency install
# components into an empty prefix, compares the installed target/header set
# against the ownership manifest, fails when any HttpClient artifact leaks in,
# and then builds and runs an out-of-tree consumer that uses that prefix plus
# the explicitly pinned third-party dependency prefix recorded by the build.
set -euo pipefail

BUILD_DIR="${1:?usage: verify_packaged_contract.sh <cmake-build-dir>}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zlink-cpp-packaged-contract.XXXXXXXX")"
PREFIX="$RUN_DIR/install"
CONSUMER_SRC="$RUN_DIR/consumer"
CONSUMER_BUILD="$RUN_DIR/consumer-build"
mkdir -p "$PREFIX" "$CONSUMER_SRC" "$CONSUMER_BUILD"

echo "packaged-contract run dir: $RUN_DIR"

# A package consumer must not discover dependencies through the repository
# source tree or an ambient global installation.  Reuse only the dependency
# prefix that configured this build; it is part of the package provenance.
dependency_prefix_path="$(sed -n 's/^CMAKE_PREFIX_PATH:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n 1)"

for component in Framework StreamConnector FrameworkDependency; do
    cmake --install "$BUILD_DIR" --component "$component" --prefix "$PREFIX" \
      > "$RUN_DIR/install-$component.log"
done

fail() {
    echo "verify_packaged_contract: $1" >&2
    exit 1
}

# --- manifest: required targets/headers/libraries -------------------------
required_paths=(
    include/zlink/framework.hpp
    include/zlink/framework/contracts/actors/actor.hpp
    include/zlink/framework/contracts/errors/error.hpp
    include/zlink/framework/contracts/channels/channel.hpp
    include/zlink/framework/contracts/channels/call.hpp
    include/zlink/framework/contracts/spots/spot.hpp
    include/zlink/framework/contracts/spots/spot_identity.hpp
    include/zlink/framework/contracts/locations/resolvers.hpp
    include/zlink/framework/contracts/streams/stream.hpp
    include/zlink/framework/contracts/workers/worker.hpp
    include/zlink/framework/contracts/configuration/endpoint_connections.hpp
    include/zlink/stream_connector.hpp
    include/zlink/Contracts/Core/routing_id.hpp
    lib/libzlink_framework.a
    lib/libzlink_stream_connector.a
    lib/libzlink_cpp.a
    lib/cmake/zlink_framework_cpp/zlink_framework_cppConfig.cmake
    lib/cmake/zlink_framework_cpp/zlink_framework_cppTargets.cmake
    lib/cmake/zlink_stream_connector_cpp/zlink_stream_connector_cppConfig.cmake
    lib/cmake/zlink_stream_connector_cpp/zlink_stream_connector_cppTargets.cmake
    lib/cmake/zlink_cpp
)
for path in "${required_paths[@]}"; do
    [[ -e "$PREFIX/$path" ]] || fail "required install entry is missing: $path"
done

# --- manifest: HttpClient must not leak into this prefix ------------------
[[ -e "$PREFIX/include/zlink/http_client.hpp" ]] \
  && fail "HttpClient header leaked into the framework prefix"
[[ -d "$PREFIX/include/zlink/http_client" ]] \
  && fail "HttpClient header tree leaked into the framework prefix"
[[ -e "$PREFIX/lib/libzlink_http_client.a" ]] \
  && fail "HttpClient library leaked into the framework prefix"
[[ -d "$PREFIX/lib/cmake/zlink_http_client_cpp" ]] \
  && fail "HttpClient package config leaked into the framework prefix"
grep -q "zlink::http_client" \
  "$PREFIX/lib/cmake/zlink_framework_cpp/zlink_framework_cppTargets.cmake" \
  && fail "framework export still references zlink::http_client"

# --- manifest: removed public surfaces must not be installed --------------
for forbidden in \
    include/zlink/framework/contracts/dispatch/cancellation.hpp; do
    [[ -e "$PREFIX/$forbidden" ]] && fail "removed contract header is still installed: $forbidden"
done
for forbidden_token in cancellation_token_t dispatch_mode_t spot_handle_t spot_handle_resolver_t; do
    grep -rq "$forbidden_token" "$PREFIX/include/zlink/framework" \
      && fail "installed framework headers still expose $forbidden_token"
done

# --- artifact evidence -----------------------------------------------------
echo "installed libraries:"
for lib in libzlink_framework.a libzlink_stream_connector.a libzlink_cpp.a; do
    sha256sum "$PREFIX/lib/$lib"
done
echo "installed header count: $(find "$PREFIX/include" -name '*.hpp' -o -name '*.h' | wc -l)"

# --- clean out-of-tree consumer --------------------------------------------
cat > "$CONSUMER_SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(zlink_cpp_packaged_contract_consumer LANGUAGES CXX)

find_package(zlink_framework_cpp CONFIG REQUIRED)
find_package(zlink_stream_connector_cpp CONFIG REQUIRED)

add_executable(packaged_consumer main.cpp)
target_link_libraries(packaged_consumer PRIVATE
  zlink::framework
  zlink::stream_connector)
EOF

cat > "$CONSUMER_SRC/main.cpp" <<'EOF'
#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>

int main ()
{
    zlink::framework::zlink_builder_t builder;
    auto channel = builder.channel ("packaged-consumer");
    channel.enable_client ();
    (void) channel.snapshot ();
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    struct probe_t
    {
    };
    if (provider.get<probe_t> ().has_value ()) {
        return 2;
    }
    return 0;
}
EOF

cmake -S "$CONSUMER_SRC" -B "$CONSUMER_BUILD" \
  -DCMAKE_PREFIX_PATH="$PREFIX;${dependency_prefix_path}" \
  -DCMAKE_BUILD_TYPE=Release \
  > "$RUN_DIR/consumer-configure.log"
cmake --build "$CONSUMER_BUILD" -j > "$RUN_DIR/consumer-build.log"

# The consumer must resolve every zlink include from the install prefix only.
repo_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
if grep -R "$repo_root/framework/languages/cpp/framework/include" \
     "$CONSUMER_BUILD/CMakeFiles" > /dev/null 2>&1; then
    fail "consumer resolved framework headers from the repository source tree"
fi

"$CONSUMER_BUILD/packaged_consumer"
echo "packaged consumer run: OK"
echo "verify_packaged_contract: PASS"
