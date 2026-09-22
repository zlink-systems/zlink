#!/usr/bin/env bash
# Build and verify a shared Framework C++ archive for the current release host.
set -euo pipefail

platform=""
update_lock=0
output_arg=""
usage() {
  echo "usage: $0 [--platform PLATFORM] <output-dir> | --update-lock [--platform PLATFORM]" >&2
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) platform="${2:-}"; shift 2 ;;
    --update-lock) update_lock=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *)
      [[ -z "$output_arg" ]] || { usage; exit 2; }
      output_arg="$1"
      shift
      ;;
  esac
done
if ((update_lock)); then
  [[ -z "$output_arg" ]] || { usage; exit 2; }
elif [[ -z "$output_arg" ]]; then
  usage
  exit 2
fi

host_os=""
host_arch=""
case "$(uname -s)" in
  Linux*) host_os=linux ;;
  Darwin*) host_os=macos ;;
  MINGW*|MSYS*|CYGWIN*) host_os=windows ;;
  *) echo "unsupported host operating system: $(uname -s)" >&2; exit 2 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) host_arch=x64; conan_arch=x86_64 ;;
  aarch64|arm64) host_arch=arm64; conan_arch=armv8 ;;
  *) echo "unsupported host architecture: $(uname -m)" >&2; exit 2 ;;
esac
host_platform="$host_os-$host_arch"
platform="${platform:-$host_platform}"
case "$platform" in
  linux-x64|linux-arm64|macos-arm64|windows-x64) ;;
  *) echo "unsupported platform: $platform" >&2; exit 2 ;;
esac
[[ "$platform" == "$host_platform" ]] || {
  echo "platform $platform must be built on its matching host (current host: $host_platform)" >&2
  exit 2
}

normalize_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$(cygpath -am "$1")"
  elif realpath -m / >/dev/null 2>&1; then
    realpath -m "$1"
  else
    python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$1"
  fi
}
sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1"
  else
    shasum -a 256 "$1"
  fi
}

if ((update_lock)); then
  output_dir="${HOME}/.cache/zlink"
else
  mkdir -p "$output_arg"
  output_dir="$(normalize_path "$output_arg")"
fi
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "$script_dir/.." && pwd)"
version="$(sed -n 's/^ZLINK_FRAMEWORK_VERSION=//p' "$source_dir/VERSION")"
[[ -n "$version" ]] || { echo "missing framework VERSION" >&2; exit 1; }

command -v conan >/dev/null || { echo "Conan 2 is required" >&2; exit 1; }
command -v cmake >/dev/null || { echo "CMake is required" >&2; exit 1; }
command -v ninja >/dev/null || { echo "Ninja is required" >&2; exit 1; }
if command -v python3 >/dev/null 2>&1; then
  python_command=python3
elif command -v python >/dev/null 2>&1; then
  python_command=python
else
  echo "Python 3 is required" >&2
  exit 1
fi
if (( ! update_lock )); then
  : "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX:?set the Core prebuilt prefix}"
  : "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX:?set the zlink-cpp prebuilt prefix}"
  core_input="$(normalize_path "$ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX")"
  cpp_input="$(normalize_path "$ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX")"
  [[ -d "$core_input" ]] || { echo "Core prefix not found: $core_input" >&2; exit 1; }
  [[ -d "$cpp_input" ]] || { echo "zlink-cpp prefix not found: $cpp_input" >&2; exit 1; }
fi

work_dir="$(mktemp -d "$output_dir/.zlink-framework-cpp-package.XXXXXXXX")"
trap 'rm -rf "$work_dir"' EXIT
build_dir="$work_dir/build"
prefix="$work_dir/zlink-framework-cpp-$version-$platform"
conan_dir="$work_dir/conan"
conan_generators="$conan_dir/generators"
conan_profile="$conan_dir/zlink-bootstrap"
recipe="$source_dir/packaging/conan/conanfile.py"
lockfile="$source_dir/packaging/conan/conan.$platform.lock"
package_lockfile="$source_dir/packaging/conan/conan.$platform.packages.json"
staged_core_prefix="$work_dir/core-prefix"
staged_cpp_prefix="$work_dir/cpp-prefix"
mkdir -p "$conan_generators" "$staged_core_prefix" "$staged_cpp_prefix"

