#!/usr/bin/env bash
# Build the zlink Core in one of two persistent build trees.
#
#   scripts/build-core.sh dev           # correctness / iteration: RelWithDebInfo, LTO OFF, tests ON, fast link
#   scripts/build-core.sh release       # release lib / perf: Release, LTO ON, tests OFF (one LTO link)
#   scripts/build-core.sh release-gate  # pre-release only: Release, LTO ON, tests ON (hotpath_gate, LTO ctest)
#   scripts/build-core.sh <mode> --lib-only   # build only the libzlink runtime in that tree (perf runners use this)
#
# Each tree is configured once (first run) and reused afterwards, so day-to-day
# work is just a rebuild — no per-run setting changes. Correctness ctest should
# use the dev tree (LTO link is the slow part and is unnecessary for tests);
# reserve the release tree for the shipped library and perf measurement. Test
# executables link the static archive, so with LTO on every test relinks the
# whole library: the release tree therefore builds without tests, and the
# release-gate mode (same tree, tests ON) is for the callgrind hotpath_gate and
# a final LTO ctest right before a release. Until the test link structure is
# reworked (CONTRIBUTING.ko.md §9), that is the fast path.
set -euo pipefail

mode="dev"
lib_only=0
for arg in "$@"; do
  case "$arg" in
    dev|release|release-gate) mode="$arg" ;;
    --lib-only) lib_only=1 ;;
    *) echo "usage: $0 [dev|release|release-gate] [--lib-only]" >&2; exit 2 ;;
  esac
done
jobs="${JOBS:-$(nproc)}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

case "$mode" in
  dev)
    build_dir="core/build-dev"
    build_type="RelWithDebInfo"
    lto="OFF"
    tests="ON"
    ;;
  release)
    build_dir="core/build"
    build_type="Release"
    lto="ON"
    tests="OFF"
    ;;
  release-gate)
    build_dir="core/build"
    build_type="Release"
    lto="ON"
    tests="ON"
    ;;
  *)
    echo "usage: $0 [dev|release|release-gate]" >&2
    exit 2
    ;;
esac

# Keep LTO's temporary link objects off the small /tmp tmpfs (LTO can exhaust it).
if [ "$lto" = "ON" ]; then
  export TMPDIR="${TMPDIR:-$repo_root/../zlink-work/tmp-build}"
  mkdir -p "$TMPDIR"
fi

echo "[build-core] mode=$mode dir=$build_dir type=$build_type ENABLE_LTO=$lto tests=$tests lib_only=$lib_only jobs=$jobs"

# --lib-only: rebuild just the shared runtime in an already configured tree
# without touching its configuration (perf runners call this when core/build is
# stale; the tree may have tests enabled, and rebuilding those would relink
# every LTO test executable).
if [ "$lib_only" = 1 ] && [ -f "$build_dir/CMakeCache.txt" ]; then
  cmake --build "$build_dir" -j "$jobs" --target libzlink
  echo "[build-core] done (lib only): $build_dir/lib"
  exit 0
fi

# Configure once; re-configuring an existing tree with the same flags is cheap and idempotent.
cmake -S core -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DENABLE_LTO="$lto" \
  -DZLINK_BUILD_TESTS="$tests" \
  -DBUILD_TESTS="$tests"

if [ "$lib_only" = 1 ]; then
  cmake --build "$build_dir" -j "$jobs" --target libzlink
  echo "[build-core] done (lib only): $build_dir/lib"
  exit 0
fi

cmake --build "$build_dir" -j "$jobs"

echo "[build-core] done: $build_dir/lib"
if [ "$tests" = "ON" ]; then
  echo "[build-core] test with: ctest --test-dir $build_dir --output-on-failure"
else
  echo "[build-core] tests are not built in this mode; use 'dev' for ctest or 'release-gate' for hotpath_gate"
fi
