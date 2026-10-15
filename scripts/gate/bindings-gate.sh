#!/usr/bin/env bash
# Binding contract suites (7 languages) against the current core/build-dev, serialized behind the samples lock.
# shellcheck source=./common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh" || exit $?
cd "$Z" || exit
require_quiet || exit 2
: > "$LOGS/results.txt"
LIB="$CORE_LIB"; INC="$Z/core/include"
run c        bindings/c      env ZLINK_C_CORE_BUILD_DIR="$Z/core/build-dev" ZLINK_CORE_INCLUDE_DIR="$INC" bash tests/run_tests.sh
run cpp      bindings/cpp    env ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR="$Z/core/build-dev" ZLINK_BUILD_JOBS=4 bash tests/run_tests.sh
run dotnet   bindings/dotnet env ZLINK_LIBRARY_PATH="$LIB" TMPDIR=/dev/shm/zlink-tmp-dotnet UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 bash tests/run_tests.sh
run go-core-libs . env CORE_LIB_DIR="$LIB" bash scripts/local-package/native/sync-local-core-libs.sh go
run go       bindings/go     bash tests/run_tests.sh
run java     bindings/java   env ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR="$INC" ZLINK_CORE_LIB_DIR="$LIB" bash tests/run_tests.sh
run node-build bindings/node npm run build
run node     bindings/node   env ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH="$LIB/libzlink.so.$CORE_VER" LD_LIBRARY_PATH="$LIB" bash tests/run_tests.sh
if [[ -n "${ZLINK_PYTHON:-}" ]]; then
  PYTHON_EXECUTABLE="$ZLINK_PYTHON"
else
  PYTHON_VENV="$LOGS/python-venv"
  run python-venv . python3 -m venv "$PYTHON_VENV"
  PYTHON_EXECUTABLE="$PYTHON_VENV/bin/python"
fi
run python-test-dependencies . "$PYTHON_EXECUTABLE" -m pip install --no-cache-dir pytest
run python   bindings/python env ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH="$LIB/libzlink.so" PYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" bash tests/run_tests.sh
run rust     bindings/rust   env LD_LIBRARY_PATH="$LIB" bash tests/run_tests.sh
echo "BINDINGS_DONE $TAG"
finish_gate bindings.done
