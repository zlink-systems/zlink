#!/usr/bin/env bash

# One owner for "where do this runner's binaries come from". The explicit
# ZLINK_CPP_BUILD_DIR overrides either default. Two trees exist:
#
#   repository  samples/ sits inside framework/languages/cpp. Its own build
#               tree (with tests) is the fallback when bootstrap.cmake has not
#               configured samples/build. The canonical package versions are
#               reapplied before each repository build.
#   package     bootstrap.cmake configured samples/build against the installed
#               framework; the runner only builds its own targets there.
#
# Both set BUILD_DIR (preferring ZLINK_CPP_BUILD_DIR) and BIN_DIR.
CPP_SAMPLES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

zlink_cpp_sample_uses_repository_build() {
  local cpp_root="$CPP_SAMPLES_DIR/.."
  [[ ( -n "${ZLINK_CPP_BUILD_DIR:-}" || ! -f "$CPP_SAMPLES_DIR/build/CMakeCache.txt" ) \
    && -f "$cpp_root/CMakeLists.txt" \
    && -f "$cpp_root/framework/include/zlink/framework.hpp" ]]
}

zlink_cpp_sample_prepare_build() {
  if zlink_cpp_sample_uses_repository_build; then
    zlink_cpp_sample_prepare_repository_build "$(cd "$CPP_SAMPLES_DIR/.." && pwd)"
  else
    zlink_cpp_sample_prepare_package_build
  fi
}

zlink_cpp_sample_prepare_package_build() {
  BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-$CPP_SAMPLES_DIR/build}"
  if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "No configured build tree at $BUILD_DIR." >&2
    echo "Run 'cmake -P bootstrap.cmake' in $CPP_SAMPLES_DIR first (see README.md)." >&2
    return 1
  fi
  BIN_DIR="$BUILD_DIR"
}

# Keep sample process evidence tied to one explicit Framework/Core package
# provenance. An existing build directory must not silently select another
# zlink_cpp version.
zlink_cpp_sample_prepare_repository_build() {
  local cpp_root="$1"
  # All C++ tests and samples share the framework build tree. Reapply the
  # canonical package versions before each sample build so a stale cache
  # cannot silently select a second build provenance.
  BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-$cpp_root/build}"
  local repository_root
  repository_root="$(cd "$cpp_root/../../.." && pwd)"
  local cpp_version
  cpp_version="$(sed -n 's/^ZLINK_BINDING_VERSION=//p' "$repository_root/bindings/cpp/VERSION")"
  local core_version
  core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repository_root/VERSION")"
  if [[ ! "$cpp_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ \
    || ! "$core_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Failed to read canonical C++ binding/Core versions." >&2
    return 1
  fi
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

# Framework gates a runner runs beside its sample: the named test targets are
# built and executed through CTest in the repository tree, where they exist.
# The package tree has no framework tests, so the gate is reported as skipped
# rather than silently passed.
zlink_cpp_sample_framework_test_targets() {
  if zlink_cpp_sample_uses_repository_build; then
    printf '%s\n' "$@"
  fi
}

zlink_cpp_sample_run_framework_tests() {
  local regex="$1"
  if ! zlink_cpp_sample_uses_repository_build; then
    echo "framework tests: skipped (package tree; no framework test targets)"
    return 0
  fi
  "${CTEST_BIN:-ctest}" --test-dir "$BUILD_DIR" -R "$regex" --output-on-failure
}