# This wrapper imports the recipe so dependency names, versions and options
# continue to have one owner. zlink-cpp is a staged first-party input.
"$python_command" - "$recipe" "$conan_dir/conanfile.py" <<'PY'
import pathlib
import sys

recipe = pathlib.Path(sys.argv[1]).resolve()
pathlib.Path(sys.argv[2]).write_text(f'''from conan import ConanFile
import sys

sys.path.insert(0, {str(recipe.parent)!r})
from conanfile import ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS, ZlinkFrameworkConan

class ZlinkBootstrap(ConanFile):
    name = "zlink-bootstrap"
    version = "0"
    settings = "os", "arch", "compiler", "build_type"
    default_options = {{key: value for key, value in ZlinkFrameworkConan.default_options.items() if "/" in key}}
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        for requirement in ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS:
            self.requires(requirement, transitive_headers=True, transitive_libs=True)
''', encoding='utf-8', newline='\n')
PY

compiler_args=()
case "$platform" in
  linux-*)
    if command -v g++-13 >/dev/null 2>&1; then
      c_compiler=gcc-13
      cxx_compiler=g++-13
    else
      command -v g++ >/dev/null || { echo "g++ is required" >&2; exit 1; }
      c_compiler=gcc
      cxx_compiler=g++
    fi
    gcc_version="$("$cxx_compiler" -dumpfullversion -dumpversion)"
    gcc_version="${gcc_version%%.*}"
    cat >"$conan_profile" <<EOF
[settings]
os=Linux
arch=$conan_arch
build_type=Release
compiler=gcc
compiler.version=$gcc_version
compiler.libcxx=libstdc++11
compiler.cppstd=gnu20
*:compiler.cppstd=gnu17
zlink-bootstrap/*:compiler.cppstd=gnu20

[conf]
tools.build:compiler_executables={"c": "$c_compiler", "cpp": "$cxx_compiler"}
EOF
    compiler_args=(-DCMAKE_C_COMPILER="$c_compiler" -DCMAKE_CXX_COMPILER="$cxx_compiler")
    ;;
  macos-arm64)
    command -v clang++ >/dev/null || { echo "Apple clang is required" >&2; exit 1; }
    apple_clang_version="$(clang++ --version | sed -n 's/^Apple clang version \([0-9][0-9]*\).*/\1/p' | head -n1)"
    [[ -n "$apple_clang_version" ]] || { echo "could not determine Apple clang version" >&2; exit 1; }
    cat >"$conan_profile" <<EOF
