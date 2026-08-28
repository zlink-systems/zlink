#!/usr/bin/env bash

# Keep sample process evidence tied to one explicit Framework/Core package
# provenance. An existing build directory must not silently select another
# zlink_cpp version.
zlink_cpp_sample_prepare_build() {
  local cpp_root="$1"
  # All C++ tests and samples share the framework build tree. Reapply the
  # canonical package versions before each sample build so a stale cache
  # cannot silently select a second build provenance.
  BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-$cpp_root/build}"
  local cpp_version="0.14.0"
  local core_version="0.14.0"
  local dependency_prefix=""
  local toolchain_file=""
  local build_type="Release"

  if [[ -z "$dependency_prefix" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    dependency_prefix="$(sed -n 's/^CMAKE_PREFIX_PATH:[^=]*=//p' \
      "$BUILD_DIR/CMakeCache.txt" | head -n 1)"
    toolchain_file="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' \
      "$BUILD_DIR/CMakeCache.txt" | head -n 1)"
    local cached_build_type
    cached_build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' \
      "$BUILD_DIR/CMakeCache.txt" | head -n 1)"
    if [[ -n "$cached_build_type" ]]; then
      build_type="$cached_build_type"
    fi
  fi

  if [[ -z "$toolchain_file" && -n "${VCPKG_ROOT:-}" \
    && -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    toolchain_file="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  fi

  local -a cmake_args=(
    -S "$cpp_root"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$build_type"
    -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION="$cpp_version"
    -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION="$core_version"
    -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON
    -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=ON
    -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=ON
    -DZLINK_FRAMEWORK_CPP_BUILD_E2E=ON
  )
  if [[ -n "$dependency_prefix" ]]; then
    cmake_args+=("-DCMAKE_PREFIX_PATH=$dependency_prefix")
  fi
  if [[ -n "$toolchain_file" ]]; then
    cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain_file")
  fi

  # The package prefixes and find-package directories are derived cache
  # entries. Drop them when applying the canonical package versions so an
  # existing build tree cannot keep resolving an older zlink_cpp/Core pair.
  cmake \
    -U ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX \
    -U ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX \
    -U zlink_cpp_DIR \
    -U zlink_DIR \
    "${cmake_args[@]}" >/dev/null
  BIN_DIR="$BUILD_DIR"
  if [[ ! -x "$BIN_DIR/sample_cpp_framework_tictactoe_play" \
    && -d "$BIN_DIR/linux-ninja-debug" ]]; then
    BIN_DIR="$BIN_DIR/linux-ninja-debug"
  fi
}
