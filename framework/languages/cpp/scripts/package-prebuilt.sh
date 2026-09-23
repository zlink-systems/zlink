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
platform_manifest="$source_dir/packaging/prebuilt-platforms.json"
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
IFS='|' read -r c_compiler cxx_compiler compiler_version <<<"$("$python_command" - "$platform_manifest" "$platform" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as manifest_file:
    matches = [entry for entry in json.load(manifest_file)["include"]
               if entry["platform"] == sys.argv[2]]
if len(matches) != 1:
    raise SystemExit(f"expected one platform entry for {sys.argv[2]}")
entry = matches[0]
print("|".join((entry["c_compiler"], entry["cxx_compiler"],
                entry.get("compiler_version", ""))))
PY
)"
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
    command -v "$c_compiler" >/dev/null || { echo "$c_compiler is required for $platform" >&2; exit 1; }
    command -v "$cxx_compiler" >/dev/null || { echo "$cxx_compiler is required for $platform" >&2; exit 1; }
    gcc_version="$("$cxx_compiler" -dumpfullversion -dumpversion)"
    gcc_version="${gcc_version%%.*}"
    [[ "$gcc_version" == 13 ]] || { echo "$platform requires GCC 13, found $gcc_version" >&2; exit 1; }
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
    command -v "$cxx_compiler" >/dev/null || { echo "Apple clang is required" >&2; exit 1; }
    apple_clang_version="$("$cxx_compiler" --version | sed -n 's/^Apple clang version \([0-9][0-9]*\).*/\1/p' | head -n1)"
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
tools.build:compiler_executables={"c": "$c_compiler", "cpp": "$cxx_compiler"}
EOF
    compiler_args=(-DCMAKE_C_COMPILER="$c_compiler" -DCMAKE_CXX_COMPILER="$cxx_compiler")
    ;;
  windows-x64)
    command -v "$cxx_compiler" >/dev/null || { echo "MSVC cl.exe is required; run from an initialized VS 2022 environment" >&2; exit 1; }
    [[ "$compiler_version" =~ ^19[0-9]$ ]] || { echo "prebuilt-platforms.json must pin the Conan MSVC version" >&2; exit 1; }
    [[ -n "${VSINSTALLDIR:-}" ]] || { echo "VSINSTALLDIR is required to verify the MSVC toolset" >&2; exit 1; }
    vc_version_file="$(cygpath -u "$VSINSTALLDIR")/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
    vc_toolset="$(tr -d '\r\n' <"$vc_version_file")"
    [[ "$vc_toolset" =~ ^14\.([0-9])[0-9]\. ]] || { echo "could not determine the MSVC toolset version" >&2; exit 1; }
    detected_msvc_version="19${BASH_REMATCH[1]}"
    [[ "$detected_msvc_version" == "$compiler_version" ]] || {
      echo "windows-x64 requires MSVC $compiler_version, found $detected_msvc_version ($vc_toolset)" >&2
      exit 1
    }
    cat >"$conan_profile" <<EOF
