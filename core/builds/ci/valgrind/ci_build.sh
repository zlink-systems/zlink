#!/usr/bin/env bash
#
# NOTE: zlink uses CMake build system (not autotools)
# This script builds with valgrind memory checking support

set -x
set -e

mkdir -p tmp build_valgrind
BUILD_PREFIX=$PWD/tmp

# Build zlink with debug symbols for valgrind
CMAKE_OPTS=()
CMAKE_OPTS+=("-DCMAKE_BUILD_TYPE=Debug")
CMAKE_OPTS+=("-DCMAKE_INSTALL_PREFIX=${BUILD_PREFIX}")
CMAKE_OPTS+=("-DCMAKE_PREFIX_PATH=${BUILD_PREFIX}")
CMAKE_OPTS+=("-DBUILD_TESTS=ON")

if [ -n "$TLS" ] && [ "$TLS" == "enabled" ]; then
    CMAKE_OPTS+=("-DWITH_TLS=ON")
fi

# Build, check, and install from local source
(
    cd build_valgrind &&
    cmake ../.. "${CMAKE_OPTS[@]}" &&
    cmake --build . -j5 &&
    # Run tests under valgrind
    ctest --verbose --timeout 300 -T memcheck --output-on-failure
) || exit 1
