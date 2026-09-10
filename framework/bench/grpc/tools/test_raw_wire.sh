#!/usr/bin/env bash
# Run after build_all.sh. Uses its configured C/C++ builds and installed dependencies.
# JAVA_HOME, ZLINK_LOCAL_PACKAGE_ROOT and native-library settings match build_all.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "${HERE}/../../.." && pwd)"

cmake --build "${HERE}/c/build" --target \
  bench_c_raw_wire_client_test bench_c_raw_wire_server_test --parallel 2
ctest --test-dir "${HERE}/c/build" -R '^bench_c_raw_wire_' --output-on-failure
cmake --build "${HERE}/cpp/build" --target bench_cpp_raw_wire_test --parallel 2
ctest --test-dir "${HERE}/cpp/build" -R '^bench_cpp_raw_wire_identity$' --output-on-failure
dotnet run --project "${HERE}/dotnet/WireTests/WithGrpcBench.WireTests.csproj" -c Release
NODE_PATH="${REPO}/framework/languages/node/node_modules${NODE_PATH:+:${NODE_PATH}}" \
  node --test "${HERE}/node/shared/raw-wire.test.js"
"${HERE}/java/gradlew" --no-daemon --max-workers=1 :shared:rawWireTest

echo RAW_WIRE_ALL_OK
