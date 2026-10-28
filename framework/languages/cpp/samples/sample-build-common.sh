#!/usr/bin/env bash

# One owner for "where do this runner's binaries come from". The explicit
# ZLINK_CPP_BUILD_DIR overrides either default. Two trees exist:
#
#   repository  samples/ sits inside framework/languages/cpp. Its own build
#               tree (with tests) is the fallback when bootstrap.cmake has not
#               configured samples/build. The tree owner configures it once;
#               runners only verify its cached package versions.
#   package     bootstrap.cmake configured samples/build against the installed
#               framework; the runner only builds its own targets there.
#
# Both set BUILD_DIR (preferring ZLINK_CPP_BUILD_DIR) and BIN_DIR.
CPP_SAMPLES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

zlink_cpp_sample_log_step() {
  printf '[%s] %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*" >&2
}

zlink_cpp_sample_uses_repository_build() {
  local cpp_root="$CPP_SAMPLES_DIR/.."
  [[ ( -n "${ZLINK_CPP_BUILD_DIR:-}" || ! -f "$CPP_SAMPLES_DIR/build/CMakeCache.txt" ) \
    && -f "$cpp_root/CMakeLists.txt" \
    && -f "$cpp_root/framework/include/zlink/framework.hpp" ]]
}

zlink_cpp_sample_prepare_build() {
  zlink_cpp_sample_log_step "provenance check"
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
  # All C++ tests and samples share the framework build tree. Its owner
  # configures the package versions; a runner rejects a mismatch without
  # changing the configured tree.
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

  local cache_file="$BUILD_DIR/CMakeCache.txt"
  if [[ -f "$cache_file" ]]; then
    local cached_cpp_version cached_core_version mismatch=0
    cached_cpp_version="$(sed -n 's/^ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION:[^=]*=//p' \
      "$cache_file" | head -n 1)"
    cached_core_version="$(sed -n 's/^ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION:[^=]*=//p' \
      "$cache_file" | head -n 1)"
    if [[ "$cached_cpp_version" != "$cpp_version" ]]; then
      printf 'CMake build tree provenance mismatch at %s:\n' "$BUILD_DIR" >&2
      printf '  ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION: cache=%s canonical=%s\n' \
        "${cached_cpp_version:-<missing>}" "$cpp_version" >&2
      mismatch=1
    fi
    if [[ "$cached_core_version" != "$core_version" ]]; then
      if [[ "$mismatch" == "0" ]]; then
        printf 'CMake build tree provenance mismatch at %s:\n' "$BUILD_DIR" >&2
      fi
      printf '  ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION: cache=%s canonical=%s\n' \
        "${cached_core_version:-<missing>}" "$core_version" >&2
      mismatch=1
    fi
    [[ "$mismatch" == "0" ]] || return 1
  else
    local toolchain_file=""
    local build_type="Release"

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
    if [[ -n "$toolchain_file" ]]; then
      cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain_file")
    fi

    zlink_cpp_sample_log_step "configure repository build"
    cmake "${cmake_args[@]}"
  fi
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
  zlink_cpp_sample_log_step "framework tests"
  "${CTEST_BIN:-ctest}" --test-dir "$BUILD_DIR" -R "$regex" --output-on-failure
}
