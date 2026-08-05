#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
prefix=""

usage() {
  cat <<'EOF'
Usage: verify-package.sh --prefix ABSOLUTE_DIR

Checks the installed Core package version, public headers, exact runtime,
runtime ABI SONAME, and a clean C consumer. The current release is 0.9.0 and
the SONAME is libzlink.so.0.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) prefix="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$prefix" = /* ]] || { echo "--prefix must be absolute" >&2; exit 2; }
prefix="$(readlink -f "$prefix")"
manifest="$prefix/share/zlink/core-package-provenance.json"
[[ -f "$manifest" ]] || { echo "Core provenance is missing: $manifest" >&2; exit 1; }

readarray -t metadata < <(node - "$prefix" "$manifest" <<'NODE'
const fs = require('node:fs');
const path = require('node:path');
const [prefix, manifestPath] = process.argv.slice(2);
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const fail = message => { console.error(message); process.exit(1); };
if (manifest.schema !== 1 || manifest.package !== 'zlink-core') {
  fail('invalid Core package provenance identity');
}
if (!/^\d+\.\d+\.\d+$/.test(manifest.version ?? '')) {
  fail('Core package version must be MAJOR.MINOR.PATCH');
}
if (Number(manifest.abiMajor) !== 0) fail('Core ABI major must be 0');
const runtime = path.join(prefix, 'lib', `libzlink.so.${manifest.version}`);
if (!fs.existsSync(runtime)) fail(`exact Core runtime is missing: ${runtime}`);
const runtimeRecord = (manifest.files ?? []).find(
  record => record.path === `lib/libzlink.so.${manifest.version}`
);
if (!runtimeRecord || !/^[0-9a-f]{64}$/.test(runtimeRecord.sha256 ?? '')) {
  fail('exact Core runtime is absent from provenance');
}
const actualSha = require('node:crypto').createHash('sha256')
  .update(fs.readFileSync(runtime)).digest('hex');
if (actualSha !== runtimeRecord.sha256) fail('Core runtime SHA-256 mismatch');
if (fs.realpathSync(path.join(prefix, 'lib', 'libzlink.so.0'))
    !== fs.realpathSync(runtime)) {
  fail('libzlink.so.0 does not resolve to the exact Core runtime');
}
if (fs.realpathSync(path.join(prefix, 'lib', 'libzlink.so'))
    !== fs.realpathSync(runtime)) {
  fail('libzlink.so does not resolve to the exact Core runtime');
}
if (!fs.existsSync(path.join(prefix, 'include', 'zlink.h'))) {
  fail('Core public header is missing');
}
console.log(manifest.version);
console.log(runtime);
console.log(runtimeRecord.sha256);
NODE
)

core_version="${metadata[0]}"
runtime="${metadata[1]}"
runtime_sha="${metadata[2]}"
[[ "$core_version" = "$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")" ]] || {
  echo "Installed Core version does not match repository VERSION" >&2
  exit 1
}
[[ "$(readelf -d "$runtime" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')" = "libzlink.so.0" ]] || {
  echo "Core runtime SONAME is not libzlink.so.0" >&2
  exit 1
}

consumer_dir="$(mktemp -d)"
cleanup() { rm -rf "$consumer_dir"; }
trap cleanup EXIT
cat >"$consumer_dir/main.c" <<'EOF'
#include <stdio.h>
#include <zlink.h>

int main(void) {
  int major = 0;
  int minor = 0;
  int patch = 0;
  zlink_version(&major, &minor, &patch);
  printf("%d.%d.%d\n", major, minor, patch);
  return 0;
}
EOF
cc -std=c11 -I"$prefix/include" "$consumer_dir/main.c" \
  -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lzlink \
  -o "$consumer_dir/consumer"
[[ "$("$consumer_dir/consumer")" = "$core_version" ]] || {
  echo "Core clean consumer reported an unexpected version" >&2
  exit 1
}
[[ "$(sha256sum "$runtime" | awk '{print $1}')" = "$runtime_sha" ]] || {
  echo "Core runtime changed after provenance validation" >&2
  exit 1
}

echo "Core package verified: version=$core_version abi=0 prefix=$prefix"
