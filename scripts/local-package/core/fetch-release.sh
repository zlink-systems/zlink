#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
repo_version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
version="${ZLINK_CORE_RELEASE_VERSION:-$repo_version}"
cache_root="${ZLINK_CORE_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/zlink/core}"
platform="${ZLINK_CORE_RELEASE_PLATFORM:-}"
force=0
allow_version_mismatch=0

usage() {
  cat <<'EOF'
Usage: fetch-release.sh [--version VERSION] [--platform PLATFORM]
                        [--cache-dir ABSOLUTE_DIR] [--force]
                        [--allow-version-mismatch]

Downloads a tagged Core release, verifies its release provenance and platform
checksums, and materializes a standard Core install prefix. The final prefix
is printed to stdout.

Supported platforms: linux-x64, linux-arm64, macos-x64, macos-arm64,
windows-x64, windows-arm64.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) version="${2:-}"; shift 2 ;;
    --platform) platform="${2:-}"; shift 2 ;;
    --cache-dir) cache_root="${2:-}"; shift 2 ;;
    --force) force=1; shift ;;
    --allow-version-mismatch) allow_version_mismatch=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Core release version must be MAJOR.MINOR.PATCH: ${version:-<missing>}" >&2
  exit 2
}
[[ "$version" = "$repo_version" || "$allow_version_mismatch" -eq 1 ]] || {
  echo "Core release version $version must match repository VERSION $repo_version" >&2
  exit 2
}

if [[ -z "$platform" ]]; then
  case "$(uname -s)" in
    Linux*)
      case "$(uname -m)" in
        x86_64|amd64) platform="linux-x64" ;;
        aarch64|arm64) platform="linux-arm64" ;;
        *) echo "Unsupported Linux architecture: $(uname -m)" >&2; exit 2 ;;
      esac
      ;;
    Darwin*)
      case "$(uname -m)" in
        x86_64|amd64) platform="macos-x64" ;;
        arm64|aarch64) platform="macos-arm64" ;;
        *) echo "Unsupported macOS architecture: $(uname -m)" >&2; exit 2 ;;
      esac
      ;;
    MINGW*|MSYS*|CYGWIN*)
      case "$(uname -m)" in
        x86_64|amd64) platform="windows-x64" ;;
        aarch64|arm64) platform="windows-arm64" ;;
        *) echo "Unsupported Windows architecture: $(uname -m)" >&2; exit 2 ;;
      esac
      ;;
    *) echo "Unsupported operating system: $(uname -s)" >&2; exit 2 ;;
  esac
fi

case "$platform" in
  linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64|windows-arm64) ;;
  *) echo "Unsupported Core release platform: $platform" >&2; exit 2 ;;
esac

