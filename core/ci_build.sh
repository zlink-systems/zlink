#!/usr/bin/env bash

set -x
set -e
export TMPDIR=/tmp
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CORE_ROOT="$SCRIPT_DIR"

cd "$CORE_ROOT"

if [ $BUILD_TYPE = "default" ]; then
    mkdir -p build tmp
    BUILD_PREFIX=$CORE_ROOT/tmp
    export TMPDIR=$CORE_ROOT/tmp

    # zlink uses CMake build system (not autotools)
    # Build and check this project
    (
        cd build &&
        cmake "$CORE_ROOT" \
            -DCMAKE_INSTALL_PREFIX="${BUILD_PREFIX}" \
            -DBUILD_TESTS=ON \
            -DBUILD_STATIC=OFF &&
        cmake --build . --verbose -j5 &&
        ctest --output-on-failure
    ) || exit 1
else
    cd "${CORE_ROOT}/builds/ci/${BUILD_TYPE}" && ./ci_build.sh
fi
