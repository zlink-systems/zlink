#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
self_test=false
dry_run=false
package=""
core_prefix=""
provenance_sha=""
candidate_sha=""
evidence=""

usage() {
  cat <<'EOF'
Usage:
  verify-consumer.sh --self-test --dry-run --evidence ABSOLUTE_JSON
  verify-consumer.sh --package ABSOLUTE_NUPKG --core-prefix ABSOLUTE_DIR \
    --core-provenance-sha256 SHA256 \
    --core-candidate-manifest-sha256 SHA256 --evidence ABSOLUTE_JSON
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --self-test) self_test=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    --package) package="${2:-}"; shift 2 ;;
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --core-provenance-sha256) provenance_sha="${2:-}"; shift 2 ;;
    --core-candidate-manifest-sha256) candidate_sha="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$evidence" = /* ]] || { echo "--evidence must be absolute" >&2; exit 2; }
project="$repo_root/bindings/dotnet/src/Zlink/Zlink.csproj"
fixture_project="$script_dir/fixtures/public-consumer/PublicConsumer.csproj"
fixture_source="$script_dir/fixtures/public-consumer/Program.cs"

if $self_test; then
  $dry_run || { echo "--dry-run is required with --self-test" >&2; exit 2; }
  grep -Fq '<PackageId>Systems.Zlink</PackageId>' "$project"
  grep -Fq '<Version>11.1.2</Version>' "$project"
  ! grep -Eq '<ProjectReference|bindings/dotnet/src' "$fixture_project"
  ! grep -Eq 'Runtime\.Native|DllImport|System\.Reflection|BindingFlags|NativeMethods' "$fixture_source"
  grep -Fq 'Zlink.Version()' "$fixture_source"
  grep -Fq 'core-package-provenance.json' "$repo_root/scripts/local-package/dotnet/build-wsl.sh"
  grep -Fq 'libzlink.so.11' "$repo_root/scripts/local-package/dotnet/build-wsl.sh"
  ! grep -Eq 'native\\(win|linux|osx)-\*' "$project"
  ! grep -Fq 'TryLoadWellKnownNames' "$repo_root/bindings/dotnet/src/Zlink/Runtime/Native/NativeLibraryLoader.cs"
  mkdir -p "$(dirname "$evidence")"
  EVIDENCE="$evidence" REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const fs = require('node:fs');
fs.writeFileSync(process.env.EVIDENCE, JSON.stringify({
  schema: 1, ledgerId: 'V11-M4-BIND-DN', command: 'BIND-PKG-TEST',
  status: 'pass', mode: 'self-test-dry-run', completedAt: new Date().toISOString(),
  repositoryRevision: process.env.REVISION, packagePublished: false,
  checks: { exactCoreInputRequired: true, legacyNativeCopyDisabled: true,
    ambientRuntimeFallbackDisabled: true, publicConsumerFixture: true,
    actualModeAvailable: true }
}, null, 2) + '\n');
NODE
  echo "dotnet package consumer tooling self-test passed"
  exit 0
fi

$dry_run && { echo "--dry-run is only valid with --self-test" >&2; exit 2; }
[[ "$package" = /* && -f "$package" ]] || { echo "--package must name an absolute NuGet package" >&2; exit 2; }
[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
[[ "$provenance_sha" =~ ^[0-9a-f]{64}$ ]] || { echo "invalid provenance SHA-256" >&2; exit 2; }
[[ "$candidate_sha" =~ ^[0-9a-f]{64}$ ]] || { echo "invalid candidate SHA-256" >&2; exit 2; }

core_prefix="$(readlink -f "$core_prefix")"
manifest="$core_prefix/share/zlink/core-package-provenance.json"
[[ "$(sha256sum "$manifest" | awk '{print $1}')" = "$provenance_sha" ]] || { echo "provenance SHA-256 mismatch" >&2; exit 1; }
readarray -t expected < <(node - "$manifest" "$candidate_sha" <<'NODE'
const fs = require('node:fs');
const [path, candidate] = process.argv.slice(2);
const m = JSON.parse(fs.readFileSync(path));
if (m.package !== 'zlink-core' || m.candidate?.ledgerId !== 'V11-M3-CORE-VERIFY' ||
    m.candidate?.manifestSha256 !== candidate || !/^11\.[0-9]+\.[0-9]+$/.test(m.version ?? '')) process.exit(1);
const file = m.files.find(x => x.path === `lib/libzlink.so.${m.version}`);
if (!file) process.exit(1);
console.log(m.version); console.log(file.sha256);
NODE
)
version="${expected[0]}"
runtime_sha="${expected[1]}"

package_version="$(unzip -p "$package" '*.nuspec' | sed -n 's:.*<version>\([^<]*\)</version>.*:\1:p' | head -1)"
node - "$package_version" "$version" <<'NODE'
const [pkg, core] = process.argv.slice(2);
const parse = value => {
  const match = /^(\d+)\.(\d+)\.(\d+)$/.exec(value);
  if (!match) process.exit(1);
  return match.slice(1).map(Number);
};
const p = parse(pkg), c = parse(core);
if (p[0] !== c[0] || p[1] !== c[1] || p[2] < c[2]) process.exit(1);
NODE

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/feed" "$work/consumer" "$work/packages" "$work/home"
cp "$package" "$work/feed/"
cp "$fixture_project" "$fixture_source" "$work/consumer/"
sed -i "s/Version=\"[0-9][0-9.]*\"/Version=\"$package_version\"/" \
  "$work/consumer/PublicConsumer.csproj"
#  The fixture pins the Core version it expects. Substitute the version this
#  package actually carries so a Core bump does not require editing the fixture.
core_tuple="$(printf '%s' "$version" | tr '.' ' ')"
sed -i \
  -e "s/(@CORE_VERSION_TUPLE@)/(${core_tuple// /, })/" \
  -e "s/@CORE_VERSION@/$version/g" \
  "$work/consumer/Program.cs"

package_entries="$work/package-entries.txt"
unzip -Z1 "$package" >"$package_entries"
expected_native="runtimes/linux-x64/native/libzlink.so
runtimes/linux-x64/native/libzlink.so.11
runtimes/linux-x64/native/libzlink.so.$version"
actual_native="$(grep '^runtimes/.*/native/' "$package_entries" || true)"
[[ "$actual_native" = "$expected_native" ]] || {
  echo "NuGet native payload is not the exact approved Core 11 runtime set" >&2; exit 1;
}
unzip -p "$package" provenance/core-package-provenance.json >"$work/embedded-provenance.json"
[[ "$(sha256sum "$work/embedded-provenance.json" | awk '{print $1}')" = "$provenance_sha" ]] || {
  echo "NuGet embedded Core provenance does not match the approved input" >&2; exit 1;
}
for native_entry in $actual_native; do
  unzip -p "$package" "$native_entry" >"$work/packed-runtime"
  [[ "$(sha256sum "$work/packed-runtime" | awk '{print $1}')" = "$runtime_sha" ]] || {
    echo "NuGet runtime $native_entry does not match Core provenance" >&2; exit 1;
  }