if [[ "$cache_root" != /* ]]; then
  cache_root="$repo_root/$cache_root"
fi
cache_root="$(realpath -m "$cache_root")"
prefix="$cache_root/$version/$platform"
case "$prefix" in
  "$cache_root"/*) ;;
  *) echo "Core release prefix escaped cache root: $prefix" >&2; exit 2 ;;
esac

manifest="$prefix/share/zlink/core-package-provenance.json"
if [[ "$force" -eq 0 && -f "$manifest" ]]; then
  manifest_version="$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -n1)"
  if [[ "$manifest_version" = "$version" ]]; then
    printf '%s\n' "$prefix"
    exit 0
  fi
fi

command -v curl >/dev/null 2>&1 || {
  echo "curl is required to download the Core release" >&2
  exit 1
}
command -v sha256sum >/dev/null 2>&1 || {
  echo "sha256sum is required to verify the Core release" >&2
  exit 1
}
command -v node >/dev/null 2>&1 || {
  echo "node is required to write Core release provenance" >&2
  exit 1
}

release_tag="core/v${version}"
release_base="https://github.com/zlink-systems/zlink/releases/download/${release_tag}"
binary_name="libzlink-${platform}"
if [[ "$platform" == windows-* ]]; then
  binary_archive_name="${binary_name}.zip"
else
  binary_archive_name="${binary_name}.tar.gz"
fi

mkdir -p "$(dirname "$prefix")"
work="$(mktemp -d "$(dirname "$prefix")/.download.XXXXXX")"
cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT

download() {
  local url="$1"
  local output="$2"
  echo "Downloading ${url}" >&2
  curl -fsSL --retry 3 --retry-delay 1 "$url" -o "$output"
}

binary_archive="$work/$binary_archive_name"
source_archive="$work/zlink-${version}-source.tar.gz"
checksums="$work/checksums.txt"
release_provenance="$work/release-provenance.txt"
download "$release_base/$binary_archive_name" "$binary_archive"
download "$release_base/zlink-${version}-source.tar.gz" "$source_archive"
download "$release_base/checksums.txt" "$checksums"
download "$release_base/release-provenance.txt" "$release_provenance"

read_release_value() {
  local key="$1"
  sed -n "s/^${key}=//p" "$release_provenance" | head -n1
}

[[ "$(read_release_value tag)" = "$release_tag" ]] || {
  echo "Release provenance tag does not match: $release_tag" >&2
  exit 1
}
[[ "$(read_release_value runtime_version)" = "$version" ]] || {
  echo "Release provenance version does not match: $version" >&2
  exit 1
}
expected_checksums_sha="$(read_release_value checksums_sha256)"
actual_checksums_sha="$(sha256sum "$checksums" | awk '{print $1}')"
[[ "$actual_checksums_sha" = "$expected_checksums_sha" ]] || {
  echo "Release checksums.txt SHA-256 mismatch" >&2
  exit 1
}
expected_source_sha="$(read_release_value source_archive_sha256)"
actual_source_sha="$(sha256sum "$source_archive" | awk '{print $1}')"
[[ "$actual_source_sha" = "$expected_source_sha" ]] || {
  echo "Release source archive SHA-256 mismatch" >&2
  exit 1
}

binary_root="$work/binary"
mkdir -p "$binary_root"
if [[ "$binary_archive_name" == *.zip ]]; then
  command -v unzip >/dev/null 2>&1 || {
    echo "unzip is required to extract the Windows Core release" >&2
    exit 1
  }
  unzip -q "$binary_archive" -d "$binary_root"
else
  tar -xzf "$binary_archive" -C "$binary_root"
fi

binary_prefix="$binary_root/$binary_name"
[[ -d "$binary_prefix" ]] || {
  echo "Core release archive has an unexpected root: $binary_name" >&2
  exit 1
}
grep "  \./${binary_name}/" "$checksums" >"$work/platform-checksums.txt" || {
  echo "Core release checksums do not contain platform $platform" >&2
  exit 1
}
(
  cd "$binary_root"
  sha256sum -c "$work/platform-checksums.txt" >&2
)

source_root="$work/source"
mkdir -p "$source_root"
tar -xzf "$source_archive" -C "$source_root"
[[ -d "$source_root/core/include" ]] || {
  echo "Core source archive does not contain core/include" >&2
  exit 1
}

stage="$work/prefix"
mkdir -p "$stage/include" "$stage/share/zlink"
cp -a "$source_root/core/include/." "$stage/include/"
cp -a "$binary_prefix/include/." "$stage/include/"
cp "$checksums" "$stage/share/zlink/release-checksums.txt"
cp "$release_provenance" "$stage/share/zlink/release-provenance.txt"

case "$platform" in
  linux-*)
    runtime="$binary_prefix/libzlink.so"
    [[ -f "$runtime" ]] || { echo "Linux Core runtime is missing: $runtime" >&2; exit 1; }
    mkdir -p "$stage/lib"
    install -m 0755 "$runtime" "$stage/lib/libzlink.so.$version"
    ln -s "libzlink.so.$version" "$stage/lib/libzlink.so.0"
    ln -s "libzlink.so.0" "$stage/lib/libzlink.so"
    runtime_path="lib/libzlink.so.$version"
    runtime_soname="libzlink.so.0"
    ;;
  macos-*)
    runtime="$binary_prefix/libzlink.dylib"
    [[ -f "$runtime" ]] || { echo "macOS Core runtime is missing: $runtime" >&2; exit 1; }
    mkdir -p "$stage/lib"
    install -m 0755 "$runtime" "$stage/lib/libzlink.dylib"
    runtime_path="lib/libzlink.dylib"
    runtime_soname=""
    ;;
  windows-*)
    [[ -f "$binary_prefix/bin/zlink.dll" ]] || {
      echo "Windows Core runtime is missing: $binary_prefix/bin/zlink.dll" >&2
      exit 1
    }
    mkdir -p "$stage/bin" "$stage/lib"
    cp -a "$binary_prefix/bin/." "$stage/bin/"
    cp -a "$binary_prefix/lib/." "$stage/lib/"
    runtime_path="bin/zlink.dll"
    runtime_soname=""
    ;;
esac

PREFIX="$stage" VERSION="$version" PLATFORM="$platform" \
  SOURCE_SHA="$(read_release_value source_sha)" TAG="$release_tag" \
  CHECKSUMS_SHA="$actual_checksums_sha" RUNTIME_PATH="$runtime_path" \
  RUNTIME_SONAME="$runtime_soname" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const root = process.env.PREFIX;
const version = process.env.VERSION;
const runtimePath = process.env.RUNTIME_PATH;
const files = [];

if (process.env.PLATFORM.startsWith('linux-')) {
  const cmakeDir = path.join(root, 'lib', 'cmake', 'zlink');
  fs.mkdirSync(cmakeDir, {recursive: true});
  fs.writeFileSync(path.join(cmakeDir, 'zlinkConfig.cmake'), `\
# Materialized by scripts/local-package/core/fetch-release.sh.
get_filename_component(_zlink_prefix "\${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET libzlink)
  add_library(libzlink SHARED IMPORTED)
  set_target_properties(libzlink PROPERTIES
    IMPORTED_LOCATION "\${_zlink_prefix}/${runtimePath}"
    INTERFACE_INCLUDE_DIRECTORIES "\${_zlink_prefix}/include")
endif()
set(zlink_INCLUDE_DIR "\${_zlink_prefix}/include")
set(zlink_LIBRARY "\${_zlink_prefix}/${runtimePath}")
set(zlink_FOUND TRUE)
unset(_zlink_prefix)
`);
  fs.writeFileSync(path.join(cmakeDir, 'zlinkConfigVersion.cmake'), `\
set(PACKAGE_VERSION "${version}")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
  endif()
endif()
`);
}

function walk(directory) {
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) walk(full);
    else if (entry.isFile()) {
      files.push({
        path: path.relative(root, full).split(path.sep).join('/'),
        sha256: crypto.createHash('sha256').update(fs.readFileSync(full)).digest('hex'),
      });
    }
  }
}

walk(root);
files.sort((a, b) => a.path.localeCompare(b.path, 'en'));
const runtime = path.join(root, runtimePath);
const manifest = {
  schema: 1,
  package: 'zlink-core',
  version,
  abiMajor: 0,
  runtime: {
    path: runtimePath,
    sha256: crypto.createHash('sha256').update(fs.readFileSync(runtime)).digest('hex'),
    soname: process.env.RUNTIME_SONAME || null,
  },
  source: {revision: process.env.SOURCE_SHA, dirty: false},
  release: {tag: process.env.TAG, checksumsSha256: process.env.CHECKSUMS_SHA},
  files,
};
fs.mkdirSync(path.join(root, 'share', 'zlink'), {recursive: true});
fs.writeFileSync(
  path.join(root, 'share', 'zlink', 'core-package-provenance.json'),
  `${JSON.stringify(manifest, null, 2)}\n`,
);
NODE

if [[ "$platform" == linux-* ]]; then
  verify_args=(--prefix "$stage")
  if [[ "$allow_version_mismatch" -eq 1 ]]; then
    verify_args+=(--allow-version-mismatch)
  fi
  PREFIX="$stage" bash "$script_dir/verify-package.sh" "${verify_args[@]}" >/dev/null
fi

rm -rf "$prefix"
mv "$stage" "$prefix"
echo "Core release installed: version=$version platform=$platform prefix=$prefix" >&2
printf '%s\n' "$prefix"
