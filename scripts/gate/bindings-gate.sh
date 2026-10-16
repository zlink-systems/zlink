#!/usr/bin/env bash
# Binding contract suites (7 languages) against the current core/build-dev, serialized behind the samples lock.
# shellcheck source=./common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh" || exit $?
cd "$Z" || exit
require_quiet || exit 2
: > "$LOGS/results.txt"
LIB="$CORE_LIB"; INC="$Z/core/include"
CORE_ABI="$(sed -n 's/^LIBZLINK_ABI_SOVERSION=//p' "$Z/VERSION")"
NODE_CORE_PREFIX="${ZLINK_CORE_CACHE_DIR:-$HOME/.cache/zlink/core}/$CORE_VER/linux-x64"
run c        bindings/c      env ZLINK_C_CORE_BUILD_DIR="$Z/core/build-dev" ZLINK_CORE_INCLUDE_DIR="$INC" bash tests/run_tests.sh
run cpp      bindings/cpp    env ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR="$Z/core/build-dev" ZLINK_BUILD_JOBS=4 bash tests/run_tests.sh
run dotnet   bindings/dotnet env ZLINK_LIBRARY_PATH="$LIB" TMPDIR=/dev/shm/zlink-tmp-dotnet UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 bash tests/run_tests.sh
run go-core-libs . env CORE_LIB_DIR="$LIB" bash scripts/local-package/native/sync-local-core-libs.sh go
run go       bindings/go     bash tests/run_tests.sh
run java     bindings/java   env ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR="$INC" ZLINK_CORE_LIB_DIR="$LIB" bash tests/run_tests.sh
run node-install bindings/node env ZLINK_SKIP_NATIVE_INSTALL=1 npm ci
# The child shell expands these variables after the gate's run wrapper changes directory.
# shellcheck disable=SC2016
run node-core-runtime bindings/node env CORE_LIB="$LIB" CORE_ABI="$CORE_ABI" bash -c '
  mkdir -p build/Release
  cp "$CORE_LIB/libzlink.so" build/Release/libzlink.so
  cp "$CORE_LIB/libzlink.so.$CORE_ABI" "build/Release/libzlink.so.$CORE_ABI"
'
run node bindings/node env -u ZLINK_CORE_INSTALL_PREFIX ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$NODE_CORE_PREFIX" ZLINK_LIBRARY_PATH="$LIB/libzlink.so.$CORE_VER" LD_LIBRARY_PATH="$LIB" npm test
if [[ -n "${ZLINK_PYTHON:-}" ]]; then
  PYTHON_EXECUTABLE="$ZLINK_PYTHON"
else
  PYTHON_VENV="$LOGS/python-venv"
  run python-venv . python3 -m venv "$PYTHON_VENV"
  PYTHON_EXECUTABLE="$PYTHON_VENV/bin/python"
fi
run python-test-dependencies . "$PYTHON_EXECUTABLE" -m pip install --no-cache-dir 'setuptools>=68' pytest
run python-core-libs . env CORE_LIB_DIR="$LIB" bash scripts/local-package/native/sync-local-core-libs.sh python
run python-native-build bindings/python env ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR="$INC" ZLINK_CORE_LIB_DIR="$LIB" ZLINK_LIBRARY_PATH="$LIB/libzlink.so" "$PYTHON_EXECUTABLE" setup.py build_ext --inplace --force
run python   bindings/python env ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH="$LIB/libzlink.so" PYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" bash tests/run_tests.sh
run rust     bindings/rust   env LD_LIBRARY_PATH="$LIB" bash tests/run_tests.sh
echo "BINDINGS_DONE $TAG"
finish_gate bindings.done
