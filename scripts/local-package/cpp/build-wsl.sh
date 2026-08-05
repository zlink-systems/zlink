#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"
core_prefix="${ZLINK_CORE_INSTALL_PREFIX:-}"
core_provenance_sha256="${ZLINK_CORE_PROVENANCE_SHA256:-}"
core_candidate_manifest_sha256="${ZLINK_CORE_CANDIDATE_MANIFEST_SHA256:-}"
evidence="${ZLINK_CPP_PACKAGE_EVIDENCE:-}"
cmake_args=()

usage() {
  cat <<'EOF'
Usage: build-wsl.sh --core-prefix ABSOLUTE_DIR \
  --core-provenance-sha256 SHA256 \
  --core-candidate-manifest-sha256 SHA256 \
  --evidence ABSOLUTE_JSON [-- CMAKE_ARGS...]

Builds and installs zlink_cpp from the approved Core 11 package, then verifies
an isolated external CMake consumer through compile, link, load, and run.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --core-provenance-sha256) core_provenance_sha256="${2:-}"; shift 2 ;;
    --core-candidate-manifest-sha256) core_candidate_manifest_sha256="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    --) shift; cmake_args=("$@"); break ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

package_version="${ZLINK_CPP_PACKAGE_VERSION:-$(awk 'index($0, "project(zlink_cpp VERSION ") == 1 { print $3; exit }' "$repo_root/bindings/cpp/CMakeLists.txt")}"
if [ -z "$package_version" ]; then
  echo "Unable to resolve zlink_cpp package version" >&2
  exit 2
fi
build_dir="${ZLINK_CPP_LOCAL_BUILD_DIR:-$artifact_root/build/bindings-cpp-$package_version}"
install_prefix="${ZLINK_CPP_INSTALL_PREFIX:-$artifact_root/install/zlink-cpp/$package_version}"
if [[ "$core_prefix" != /* ]]; then
  echo "--core-prefix must name an absolute installed Core 11 package prefix" >&2
  exit 2
fi
[[ "$evidence" = /* ]] || { echo "--evidence must be an absolute path" >&2; exit 2; }
[[ "$core_provenance_sha256" =~ ^[0-9a-f]{64}$ ]] || {
  echo "--core-provenance-sha256 must be a lowercase SHA-256" >&2
  exit 2
}
[[ "$core_candidate_manifest_sha256" =~ ^[0-9a-f]{64}$ ]] || {
  echo "--core-candidate-manifest-sha256 must be a lowercase SHA-256" >&2
  exit 2
}
core_prefix="$(readlink -f "$core_prefix")"
artifact_root="$(realpath -m "$artifact_root")"
install_prefix="$(realpath -m "$install_prefix")"
evidence="$(realpath -m "$evidence")"
[[ "$package_version" =~ ^11\.[0-9]+\.[0-9]+$ ]] || {
  echo "zlink_cpp package version must be numeric 11.x.y, found: $package_version" >&2
  exit 1
}
[[ "$artifact_root" != / && "$artifact_root" != "$(readlink -f "$repo_root")" ]] || {
  echo "Unsafe local package artifact root: $artifact_root" >&2
  exit 2
}
case "$install_prefix" in
  "$artifact_root"/install/zlink-cpp/*) ;;
  *) echo "C++ install prefix must remain below the local package artifact root" >&2; exit 2 ;;
esac
case "$evidence" in
  "$install_prefix"|"$install_prefix"/*)
    echo "--evidence must be outside the install prefix" >&2
    exit 2 ;;
esac

cmake -S "$repo_root/bindings/cpp" -B "$build_dir" \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DZLINK_CPP_CORE_PACKAGE_PREFIX="$core_prefix" \
  -DZLINK_CPP_BUILD_TESTS=OFF \
  -DZLINK_CPP_BUILD_SAMPLES=OFF \
  "${cmake_args[@]}"

if grep -q '^CMAKE_CONFIGURATION_TYPES:' "$build_dir/CMakeCache.txt"; then
  cmake --build "$build_dir" --config "$configuration"
else
  cmake --build "$build_dir"
fi

# A package prefix is replaced as one unit so stale headers from an older
# service projection cannot survive a raw-only package install.
rm -rf "$install_prefix"
if grep -q '^CMAKE_CONFIGURATION_TYPES:' "$build_dir/CMakeCache.txt"; then
  cmake --install "$build_dir" --config "$configuration"
else
  cmake --install "$build_dir"
fi

"$script_dir/verify-consumer.sh" \
  --prefix "$install_prefix" \
  --core-prefix "$core_prefix" \
  --core-provenance-sha256 "$core_provenance_sha256" \
  --core-candidate-manifest-sha256 "$core_candidate_manifest_sha256" \
  --evidence "$evidence"

echo "-- C++ local package version: $package_version"
echo "-- C++ local install prefix: $install_prefix"
echo "-- C++ package evidence: $evidence"
