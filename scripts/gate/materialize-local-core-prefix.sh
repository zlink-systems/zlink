#!/usr/bin/env bash
# Recreate the Core package prefix cache that scripts/local-package/core/fetch-release.sh
# short-circuits on, using a locally built Core tree. Needed because no GitHub release
# exists for the in-development VERSION (latest release tag is core/v0.15.1) and the
# WSL reinstall wiped the previously materialized cache.
set -euo pipefail

repo_root="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel 2>/dev/null || echo /home/hep7/project/zlink)"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/core/build-dev}"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
platform="linux-x64"
cache_root="${ZLINK_CORE_CACHE_DIR:-$HOME/.cache/zlink/core}"
prefix="$cache_root/$version/$platform"

runtime_src="$build_dir/lib/libzlink.so.$version"
[[ -f "$runtime_src" ]] || { echo "built runtime missing: $runtime_src" >&2; exit 1; }

stage="$(mktemp -d "$cache_root/.stage.XXXXXX" 2>/dev/null || { mkdir -p "$cache_root"; mktemp -d "$cache_root/.stage.XXXXXX"; })"
trap 'rm -rf "$stage"' EXIT

mkdir -p "$stage/include" "$stage/lib" "$stage/share/zlink"
cp -a "$repo_root/core/include/." "$stage/include/"
install -m 0755 "$runtime_src" "$stage/lib/libzlink.so.$version"
ln -s "libzlink.so.$version" "$stage/lib/libzlink.so.0"
ln -s "libzlink.so.0" "$stage/lib/libzlink.so"

source_rev="$(git -C "$repo_root" rev-parse HEAD)"
dirty=false
[[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=no)" ]] && dirty=true

PREFIX="$stage" VERSION="$version" PLATFORM="$platform" \
  SOURCE_SHA="$source_rev" DIRTY="$dirty" TAG="local/build-dev" \
  RUNTIME_PATH="lib/libzlink.so.$version" RUNTIME_SONAME="libzlink.so.0" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const root = process.env.PREFIX;
const version = process.env.VERSION;
const runtimePath = process.env.RUNTIME_PATH;
const files = [];

const cmakeDir = path.join(root, 'lib', 'cmake', 'zlink');
fs.mkdirSync(cmakeDir, {recursive: true});
fs.writeFileSync(path.join(cmakeDir, 'zlinkConfig.cmake'), `\
# Materialized locally (no release asset for this in-development version).
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
  source: {revision: process.env.SOURCE_SHA, dirty: process.env.DIRTY === 'true'},
  release: {tag: process.env.TAG, checksumsSha256: null},
  files,
};
fs.mkdirSync(path.join(root, 'share', 'zlink'), {recursive: true});
fs.writeFileSync(
  path.join(root, 'share', 'zlink', 'core-package-provenance.json'),
  `${JSON.stringify(manifest, null, 2)}\n`,
);
NODE

bash "$repo_root/scripts/local-package/core/verify-package.sh" --prefix "$stage" >/dev/null

rm -rf "$prefix"
mkdir -p "$(dirname "$prefix")"
mv "$stage" "$prefix"
trap - EXIT
echo "Core prefix materialized: $prefix (from $build_dir, rev ${source_rev:0:10}, dirty=$dirty)"
