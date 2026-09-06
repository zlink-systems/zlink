#!/usr/bin/env bash
# Binding contract suites (7 languages) against the current core/build-dev, serialized behind the samples lock.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"; cd "$Z"; require_quiet || exit 2
: > "$LOGS/results.txt"
LIB="$Z/core/build-dev/lib"; INC="$Z/core/include"
run c        bindings/c      env ZLINK_C_CORE_BUILD_DIR="$Z/core/build-dev" ZLINK_CORE_INCLUDE_DIR="$INC" bash tests/run_tests.sh
run cpp      bindings/cpp    env ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR="$Z/core/build-dev" ZLINK_BUILD_JOBS=4 bash tests/run_tests.sh
run dotnet   bindings/dotnet env ZLINK_LIBRARY_PATH="$LIB" TMPDIR=/dev/shm/zlink-tmp-dotnet bash tests/run_tests.sh
run go       bindings/go     bash tests/run_tests.sh
run java     bindings/java   env ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR="$INC" ZLINK_CORE_LIB_DIR="$LIB" bash tests/run_tests.sh
run node     bindings/node   env ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH="$LIB/libzlink.so.0.17.0" LD_LIBRARY_PATH="$LIB" bash tests/run_tests.sh
run python   bindings/python env ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH="$LIB/libzlink.so" PYTHON_EXECUTABLE="${ZLINK_PYTHON:-/tmp/zlink-python-r3-venv/bin/python}" bash tests/run_tests.sh
run rust     bindings/rust   env LD_LIBRARY_PATH="$LIB" bash tests/run_tests.sh
echo "BINDINGS_DONE $TAG"; awk '$2!=0{f=1; print "FAILED:", $1} END{exit f}' "$LOGS/results.txt" && echo "ALL GREEN"
touch "$LOGS/bindings.done"
