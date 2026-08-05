#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=release-tag.sh
source "${script_dir}/release-tag.sh"

if [ $# -lt 1 ]; then
  echo "usage: $0 <release-url-or-tag>" >&2
  echo "env:   ZLINK_RELEASE_REPO=owner/repo (optional override)" >&2
  exit 1
fi

input="$1"
repo="${ZLINK_RELEASE_REPO:-}"
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"

if [ -z "$repo" ]; then
  origin_url="$(git -C "$repo_root" remote get-url origin 2>/dev/null || true)"
  if [ -n "$origin_url" ]; then
    repo_path="$origin_url"
    repo_path="${repo_path#*://}"
    repo_path="${repo_path#*@}"
    if [[ "$repo_path" == *:* ]]; then
      repo_path="${repo_path#*:}"
    else
      repo_path="${repo_path#*/}"
    fi
    repo_path="${repo_path%.git}"
    if [[ "$repo_path" =~ ^[^/]+/[^/]+$ ]]; then
      repo="$repo_path"
    fi
  fi
fi

if [ -z "$repo" ]; then
  echo "unable to resolve GitHub repo; set ZLINK_RELEASE_REPO=owner/repo" >&2
  exit 1
fi

zlink_parse_core_release_ref "$input"
tag="${ZLINK_CORE_RELEASE_TAG}"
runtime_version="${ZLINK_CORE_RUNTIME_VERSION}"

if ! command -v gh >/dev/null 2>&1; then
  echo "gh CLI required" >&2
  exit 1
fi

workdir="/tmp/zlink_release_fetch"
rm -rf "$workdir"
mkdir -p "$workdir"

pushd "$workdir" >/dev/null

gh release download "$tag" -R "$repo"

mkdir -p extracted
shopt -s nullglob
for f in libzlink-*.tar.gz; do
  tar -xzf "$f" -C extracted
done

for f in libzlink_c-*.tar.gz; do
  tar -xzf "$f" -C extracted
done

for pkg in zlink-node-*.tar.gz; do
  tar -xzf "$pkg" -C extracted
done

if [ ! -f checksums.txt ]; then
  echo "core release is missing checksums.txt: $tag" >&2
  exit 1
fi
if [ ! -f release-provenance.txt ]; then
  echo "core release is missing release-provenance.txt: $tag" >&2
  exit 1
fi
source_archive="zlink-${runtime_version}-source.tar.gz"
if [ ! -f "${source_archive}" ]; then
  echo "core release is missing source archive: ${source_archive}" >&2
  exit 1
fi
if [ ! -f extracted/libzlink-linux-x64/include/zlink.h ]; then
  echo "core release is missing the linux-x64 public version header" >&2
  exit 1
fi

encoded_tag="${ZLINK_CORE_RELEASE_TAG_ENCODED}"
resolved_source_sha="$(gh api "repos/${repo}/commits/${encoded_tag}" --jq .sha)"
"${script_dir}/verify-release-provenance.sh" \
  --tag "${tag}" \
  --repo "${repo}" \
  --manifest release-provenance.txt \
  --checksums checksums.txt \
  --source-archive "${source_archive}" \
  --header extracted/libzlink-linux-x64/include/zlink.h \
  --resolved-source-sha "${resolved_source_sha}"
(
  cd extracted
  sha256sum --check ../checksums.txt
)

copy() { src="$1" dst="$2"; if [ -f "$src" ]; then install -D -m 0755 "$src" "$dst"; fi }
copy_windows_dll() {
  pkg_dir="$1"
  dst="$2"
  dll=""
  if [ -d "$pkg_dir/bin" ]; then
    dll="$(find "$pkg_dir/bin" -maxdepth 1 -type f -name 'zlink.dll' | head -n 1 || true)"
    if [ -z "$dll" ]; then
      dll="$(find "$pkg_dir/bin" -maxdepth 1 -type f -name 'libzlink-*.dll' | head -n 1 || true)"
    fi
  fi
  if [ -n "$dll" ] && [ -f "$dll" ]; then
    install -D -m 0755 "$dll" "$dst"
  fi
}
copy_windows_bin_dlls() {
  pkg_dir="$1"
  dst_dir="$2"
  if [ ! -d "$pkg_dir/bin" ]; then
    return
  fi
  mkdir -p "$dst_dir"
  find "$pkg_dir/bin" -maxdepth 1 -type f -name '*.dll' -print0 |
    while IFS= read -r -d '' dll; do
      install -m 0755 "$dll" "$dst_dir/$(basename "$dll")"
    done
}
copy_windows_c_dll() {
  pkg_dir="$1"
  dst="$2"
  dll=""
  if [ -d "$pkg_dir/bin" ]; then
    dll="$(find "$pkg_dir/bin" -maxdepth 1 -type f -name 'zlink_c.dll' | head -n 1 || true)"
    if [ -z "$dll" ]; then
      dll="$(find "$pkg_dir/bin" -maxdepth 1 -type f -name 'libzlink_c-*.dll' | head -n 1 || true)"
    fi
  fi
  if [ -n "$dll" ] && [ -f "$dll" ]; then
    install -D -m 0755 "$dll" "$dst"
  fi
}
copy_linux_soname() {
  pkg_dir="$1"
  dst_dir="$2"
  if [ ! -d "$pkg_dir" ]; then
    return
  fi
  soname="$(find "$pkg_dir" -maxdepth 1 -type f -name 'libzlink.so.[0-9]*' | sort -V | tail -n 1 || true)"
  if [ -n "$soname" ] && [ -f "$soname" ]; then
    soname_base="$(basename "$soname")"
    version_part="${soname_base#libzlink.so.}"
    major="${version_part%%.*}"
    mkdir -p "$dst_dir"
    rm -f "$dst_dir/libzlink.so" "$dst_dir/libzlink.so.$major"
    find "$dst_dir" -maxdepth 1 -type f -name 'libzlink.so.*' -delete
    find "$dst_dir" -maxdepth 1 -type l -name 'libzlink.so.*' -delete
    install -D -m 0755 "$soname" "$dst_dir/$soname_base"
    if [ "$soname_base" != "libzlink.so.$major" ]; then
      ln -s "$soname_base" "$dst_dir/libzlink.so.$major"
    fi
    ln -s "libzlink.so.$major" "$dst_dir/libzlink.so"
  fi
}

cd extracted

# Java
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/java/src/main/resources/native/linux-x86_64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/java/src/main/resources/native/linux-x86_64"
copy libzlink-linux-arm64/libzlink.so "$repo_root/bindings/java/src/main/resources/native/linux-aarch64/libzlink.so"
copy_linux_soname libzlink-linux-arm64 "$repo_root/bindings/java/src/main/resources/native/linux-aarch64"
copy libzlink-macos-x64/libzlink.dylib "$repo_root/bindings/java/src/main/resources/native/darwin-x86_64/libzlink.dylib"
copy libzlink-macos-arm64/libzlink.dylib "$repo_root/bindings/java/src/main/resources/native/darwin-aarch64/libzlink.dylib"
copy_windows_dll libzlink-windows-x64 "$repo_root/bindings/java/src/main/resources/native/windows-x86_64/zlink.dll"
copy_windows_dll libzlink-windows-arm64 "$repo_root/bindings/java/src/main/resources/native/windows-aarch64/zlink.dll"
copy_windows_bin_dlls libzlink-windows-x64 "$repo_root/bindings/java/src/main/resources/native/windows-x86_64"
copy_windows_bin_dlls libzlink-windows-arm64 "$repo_root/bindings/java/src/main/resources/native/windows-aarch64"
copy libzlink_c-linux-x64/libzlink_c.so "$repo_root/bindings/java/src/main/resources/native/linux-x86_64/libzlink_c.so"
copy libzlink_c-macos-x64/libzlink_c.dylib "$repo_root/bindings/java/src/main/resources/native/darwin-x86_64/libzlink_c.dylib"
copy_windows_c_dll libzlink_c-windows-x64 "$repo_root/bindings/java/src/main/resources/native/windows-x86_64/zlink_c.dll"

# Python
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/python/src/zlink/native/linux-x86_64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/python/src/zlink/native/linux-x86_64"
# The Python Core 11 package currently publishes Linux x86_64 only. Other
# binding release jobs keep their own platform payload policy below.

# .NET
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/dotnet/native/linux-x64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/dotnet/native/linux-x64"
copy libzlink-linux-arm64/libzlink.so "$repo_root/bindings/dotnet/native/linux-arm64/libzlink.so"
copy_linux_soname libzlink-linux-arm64 "$repo_root/bindings/dotnet/native/linux-arm64"
copy libzlink-macos-x64/libzlink.dylib "$repo_root/bindings/dotnet/native/osx-x64/libzlink.dylib"
copy libzlink-macos-arm64/libzlink.dylib "$repo_root/bindings/dotnet/native/osx-arm64/libzlink.dylib"
copy_windows_dll libzlink-windows-x64 "$repo_root/bindings/dotnet/native/win-x64/zlink.dll"
copy_windows_dll libzlink-windows-arm64 "$repo_root/bindings/dotnet/native/win-arm64/zlink.dll"
copy_windows_bin_dlls libzlink-windows-x64 "$repo_root/bindings/dotnet/native/win-x64"
copy_windows_bin_dlls libzlink-windows-arm64 "$repo_root/bindings/dotnet/native/win-arm64"

# Node (native libs only; zlink.node comes from node prebuilds job)
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/node/prebuilds/linux-x64/libzlink.so"
copy libzlink-linux-arm64/libzlink.so "$repo_root/bindings/node/prebuilds/linux-arm64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/node/prebuilds/linux-x64"
copy_linux_soname libzlink-linux-arm64 "$repo_root/bindings/node/prebuilds/linux-arm64"
copy libzlink-macos-x64/libzlink.dylib "$repo_root/bindings/node/prebuilds/darwin-x64/libzlink.dylib"
copy libzlink-macos-arm64/libzlink.dylib "$repo_root/bindings/node/prebuilds/darwin-arm64/libzlink.dylib"
copy_windows_dll libzlink-windows-x64 "$repo_root/bindings/node/prebuilds/win32-x64/zlink.dll"
copy_windows_dll libzlink-windows-arm64 "$repo_root/bindings/node/prebuilds/win32-arm64/zlink.dll"
copy_windows_bin_dlls libzlink-windows-x64 "$repo_root/bindings/node/prebuilds/win32-x64"
copy_windows_bin_dlls libzlink-windows-arm64 "$repo_root/bindings/node/prebuilds/win32-arm64"
copy libzlink_c-linux-x64/libzlink_c.so "$repo_root/bindings/node/prebuilds/linux-x64/libzlink_c.so"
copy libzlink_c-macos-x64/libzlink_c.dylib "$repo_root/bindings/node/prebuilds/darwin-x64/libzlink_c.dylib"
copy_windows_c_dll libzlink_c-windows-x64 "$repo_root/bindings/node/prebuilds/win32-x64/zlink_c.dll"

copy zlink-node-linux-x64/zlink.node "$repo_root/bindings/node/prebuilds/linux-x64/zlink.node"
copy zlink-node-linux-arm64/zlink.node "$repo_root/bindings/node/prebuilds/linux-arm64/zlink.node"
copy zlink-node-darwin-x64/zlink.node "$repo_root/bindings/node/prebuilds/darwin-x64/zlink.node"
copy zlink-node-darwin-arm64/zlink.node "$repo_root/bindings/node/prebuilds/darwin-arm64/zlink.node"
copy zlink-node-win32-x64/zlink.node "$repo_root/bindings/node/prebuilds/win32-x64/zlink.node"
copy zlink-node-win32-arm64/zlink.node "$repo_root/bindings/node/prebuilds/win32-arm64/zlink.node"

# C++ bindings
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/cpp/native/linux-x86_64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/cpp/native/linux-x86_64"
copy libzlink-linux-arm64/libzlink.so "$repo_root/bindings/cpp/native/linux-aarch64/libzlink.so"
copy_linux_soname libzlink-linux-arm64 "$repo_root/bindings/cpp/native/linux-aarch64"
copy libzlink-macos-x64/libzlink.dylib "$repo_root/bindings/cpp/native/darwin-x86_64/libzlink.dylib"
copy libzlink-macos-arm64/libzlink.dylib "$repo_root/bindings/cpp/native/darwin-aarch64/libzlink.dylib"
copy_windows_dll libzlink-windows-x64 "$repo_root/bindings/cpp/native/windows-x86_64/zlink.dll"
copy_windows_dll libzlink-windows-arm64 "$repo_root/bindings/cpp/native/windows-aarch64/zlink.dll"
copy_windows_bin_dlls libzlink-windows-x64 "$repo_root/bindings/cpp/native/windows-x86_64"
copy_windows_bin_dlls libzlink-windows-arm64 "$repo_root/bindings/cpp/native/windows-aarch64"
copy libzlink_c-linux-x64/libzlink_c.so "$repo_root/bindings/cpp/native/linux-x86_64/libzlink_c.so"
copy libzlink_c-macos-x64/libzlink_c.dylib "$repo_root/bindings/cpp/native/darwin-x86_64/libzlink_c.dylib"
copy_windows_c_dll libzlink_c-windows-x64 "$repo_root/bindings/cpp/native/windows-x86_64/zlink_c.dll"
copy libzlink-linux-x64/include/zlink.h "$repo_root/bindings/cpp/include/zlink.h"
copy libzlink-linux-x64/include/zlink_enum.h "$repo_root/bindings/cpp/include/zlink_enum.h"
copy libzlink-linux-x64/include/zlink_errno.h "$repo_root/bindings/cpp/include/zlink_errno.h"

# Go bindings
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/go/native/linux-x86_64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/go/native/linux-x86_64"
copy libzlink-linux-arm64/libzlink.so "$repo_root/bindings/go/native/linux-aarch64/libzlink.so"
copy_linux_soname libzlink-linux-arm64 "$repo_root/bindings/go/native/linux-aarch64"
copy libzlink-macos-x64/libzlink.dylib "$repo_root/bindings/go/native/darwin-x86_64/libzlink.dylib"
copy libzlink-macos-arm64/libzlink.dylib "$repo_root/bindings/go/native/darwin-aarch64/libzlink.dylib"
copy_windows_dll libzlink-windows-x64 "$repo_root/bindings/go/native/windows-x86_64/zlink.dll"
copy_windows_dll libzlink-windows-arm64 "$repo_root/bindings/go/native/windows-aarch64/zlink.dll"
copy_windows_bin_dlls libzlink-windows-x64 "$repo_root/bindings/go/native/windows-x86_64"
copy_windows_bin_dlls libzlink-windows-arm64 "$repo_root/bindings/go/native/windows-aarch64"
copy libzlink_c-linux-x64/libzlink_c.so "$repo_root/bindings/go/native/linux-x86_64/libzlink_c.so"
copy libzlink_c-macos-x64/libzlink_c.dylib "$repo_root/bindings/go/native/darwin-x86_64/libzlink_c.dylib"
copy_windows_c_dll libzlink_c-windows-x64 "$repo_root/bindings/go/native/windows-x86_64/zlink_c.dll"
copy libzlink-linux-x64/include/zlink.h "$repo_root/bindings/go/include/zlink.h"
copy libzlink-linux-x64/include/zlink_enum.h "$repo_root/bindings/go/include/zlink_enum.h"
copy libzlink-linux-x64/include/zlink_errno.h "$repo_root/bindings/go/include/zlink_errno.h"

# Rust bindings
copy libzlink-linux-x64/libzlink.so "$repo_root/bindings/rust/native/linux-x86_64/libzlink.so"
copy_linux_soname libzlink-linux-x64 "$repo_root/bindings/rust/native/linux-x86_64"
copy libzlink-linux-arm64/libzlink.so "$repo_root/bindings/rust/native/linux-aarch64/libzlink.so"
copy_linux_soname libzlink-linux-arm64 "$repo_root/bindings/rust/native/linux-aarch64"
copy libzlink-macos-x64/libzlink.dylib "$repo_root/bindings/rust/native/darwin-x86_64/libzlink.dylib"
copy libzlink-macos-arm64/libzlink.dylib "$repo_root/bindings/rust/native/darwin-aarch64/libzlink.dylib"
copy_windows_dll libzlink-windows-x64 "$repo_root/bindings/rust/native/windows-x86_64/zlink.dll"
copy_windows_dll libzlink-windows-arm64 "$repo_root/bindings/rust/native/windows-aarch64/zlink.dll"
copy_windows_bin_dlls libzlink-windows-x64 "$repo_root/bindings/rust/native/windows-x86_64"
copy_windows_bin_dlls libzlink-windows-arm64 "$repo_root/bindings/rust/native/windows-aarch64"
copy libzlink-linux-x64/include/zlink.h "$repo_root/bindings/rust/include/zlink.h"
copy libzlink-linux-x64/include/zlink_enum.h "$repo_root/bindings/rust/include/zlink_enum.h"
copy libzlink-linux-x64/include/zlink_errno.h "$repo_root/bindings/rust/include/zlink_errno.h"

IFS='.' read -r release_major release_minor release_patch <<< "${runtime_version}"

align_version_macros() {
  header="$1"
  if [ ! -f "$header" ]; then
    return
  fi
  sed -i -E \
    -e "s/^#define ZLINK_VERSION_MAJOR [0-9]+$/#define ZLINK_VERSION_MAJOR $release_major/" \
    -e "s/^#define ZLINK_VERSION_MINOR [0-9]+$/#define ZLINK_VERSION_MINOR $release_minor/" \
    -e "s/^#define ZLINK_VERSION_PATCH [0-9]+$/#define ZLINK_VERSION_PATCH $release_patch/" \
    "$header"
}

align_version_macros "$repo_root/bindings/c/include/zlink.h"
for header in \
  "$repo_root/bindings/c/include/zlink/common.h" \
  "$repo_root/bindings/cpp/include/zlink/common.h" \
  "$repo_root/bindings/go/include/zlink/common.h" \
  "$repo_root/bindings/rust/include/zlink/common.h"; do
  align_version_macros "$header"
done

popd >/dev/null

echo "Done: fetched $tag and updated bindings binaries"
