#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
build_dir="${ZLINK_CORE_BUILD_DIR:-$repo_root/core/build}"
output_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"
evidence=""

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--build-dir DIR] [--output-root ABSOLUTE_DIR]
                    [--evidence ABSOLUTE_JSON]

Builds Core from the current source, installs a versioned 0.10.1 local package,
writes a generic provenance record, and verifies a clean C consumer. The ABI
SONAME is libzlink.so.0.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) build_dir="${2:-}"; shift 2 ;;
    --output-root) output_root="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$output_root" = /* ]] || { echo "--output-root must be absolute" >&2; exit 2; }
if [[ "$build_dir" != /* ]]; then build_dir="$repo_root/$build_dir"; fi
build_dir="$(realpath -m "$build_dir")"
output_root="$(realpath -m "$output_root")"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "VERSION must be MAJOR.MINOR.PATCH, found: ${version:-<missing>}" >&2
  exit 1
}
[[ "$output_root" != / && "$output_root" != "$repo_root" ]] || {
  echo "Unsafe local package output root: $output_root" >&2
  exit 2
}

if [[ -z "$evidence" ]]; then
  evidence="$output_root/core-package-$version.json"
fi
[[ "$evidence" = /* ]] || { echo "--evidence must be absolute" >&2; exit 2; }

cmake -S "$repo_root/core" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$configuration" \
  -DBUILD_TESTS=OFF -DZLINK_BUILD_TESTS=OFF \
  -DBUILD_BENCHMARKS=OFF -DWITH_DOCS=OFF
cmake --build "$build_dir" --parallel "${ZLINK_CORE_PACKAGE_BUILD_JOBS:-4}"

prefix="$output_root/install/zlink-core/$version"
case "$prefix" in "$output_root"/*) ;; *) echo "Install prefix escaped output root" >&2; exit 2 ;; esac
rm -rf "$prefix"
mkdir -p "$prefix" "$(dirname "$evidence")"
cmake --install "$build_dir" --prefix "$prefix"

manifest="$prefix/share/zlink/core-package-provenance.json"
mkdir -p "$(dirname "$manifest")"
PREFIX="$prefix" MANIFEST="$manifest" VERSION="$version" \
REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const root = process.env.PREFIX;
const manifestPath = process.env.MANIFEST;
const version = process.env.VERSION;
const files = [];
function walk(directory) {
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    const full = path.join(directory, entry.name);
    if (full === manifestPath) continue;
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
const runtimePath = path.join(root, 'lib', `libzlink.so.${version}`);
const runtimeSha = crypto.createHash('sha256').update(fs.readFileSync(runtimePath)).digest('hex');
const manifest = {
  schema: 1,
  package: 'zlink-core',
  version,
  abiMajor: 0,
  runtime: {path: `lib/libzlink.so.${version}`, sha256: runtimeSha, soname: 'libzlink.so.0'},
  source: {revision: process.env.REVISION, dirty: true},
  files,
};
fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
NODE

bash "$script_dir/verify-package.sh" --prefix "$prefix"
manifest_sha="$(sha256sum "$manifest" | awk '{print $1}')"
runtime_sha="$(sha256sum "$prefix/lib/libzlink.so.$version" | awk '{print $1}')"
cat >"$evidence" <<EOF
{
  "schema": 1,
  "package": "zlink-core",
  "version": "$version",
  "abiMajor": 0,
  "status": "pass",
  "prefix": "$prefix",
  "provenanceManifest": "$manifest",
  "provenanceSha256": "$manifest_sha",
  "runtime": {
    "path": "$prefix/lib/libzlink.so.$version",
    "sha256": "$runtime_sha",
    "soname": "libzlink.so.0"
  }
}
EOF

echo "Core local package: $prefix"
echo "Core provenance: $manifest"
echo "Core evidence: $evidence"
