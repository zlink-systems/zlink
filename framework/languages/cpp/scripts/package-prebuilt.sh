#!/usr/bin/env bash
# Build a Linux x64 shared Framework prefix and package its Core closure.
set -euo pipefail

update_lock=0
if [[ $# -eq 1 && "$1" == "--update-lock" ]]; then
  update_lock=1
elif [[ $# -ne 1 ]]; then
  echo "usage: $0 <output-dir> | --update-lock" >&2
  exit 2
fi

if ((update_lock)); then
  output_dir="${HOME}/.cache/zlink"
  mkdir -p "$output_dir"
else
  output_dir="$(mkdir -p "$1" && cd "$1" && pwd)"
fi
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "$script_dir/.." && pwd)"
version="$(sed -n 's/^ZLINK_FRAMEWORK_VERSION=//p' "$source_dir/VERSION")"
[[ -n "$version" ]] || { echo "missing framework VERSION" >&2; exit 1; }

command -v conan >/dev/null || { echo "Conan 2 is required" >&2; exit 1; }
command -v g++-13 >/dev/null || { echo "g++-13 is required" >&2; exit 1; }
if (( ! update_lock )); then
  : "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX:?set the Core prebuilt prefix}"
  : "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX:?set the zlink-cpp prebuilt prefix}"
  [[ -d "$ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX" ]] || { echo "Core prefix not found" >&2; exit 1; }
  [[ -d "$ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX" ]] || { echo "zlink-cpp prefix not found" >&2; exit 1; }
fi

work_dir="$(mktemp -d "$output_dir/.zlink-framework-cpp-package.XXXXXXXX")"
trap 'rm -rf "$work_dir"' EXIT
build_dir="$work_dir/build"
prefix="$work_dir/zlink-framework-cpp-$version-linux-x64"
conan_dir="$work_dir/conan"
conan_generators="$conan_dir/generators"
conan_profile="$conan_dir/zlink-bootstrap"
recipe="$source_dir/packaging/conan/conanfile.py"
lockfile="$source_dir/packaging/conan/conan.lock"
package_lockfile="$source_dir/packaging/conan/conan.packages.json"
staged_core_prefix="$work_dir/core-prefix"
staged_cpp_prefix="$work_dir/cpp-prefix"
mkdir -p "$conan_generators" "$staged_core_prefix" "$staged_cpp_prefix"

# The wrapper imports the recipe so its requirement and option policies have
# one owner. It omits only zlink-cpp, which is staged as this archive's local
# first-party input rather than resolved from Conan.
{
  printf '%s\n' \
    'from conan import ConanFile' \
    'import sys' \
    '' \
    "sys.path.insert(0, r\"$(dirname "$recipe")\")" \
    'from conanfile import ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS, ZlinkFrameworkConan' \
    '' \
    'class ZlinkBootstrap(ConanFile):' \
    '    name = "zlink-bootstrap"' \
    '    version = "0"' \
    '    settings = "os", "arch", "compiler", "build_type"' \
    '    default_options = {key: value for key, value in ZlinkFrameworkConan.default_options.items() if "/" in key}' \
    '    generators = "CMakeDeps", "CMakeToolchain"' \
    '' \
    '    def requirements(self):' \
    '        for requirement in ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS:' \
    '            self.requires(requirement, transitive_headers=True, transitive_libs=True)'
} > "$conan_dir/conanfile.py"

gcc_version="$(g++-13 -dumpfullversion -dumpversion)"
gcc_version="${gcc_version%%.*}"
{
  printf '%s\n' \
    '[settings]' \
    'os=Linux' \
    'arch=x86_64' \
    'build_type=Release' \
    'compiler=gcc' \
    "compiler.version=$gcc_version" \
    'compiler.libcxx=libstdc++11' \
    'compiler.cppstd=gnu20' \
    '*:compiler.cppstd=gnu17' \
    'zlink-bootstrap/*:compiler.cppstd=gnu20' \
    '' \
    '[conf]' \
    'tools.build:compiler_executables={"c": "gcc-13", "cpp": "g++-13"}'
} > "$conan_profile"

candidate_lockfile="$conan_dir/conan.lock"
conan lock create "$conan_dir" \
  "--profile:host=$conan_profile" \
  "--profile:build=$conan_profile" \
  "--lockfile-out=$candidate_lockfile"
candidate_package_lockfile="$conan_dir/conan.packages.json"

function write_package_lockfile() {
  local input_lockfile="$1"
  local output_lockfile="$2"
  local graph_file="$conan_dir/graph.json"

  conan graph info "$conan_dir" \
    "--profile:host=$conan_profile" \
    "--profile:build=$conan_profile" \
    "--lockfile=$input_lockfile" \
    --format=json > "$graph_file"
  python3 - "$graph_file" "$output_lockfile" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as graph_file:
    nodes = json.load(graph_file)["graph"]["nodes"].values()

package_revisions = {
    (node["ref"], node["package_id"], node["prev"])
    for node in nodes
    if node.get("ref") and node.get("package_id") and node.get("prev")
    and not node["ref"].startswith("zlink-bootstrap/")
}
packages = [
    {"ref": ref, "package_id": package_id, "prev": prev}
    for ref, package_id, prev in sorted(package_revisions)
]
if not packages:
    raise SystemExit("Conan graph did not resolve package revisions")

with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as output_file:
    json.dump({"version": 1, "packages": packages}, output_file, indent=2)
    output_file.write("\n")
PY
}

if (( ! update_lock )); then
  [[ -f "$lockfile" ]] || {
    echo "missing $lockfile; run $0 --update-lock" >&2
    exit 1
  }
  [[ -f "$package_lockfile" ]] || {
    echo "missing $package_lockfile; run $0 --update-lock" >&2
    exit 1
  }
  if ! cmp -s "$candidate_lockfile" "$lockfile"; then
    echo "$lockfile is out of date for $recipe; run $0 --update-lock" >&2
    exit 1
  fi
  write_package_lockfile "$lockfile" "$candidate_package_lockfile"
  if ! cmp -s "$candidate_package_lockfile" "$package_lockfile"; then
    echo "$package_lockfile is out of date for resolved Conan package revisions; run $0 --update-lock" >&2
    exit 1
  fi
fi
install_lockfile="$candidate_lockfile"
if (( ! update_lock )); then
  install_lockfile="$lockfile"
fi
conan install "$conan_dir" \
  "--profile:host=$conan_profile" \
  "--profile:build=$conan_profile" \
  "--lockfile=$install_lockfile" \
  --build=missing \
  --output-folder "$conan_generators"
write_package_lockfile "$install_lockfile" "$candidate_package_lockfile"
if ((update_lock)); then
  cp "$candidate_lockfile" "$lockfile"
  cp "$candidate_package_lockfile" "$package_lockfile"
  echo "updated $lockfile and $package_lockfile"
  exit 0
fi
if ! cmp -s "$candidate_package_lockfile" "$package_lockfile"; then
  echo "$package_lockfile does not match installed Conan package revisions; run $0 --update-lock" >&2
  exit 1
fi

# Local-package prefixes can contain absolute symlinks into the machine cache.
# Materialize those inputs so the resulting archive never retains host paths.
cp -aL "$ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX/." "$staged_core_prefix/"
cp -aL "$ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX/." "$staged_cpp_prefix/"

# Keep the prebuilt framework compilation at GNU C++20. The Conan recipe owner
# and profile above are the same path as tutorial/bootstrap.cmake. Core and
# zlink-cpp are staged at install.
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-13 \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_TOOLCHAIN_FILE="$conan_generators/conan_toolchain.cmake" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DZLINK_FRAMEWORK_CPP_SHARED=ON \
  -DZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST=ON \
  -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX="$staged_core_prefix" \
  -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX="$staged_cpp_prefix" \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_UNREAL=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_GODOT=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_AXMOL=OFF
cmake --build "$build_dir" --parallel 8
cmake --install "$build_dir"

for library in zlink_framework zlink_http_client zlink_stream_connector; do
  if nm -D --defined-only "$prefix/lib/lib${library}.so" | awk '{print $3}' | \
      grep -Eq '^(_ZN5boost|_ZN6google8protobuf|_ZN4absl|_ZN13opentelemetry)'; then
    echo "third-party dynamic export found in $library" >&2
    exit 1
  fi
done

archive="$output_dir/zlink-framework-cpp-$version-linux-x64.tar.gz"
source_date_epoch="${SOURCE_DATE_EPOCH:-0}"
[[ "$source_date_epoch" =~ ^[0-9]+$ ]] || {
  echo "SOURCE_DATE_EPOCH must be a non-negative integer" >&2
  exit 1
}
tar --sort=name --mtime="@$source_date_epoch" --owner=0 --group=0 --numeric-owner \
  -C "$work_dir" -cf - "$(basename "$prefix")" | gzip -n > "$archive"
echo "$archive"