done

cat >"$work/consumer/NuGet.Config" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear/><add key="isolated" value="$work/feed"/></packageSources></configuration>
EOF

(
  cd "$work/consumer"
  env -u ZLINK_LIBRARY_PATH -u LD_LIBRARY_PATH HOME="$work/home" \
    NUGET_PACKAGES="$work/packages" DOTNET_CLI_HOME="$work/home" \
    dotnet restore --configfile NuGet.Config --no-cache
  env -u ZLINK_LIBRARY_PATH -u LD_LIBRARY_PATH HOME="$work/home" \
    NUGET_PACKAGES="$work/packages" DOTNET_CLI_HOME="$work/home" \
    dotnet build --no-restore -c Release
  env -u ZLINK_LIBRARY_PATH -u LD_LIBRARY_PATH HOME="$work/home" \
    NUGET_PACKAGES="$work/packages" DOTNET_CLI_HOME="$work/home" \
    dotnet run --no-build --no-restore -c Release >"$work/run.json"
)

loaded_path="$(node -p "JSON.parse(require('fs').readFileSync('$work/run.json','utf8')).loadedRuntime")"
loaded_version="$(node -p "JSON.parse(require('fs').readFileSync('$work/run.json','utf8')).version")"
[[ "$loaded_version" = "$version" ]] || { echo "loaded Core version mismatch" >&2; exit 1; }
[[ -f "$loaded_path" ]] || { echo "loaded Core path is not a file: $loaded_path" >&2; exit 1; }
[[ "$(sha256sum "$loaded_path" | awk '{print $1}')" = "$runtime_sha" ]] || { echo "loaded Core SHA-256 mismatch" >&2; exit 1; }
[[ "$(readelf -d "$loaded_path" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')" = "libzlink.so.11" ]] || { echo "loaded Core SONAME mismatch" >&2; exit 1; }

package_sha="$(sha256sum "$package" | awk '{print $1}')"
mkdir -p "$(dirname "$evidence")"
EVIDENCE="$evidence" PACKAGE="$package" PACKAGE_SHA="$package_sha" PROVENANCE_SHA="$provenance_sha" \
CANDIDATE_SHA="$candidate_sha" PACKAGE_VERSION="$package_version" CORE_VERSION="$version" \
RUNTIME_SHA="$runtime_sha" LOADED_PATH="$loaded_path" \
REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const fs = require('node:fs');
fs.writeFileSync(process.env.EVIDENCE, JSON.stringify({
  schema: 1, ledgerId: 'V11-M4-PKG-DN', status: 'pass', mode: 'actual',
  completedAt: new Date().toISOString(), repositoryRevision: process.env.REVISION,
  package: { path: process.env.PACKAGE, sha256: process.env.PACKAGE_SHA,
    version: process.env.PACKAGE_VERSION },
  core: { provenanceSha256: process.env.PROVENANCE_SHA,
    candidateManifestSha256: process.env.CANDIDATE_SHA,
    runtimeSha256: process.env.RUNTIME_SHA, soname: 'libzlink.so.11',
    loadedVersion: process.env.CORE_VERSION, loadedPath: process.env.LOADED_PATH },
  checks: { isolatedRestore: 'pass', publicOnlyBuild: 'pass', run: 'pass',
    exactRuntimeLoaded: 'pass', embeddedProvenance: 'pass',
    exactNativePayloadOnly: 'pass', ambientLibraryEnvironmentRemoved: true }
}, null, 2) + '\n');
NODE

echo "dotnet isolated NuGet consumer passed with Core $version"
echo "Evidence: $evidence"