[settings]
os=Macos
arch=armv8
build_type=Release
compiler=apple-clang
compiler.version=$apple_clang_version
compiler.libcxx=libc++
compiler.cppstd=gnu20
*:compiler.cppstd=gnu17
zlink-bootstrap/*:compiler.cppstd=gnu20

[conf]
tools.build:compiler_executables={"c": "clang", "cpp": "clang++"}
EOF
    compiler_args=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
    ;;
  windows-x64)
    command -v cl >/dev/null || { echo "MSVC cl.exe is required; run from an initialized VS 2022 environment" >&2; exit 1; }
    msvc_toolset="${ZLINK_FRAMEWORK_CPP_MSVC_VERSION:-}"
    if [[ -z "$msvc_toolset" && -n "${VSINSTALLDIR:-}" ]]; then
      vc_version_file="$(cygpath -u "$VSINSTALLDIR")/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
      vc_toolset="$(tr -d '\r\n' <"$vc_version_file")"
      if [[ "$vc_toolset" =~ ^14\.([0-9])[0-9]\. ]]; then
        msvc_toolset="19${BASH_REMATCH[1]}"
      fi
    fi
    [[ "$msvc_toolset" =~ ^19[0-9]$ ]] || { echo "could not determine the Conan MSVC version from cl.exe" >&2; exit 1; }
    cat >"$conan_profile" <<EOF
[settings]
os=Windows
arch=x86_64
build_type=Release
compiler=msvc
compiler.version=$msvc_toolset
compiler.runtime=dynamic
compiler.cppstd=20
*:compiler.cppstd=17
zlink-bootstrap/*:compiler.cppstd=20

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
EOF
    compiler_args=(-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl)
    ;;
esac

candidate_lockfile="$conan_dir/conan.lock"
if ((update_lock)); then
  conan lock create "$conan_dir" \
    "--profile:host=$conan_profile" \
    "--profile:build=$conan_profile" \
    "--lockfile-out=$candidate_lockfile"
else
  [[ -f "$lockfile" ]] || { echo "missing $lockfile; run $0 --update-lock --platform $platform on that host" >&2; exit 1; }
  cp "$lockfile" "$candidate_lockfile"
fi
candidate_package_lockfile="$conan_dir/conan.packages.json"

write_package_lockfile() {
  local input_lockfile="$1"
  local output_lockfile="$2"
  local graph_file="$conan_dir/graph.json"
  conan graph info "$conan_dir" \
    "--profile:host=$conan_profile" \
    "--profile:build=$conan_profile" \
    "--lockfile=$input_lockfile" \
    --format=json >"$graph_file"
  "$python_command" - "$graph_file" "$output_lockfile" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as graph_file:
    nodes = json.load(graph_file)["graph"]["nodes"].values()
packages = sorted({
    (node["ref"], node["package_id"], node["prev"])
    for node in nodes
    if node.get("ref") and node.get("package_id") and node.get("prev")
    and not node["ref"].startswith("zlink-bootstrap/")
})
if not packages:
    raise SystemExit("Conan graph did not resolve package revisions")
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as output_file:
    json.dump({"version": 1, "packages": [
        {"ref": ref, "package_id": package_id, "prev": prev}
        for ref, package_id, prev in packages
    ]}, output_file, indent=2)
    output_file.write("\n")
PY
}

if (( ! update_lock )); then
  if [[ -f "$package_lockfile" ]]; then
    write_package_lockfile "$candidate_lockfile" "$candidate_package_lockfile"
    cmp -s "$candidate_package_lockfile" "$package_lockfile" || {
      echo "$package_lockfile is out of date for resolved Conan package revisions" >&2
      exit 1
    }
  fi
fi
conan install "$conan_dir" \
  "--profile:host=$conan_profile" \
  "--profile:build=$conan_profile" \
  "--lockfile=$candidate_lockfile" \
  --build=missing \
  --output-folder "$conan_generators"
write_package_lockfile "$candidate_lockfile" "$candidate_package_lockfile"
if ((update_lock)); then
  cp "$candidate_lockfile" "$lockfile"
  cp "$candidate_package_lockfile" "$package_lockfile"
  echo "updated $lockfile and $package_lockfile"
  exit 0
fi
if [[ -f "$package_lockfile" ]]; then
  cmp -s "$candidate_package_lockfile" "$package_lockfile" || {
    echo "$package_lockfile does not match installed Conan package revisions" >&2
    exit 1
  }
fi

# The shared Framework and staged Core configs retain their public compile/link
# contracts for nlohmann_json and OpenSSL. Stage those development inputs so
# consumers need only this archive on CMAKE_PREFIX_PATH.
conan_package_folder() {
  local package_name="$1"
  local package_reference
  local package_folder_raw
  package_reference="$("$python_command" - "$conan_dir/graph.json" "$package_name" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as graph_file:
    nodes = json.load(graph_file)["graph"]["nodes"].values()
packages = [
    f'{node["ref"]}:{node["package_id"]}#{node["prev"]}'
    for node in nodes
    if node.get("ref", "").startswith(f"{sys.argv[2]}/")
    and node.get("package_id")
    and node.get("prev")
]
if len(packages) != 1:
    raise SystemExit(f"expected one {sys.argv[2]} package, found {len(packages)}")
print(packages[0])
PY
)"
  package_folder_raw="$(conan cache path "$package_reference")"
  normalize_path "$package_folder_raw"
}
nlohmann_package_folder="$(conan_package_folder nlohmann_json)"
openssl_package_folder="$(conan_package_folder openssl)"
mkdir -p "$prefix/include" "$prefix/lib/cmake/nlohmann_json"
cp -aL "$nlohmann_package_folder/include/." "$prefix/include/"
cp -aL "$openssl_package_folder/include/." "$prefix/include/"
cp -aL "$openssl_package_folder/lib/." "$prefix/lib/"
cat >"$prefix/lib/cmake/nlohmann_json/nlohmann_jsonConfig.cmake" <<'CMAKE'
get_filename_component(_nlohmann_json_prefix
  "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET nlohmann_json::nlohmann_json)
  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
  set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_nlohmann_json_prefix}/include")
endif()
unset(_nlohmann_json_prefix)
CMAKE

# Materialize absolute symlinks from local-package caches so the archive has no
# references to the build machine.
cp -aL "$core_input/." "$staged_core_prefix/"
cp -aL "$cpp_input/." "$staged_cpp_prefix/"

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  "${compiler_args[@]}" \
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

libraries=(zlink_framework zlink_http_client zlink_stream_connector)
case "$platform" in
  linux-*)
    for library in "${libraries[@]}"; do
      binary="$prefix/lib/lib${library}.so"
      [[ -f "$binary" ]] || { echo "missing shared library: $binary" >&2; exit 1; }
      exports="$(nm -D --defined-only "$binary")"
      grep -Eq ' (_ZN5zlink|_ZTVN5zlink|_ZTIN5zlink|_ZTSN5zlink)' <<<"$exports" || {
        echo "no zlink C++ exports found in $library" >&2; exit 1;
      }
      if grep -Eq ' (_ZN5boost|_ZN6google8protobuf|_ZN4absl|_ZN13opentelemetry)' <<<"$exports"; then
        echo "third-party dynamic export found in $library" >&2; exit 1
      fi
    done
    ;;
  macos-arm64)
    for library in "${libraries[@]}"; do
      binary="$(find "$prefix/lib" -maxdepth 1 -type f -name "lib${library}*.dylib" | head -n1)"
      [[ -n "$binary" ]] || { echo "missing shared library: lib${library}.dylib" >&2; exit 1; }
      install_name="$(otool -D "$binary" | tail -n +2 | head -n1)"
      [[ "$install_name" == @loader_path/* ]] || { echo "non-relocatable install name: $install_name" >&2; exit 1; }
      exports="$(nm -gU "$binary")"
      grep -Eq ' (__ZN5zlink|__ZTVN5zlink|__ZTIN5zlink|__ZTSN5zlink)' <<<"$exports" || {
        echo "no zlink C++ exports found in $library" >&2; exit 1;
      }
      if grep -Eq ' (__ZN5boost|__ZN6google8protobuf|__ZN4absl|__ZN13opentelemetry)' <<<"$exports"; then
        echo "third-party dynamic export found in $library" >&2; exit 1
      fi
    done
    ;;
  windows-x64)
    for library in "${libraries[@]}"; do
      binary="$prefix/bin/${library}.dll"
      [[ -f "$binary" ]] || { echo "missing shared library: $binary" >&2; exit 1; }
      exports="$(MSYS2_ARG_CONV_EXCL=/EXPORTS dumpbin /EXPORTS "$(cygpath -w "$binary")")"
      grep -Eq '\?[^ ]*@zlink@@' <<<"$exports" || { echo "no zlink C++ exports found in $library" >&2; exit 1; }
      if grep -Eiq 'boost|protobuf|absl|opentelemetry' <<<"$exports"; then
        echo "third-party dynamic export found in $library" >&2; exit 1
      fi
      dependencies="$(MSYS2_ARG_CONV_EXCL=/DEPENDENTS dumpbin /DEPENDENTS "$(cygpath -w "$binary")")"
      if grep -Eiq 'boost|protobuf|absl|opentelemetry|libssl|libcrypto|lz4' <<<"$dependencies"; then
        echo "third-party DLL dependency found in $library" >&2; exit 1
      fi
    done
    ;;
esac

archive="$output_dir/zlink-framework-cpp-$version-$platform.tar.gz"
source_date_epoch="${SOURCE_DATE_EPOCH:-0}"
[[ "$source_date_epoch" =~ ^[0-9]+$ ]] || { echo "SOURCE_DATE_EPOCH must be a non-negative integer" >&2; exit 1; }
tar_command=tar
if [[ "$platform" == macos-arm64 ]]; then
  command -v gtar >/dev/null 2>&1 || { echo "GNU tar (gtar) is required on macOS" >&2; exit 1; }
  tar_command=gtar
fi
"$tar_command" --sort=name --mtime="@$source_date_epoch" --owner=0 --group=0 --numeric-owner \
  -C "$work_dir" -cf - "$(basename "$prefix")" | gzip -n >"$archive"
(
  cd "$output_dir"
  archive_name="$(basename "$archive")"
  sha256_file "$archive_name" >"$archive_name.sha256"
)
echo "$archive"
