#!/usr/bin/env bash
# Build the zlink Core in one of two persistent build trees.
#
#   scripts/build-core.sh dev       # correctness / iteration: RelWithDebInfo, LTO OFF, fast link
#   scripts/build-core.sh release   # release lib / perf: Release, LTO ON (slow LTO link)
#
# Each tree is configured once (first run) and reused afterwards, so day-to-day
# work is just a rebuild — no per-run setting changes. Correctness ctest should
# use the dev tree (LTO link is the slow part and is unnecessary for tests);
# reserve the release tree for the shipped library and perf measurement.
set -euo pipefail

mode="${1:-dev}"
jobs="${JOBS:-$(nproc)}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

case "$mode" in
  dev)
    build_dir="core/build-dev"
    build_type="RelWithDebInfo"
    lto="OFF"
    ;;
  release)
    build_dir="core/build"
    build_type="Release"
    lto="ON"
    ;;
  *)
    echo "usage: $0 [dev|release]" >&2
    exit 2
    ;;
esac

# Keep LTO's temporary link objects off the small /tmp tmpfs (LTO can exhaust it).
if [ "$lto" = "ON" ]; then
  export TMPDIR="${TMPDIR:-$repo_root/../zlink-work/tmp-build}"
  mkdir -p "$TMPDIR"
fi

echo "[build-core] mode=$mode dir=$build_dir type=$build_type ENABLE_LTO=$lto jobs=$jobs"

# Configure once; re-configuring an existing tree with the same flags is cheap and idempotent.
cmake -S core -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DENABLE_LTO="$lto" \
  -DZLINK_BUILD_TESTS=ON \
  -DBUILD_TESTS=ON

cmake --build "$build_dir" -j "$jobs"

echo "[build-core] done: $build_dir/lib"
echo "[build-core] test with: ctest --test-dir $build_dir --output-on-failure"