[settings]
os=Windows
arch=x86_64
build_type=Release
compiler=msvc
compiler.version=$compiler_version
compiler.runtime=dynamic
compiler.cppstd=20
*:compiler.cppstd=17
zlink-bootstrap/*:compiler.cppstd=20

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
EOF
    compiler_args=(-DCMAKE_C_COMPILER="$c_compiler" -DCMAKE_CXX_COMPILER="$cxx_compiler")
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
    (node["ref"], node["package_id"])
    for node in nodes
    if node.get("ref") and node.get("package_id")
    and node.get("binary") != "Skip"
    and not node["ref"].startswith("zlink-bootstrap/")
})
if not packages:
    raise SystemExit("Conan graph did not resolve binary packages")
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as output_file:
    json.dump({"version": 1, "packages": [
        {"ref": ref, "package_id": package_id}
        for ref, package_id in packages
    ]}, output_file, indent=2)
    output_file.write("\n")
PY
}

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
if [[ ! -f "$package_lockfile" ]] || ! "$python_command" - "$candidate_package_lockfile" "$package_lockfile" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as candidate_file:
    candidate = json.load(candidate_file)
with open(sys.argv[2], encoding="utf-8") as committed_file:
    committed = json.load(committed_file)

def package_set(document):
    return {
        (package["ref"], package["package_id"])
        for package in document.get("packages", [])
    }

matches = (candidate.get("version") == committed.get("version") and
           package_set(candidate) == package_set(committed))
raise SystemExit(0 if matches else 1)
PY
then
  echo "Conan packages file to commit: $package_lockfile" >&2
  echo "----- BEGIN $package_lockfile -----" >&2
  cat "$candidate_package_lockfile" >&2
  echo "----- END $package_lockfile -----" >&2
  echo "run '$0 --update-lock --platform $platform' on $platform and commit both platform pin files" >&2
  exit 1
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

# Preserve the Core archive's relative library symlinks as part of its runtime
# closure. Core release packaging owns making those links relocatable.
cp -a "$core_input/." "$staged_core_prefix/"
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
      "$python_command" "$source_dir/scripts/verify-apple-exports.py" \
        "$source_dir/cmake/zlink-framework-shared-symbols.map" "$binary"
    done
    ;;
  macos-arm64)
    verify_macos_relative_reference() {
      local binary="$1"
      local reference_kind="$2"
      local reference="$3"
      local dependency_path
      local rpath
      local rpath_found=0

      case "$reference" in
        @loader_path/*)
          dependency_path="$(normalize_path "$(dirname "$binary")/${reference#@loader_path/}")"
          ;;
        @rpath/*)
          while IFS= read -r rpath; do
            rpath_found=1
            case "$rpath" in
              @loader_path|@loader_path/*) ;;
              *)
                echo "non-loader-relative LC_RPATH in $(basename "$binary"): $rpath" >&2
                exit 1
                ;;
            esac
          done < <(otool -l "$binary" | awk '
            $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
            in_rpath && $1 == "path" { print $2; in_rpath = 0 }
          ')
          ((rpath_found)) || {
            echo "missing LC_RPATH for $reference_kind in $(basename "$binary"): $reference" >&2
            exit 1
          }
          dependency_path="$prefix/lib/${reference#@rpath/}"
          ;;
        *)
          echo "non-relocatable $reference_kind in $(basename "$binary"): $reference" >&2
          exit 1
          ;;
      esac

      [[ -e "$dependency_path" ]] || {
        echo "unresolved $reference_kind in clean prefix: $(basename "$binary") -> $reference" >&2
        exit 1
      }
    }
    for library in "${libraries[@]}"; do
      binary="$(find "$prefix/lib" -maxdepth 1 -type f -name "lib${library}*.dylib" | head -n1)"
      [[ -n "$binary" ]] || { echo "missing shared library: lib${library}.dylib" >&2; exit 1; }
      install_name="$(otool -D "$binary" | tail -n +2 | head -n1)"
      verify_macos_relative_reference "$binary" "install name" "$install_name"
      exports="$(nm -gU "$binary")"
      grep -Eq ' (__ZN5zlink|__ZTVN5zlink|__ZTIN5zlink|__ZTSN5zlink)' <<<"$exports" || {
        echo "no zlink C++ exports found in $library" >&2; exit 1;
      }
      if grep -Eq ' (__ZN5boost|__ZN6google8protobuf|__ZN4absl|__ZN13opentelemetry)' <<<"$exports"; then
        echo "third-party dynamic export found in $library" >&2; exit 1
      fi
    done
    while IFS= read -r binary; do
      install_name="$(otool -D "$binary" | tail -n +2 | head -n1)"
      verify_macos_relative_reference "$binary" "install name" "$install_name"
      while IFS= read -r dependency; do
        case "$dependency" in
          /usr/lib/*|/System/Library/*) ;;
          *) verify_macos_relative_reference "$binary" "dependency" "$dependency" ;;
        esac
      done < <(otool -L "$binary" | tail -n +2 | sed 's/^[[:space:]]*//' | cut -d ' ' -f1)
    done < <(find "$prefix/lib" -maxdepth 1 -type f -name '*.dylib' -print)
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
    done
    while IFS= read -r binary; do
      while IFS= read -r dependency; do
        dependency_lower="$(tr '[:upper:]' '[:lower:]' <<<"$dependency")"
        case "$dependency_lower" in
          api-ms-win-*|ext-ms-win-*|kernel32.dll|user32.dll|advapi32.dll|ws2_32.dll|mswsock.dll|bcrypt.dll|crypt32.dll|secur32.dll|shell32.dll|ole32.dll|oleaut32.dll|gdi32.dll|ntdll.dll|rpcrt4.dll|shlwapi.dll|normaliz.dll|comdlg32.dll|winmm.dll|version.dll|iphlpapi.dll|psapi.dll|userenv.dll|wintrust.dll|ncrypt.dll|dnsapi.dll|combase.dll|cfgmgr32.dll|setupapi.dll|msvcp*.dll|vcruntime*.dll|ucrtbase.dll) ;;
          *)
            resolved=0
            while IFS= read -r packaged_dll; do
              if [[ "$(tr '[:upper:]' '[:lower:]' <<<"$(basename "$packaged_dll")")" == "$dependency_lower" ]]; then
                resolved=1
                break
              fi
            done < <(find "$prefix/bin" -maxdepth 1 -type f -iname '*.dll' -print)
            ((resolved)) || {
              echo "unresolved dependency in clean prefix: $(basename "$binary") -> $dependency" >&2
              exit 1
            }
            ;;
        esac
      done < <(MSYS2_ARG_CONV_EXCL=/DEPENDENTS dumpbin /DEPENDENTS "$(cygpath -w "$binary")" |
        sed -n 's/^[[:space:]]*\([^[:space:]]*[.][dD][lL][lL]\)[[:space:]]*$/\1/p')
    done < <(find "$prefix/bin" -maxdepth 1 -type f -iname '*.dll' -print)
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
if [[ "$platform" == macos-arm64 ]]; then
  archive_verify_dir="$work_dir/archive-verify"
  archive_prefix="$archive_verify_dir/$(basename "$prefix")"
  quickstart_build="$work_dir/quickstart-build"
  mkdir -p "$archive_verify_dir"
  "$tar_command" -xzf "$archive" -C "$archive_verify_dir"
  while IFS= read -r binary; do
    codesign --verify --deep --strict "$binary"
  done < <(find "$archive_prefix/lib" -maxdepth 1 -type f -name '*.dylib' -print)
  cmake -S "$source_dir/quickstart" -B "$quickstart_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$cxx_compiler" \
    -DCMAKE_PREFIX_PATH="$archive_prefix" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$archive_prefix/lib"
  cmake --build "$quickstart_build" --parallel 8
  "$archive_prefix/lib/quickstart_server" >"$work_dir/quickstart-server.log" 2>&1 &
  quickstart_server_pid=$!
  "$archive_prefix/lib/quickstart_client" >"$work_dir/quickstart-client.log" 2>&1 &
  quickstart_client_pid=$!
  quickstart_answer=""
  for _ in $(seq 1 60); do
    quickstart_answer="$(curl -sf http://127.0.0.1:5083/hello/world || true)"
    [[ "$quickstart_answer" == *'"hello, world"'* ]] && break
    sleep 1
  done
  kill "$quickstart_client_pid" "$quickstart_server_pid" 2>/dev/null || true
  wait "$quickstart_client_pid" "$quickstart_server_pid" 2>/dev/null || true
  if [[ "$quickstart_answer" != *'"hello, world"'* ]]; then
    cat "$work_dir/quickstart-server.log" "$work_dir/quickstart-client.log" >&2
    echo "quickstart failed against the unpacked macOS archive" >&2
    exit 1
  fi
fi
(
  cd "$output_dir"
  archive_name="$(basename "$archive")"
  sha256_file "$archive_name" >"$archive_name.sha256"
)
echo "$archive"
