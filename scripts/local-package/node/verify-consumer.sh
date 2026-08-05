#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
self_test=false
dry_run=false
evidence=""

usage() {
  echo "Usage: verify-consumer.sh --self-test --dry-run --evidence ABSOLUTE_JSON"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --self-test) self_test=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

$self_test || { echo "--self-test is required for BIND-PKG-TEST" >&2; exit 2; }
$dry_run || { echo "--dry-run is required for BIND-PKG-TEST" >&2; exit 2; }
[[ "$evidence" = /* ]] || { echo "--evidence must be an absolute path" >&2; exit 2; }

binding_dir="$repo_root/bindings/node"
fixture_source="$script_dir/fixtures/public-consumer"
package_json="$binding_dir/package.json"
build_script="$script_dir/build-wsl.sh"
resolver="$binding_dir/scripts/resolve_core.js"

checks=()
check() {
  local name="$1"
  shift
  if "$@"; then checks+=("$name=pass"); else echo "check failed: $name" >&2; exit 1; fi
}
has_literal() { grep -Fq -- "$1" "$2"; }
has_no_match() { ! grep -Eq -- "$1" "$2"; }

#  The expected version is derived from the binding's own package metadata, so a
#  Core or binding bump does not require editing this script.
binding_version="$(node -e 'process.stdout.write(require(process.argv[1]).version)' "$package_json")"
[[ "$binding_version" =~ ^11\.[0-9]+\.[0-9]+$ ]] || {
  echo "binding package version must be 11.x.y, found: $binding_version" >&2; exit 1; }

check package-version has_literal "\"version\": \"$binding_version\"" "$package_json"
check cjs-export has_literal '"require": "./dist/index.js"' "$package_json"
check esm-export has_literal '"import": "./dist/index.mjs"' "$package_json"
check declaration-export has_literal '"types": "./dist/index.d.ts"' "$package_json"
check installed-core-input has_literal 'ZLINK_CORE_INSTALL_PREFIX' "$build_script"
check approved-core-provenance-input has_literal 'ZLINK_APPROVED_CORE_PROVENANCE_SHA256' "$build_script"
check approved-core-provenance-compare has_literal 'actual_core_provenance_sha256' "$build_script"
check core-provenance-resolver has_literal 'core-package-provenance.json' "$resolver"
check core-major-gate has_literal '/^11\.\d+\.\d+$/' "$resolver"
check no-repository-core-fallback has_no_match 'core/(build|include)|ZLINK_LIB_PATH' "$binding_dir/binding.gyp"
check no-pre-core-payload test ! -d "$binding_dir/prebuilds"
check fixture-package-dependency has_literal "\"@zlink-systems/zlink\": \"$binding_version\"" "$fixture_source/package.json"
check public-only-fixture has_no_match 'runtime/native|nativeHandle|node-gyp|bindings/node|addon' "$fixture_source/consumer.mts"

(cd "$binding_dir" && npm run build >/dev/null)

fixture="$(mktemp -d)"
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/node_modules/@zlink-systems/zlink"
cp -R "$binding_dir/dist" "$fixture/node_modules/@zlink-systems/zlink/dist"
cp "$binding_dir/package.json" "$fixture/node_modules/@zlink-systems/zlink/package.json"
mkdir -p "$fixture/node_modules/@types"
cp -R "$binding_dir/node_modules/@types/node" "$fixture/node_modules/@types/node"
cp -R "$binding_dir/node_modules/undici-types" "$fixture/node_modules/undici-types"
cp "$fixture_source/consumer.mts" "$fixture/consumer.mts"
cp "$fixture_source/consumer.cts" "$fixture/consumer.cts"
cp "$fixture_source/tsconfig.json" "$fixture/tsconfig.json"
"$binding_dir/node_modules/.bin/tsc" -p "$fixture/tsconfig.json"

node -e "const p=require('$package_json'); if(!p.exports['.'].import || !p.exports['.'].require || !p.exports['.'].types) process.exit(1)"
node -e "const fs=require('fs'); const s=fs.readFileSync('$binding_dir/dist/index.mjs','utf8'); for(const n of ['createContext','createPairSocket','createStreamSocket','createPoller']) if(!s.includes(n)) process.exit(1)"

if "$script_dir/verify-consumer.sh" --self-test --dry-run --evidence relative.json >/dev/null 2>&1; then
  echo "Verifier accepted a relative evidence path" >&2
  exit 1
fi

mkdir -p "$(dirname "$evidence")"
CHECKS="$(printf '%s\n' "${checks[@]}")" EVIDENCE="$evidence" \
REPOSITORY_REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const fs = require('node:fs');
const checks = Object.fromEntries(process.env.CHECKS.trim().split('\n').map(line => {
  const split = line.indexOf('=');
  return [line.slice(0, split), line.slice(split + 1)];
}));
const result = {
  schema: 'zlink-v11-binding-package-consumer-self-test-v1',
  ledgerId: 'V11-M4-BIND-NODE',
  command: 'BIND-PKG-TEST',
  language: 'node',
  status: 'passed',
  mode: 'self-test-dry-run',
  completedAt: new Date().toISOString(),
  repositoryRevision: process.env.REPOSITORY_REVISION,
  packagePublished: false,
  checks: {
    ...checks,
    cleanEsmConsumerCompile: 'pass',
    cleanCjsConsumerCompile: 'pass',
    dualPublicExportProjection: 'pass',
    relativeEvidencePathRejected: 'pass'
  }
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE

echo "node binding package consumer self-test passed"
echo "Evidence: $evidence"
