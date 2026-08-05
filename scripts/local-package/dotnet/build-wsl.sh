#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"
provenance_sha="${ZLINK_CORE_PROVENANCE_SHA256:-}"
candidate_sha="${ZLINK_CORE_CANDIDATE_MANIFEST_SHA256:-}"
evidence=""

usage() {
  cat <<'EOF'
Usage: build-wsl.sh --core-prefix ABSOLUTE_DIR \
  --core-provenance-sha256 SHA256 \
  --core-candidate-manifest-sha256 SHA256 \
  --evidence ABSOLUTE_JSON [DOTNET_PACK_ARGS...]
EOF
}

pack_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --core-provenance-sha256) provenance_sha="${2:-}"; shift 2 ;;
    --core-candidate-manifest-sha256) candidate_sha="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) pack_args+=("$1"); shift ;;
  esac
done

[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
[[ "$evidence" = /* ]] || { echo "--evidence must be absolute" >&2; exit 2; }
[[ "$provenance_sha" =~ ^[0-9a-f]{64}$ ]] || { echo "invalid Core provenance SHA-256" >&2; exit 2; }
[[ "$candidate_sha" =~ ^[0-9a-f]{64}$ ]] || { echo "invalid Core candidate manifest SHA-256" >&2; exit 2; }

core_prefix="$(readlink -f "$core_prefix")"
manifest="$core_prefix/share/zlink/core-package-provenance.json"
[[ -f "$manifest" ]] || { echo "Core package provenance is missing: $manifest" >&2; exit 1; }
[[ "$(sha256sum "$manifest" | awk '{print $1}')" = "$provenance_sha" ]] || {
  echo "Core package provenance SHA-256 does not match the approved input" >&2; exit 1;
}

readarray -t metadata < <(node - "$manifest" "$candidate_sha" <<'NODE'
const fs = require('node:fs');
const [path, expectedCandidate] = process.argv.slice(2);
const m = JSON.parse(fs.readFileSync(path, 'utf8'));
const fail = message => { console.error(message); process.exit(1); };
if (m.schema !== 1 || m.package !== 'zlink-core') fail('invalid Core package provenance identity');
if (!/^11\.[0-9]+\.[0-9]+$/.test(m.version ?? '')) fail('Core package version must be 11.x.y');
if (m.candidate?.ledgerId !== 'V11-M3-CORE-VERIFY') fail('unexpected Core candidate ledger identity');
if (m.candidate?.manifestSha256 !== expectedCandidate) fail('Core candidate manifest SHA-256 mismatch');
const runtime = `lib/libzlink.so.${m.version}`;
const entry = m.files?.find(file => file.path === runtime);
if (!entry || !/^[0-9a-f]{64}$/.test(entry.sha256 ?? '')) fail('exact Core runtime is absent from provenance');
console.log(m.version);
console.log(entry.sha256);
NODE
)
core_version="${metadata[0]}"
runtime_sha="${metadata[1]}"
native_root="$core_prefix/lib"
exact_runtime="$native_root/libzlink.so.$core_version"
[[ -f "$exact_runtime" ]] || { echo "exact Core runtime is missing: $exact_runtime" >&2; exit 1; }
[[ "$(sha256sum "$exact_runtime" | awk '{print $1}')" = "$runtime_sha" ]] || {
  echo "Core runtime SHA-256 does not match provenance" >&2; exit 1;
}
[[ "$(readelf -d "$exact_runtime" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')" = "libzlink.so.11" ]] || {
  echo "Core runtime SONAME is not libzlink.so.11" >&2; exit 1;
}
for link in libzlink.so libzlink.so.11; do
  [[ "$(readlink -f "$native_root/$link")" = "$(readlink -f "$exact_runtime")" ]] || {
    echo "$link does not resolve to the approved exact runtime" >&2; exit 1;
  }
done

package_version="$(sed -n 's:.*<Version>\([^<]*\)</Version>.*:\1:p' "$repo_root/bindings/dotnet/src/Zlink/Zlink.csproj" | head -1)"
node - "$package_version" "$core_version" <<'NODE'
const [pkg, core] = process.argv.slice(2);
const parse = value => {
  const match = /^(\d+)\.(\d+)\.(\d+)$/.exec(value);
  if (!match) throw new Error(`invalid numeric version: ${value}`);
  return match.slice(1).map(Number);
};
const [packageMajor, packageMinor, packagePatch] = parse(pkg);
const [coreMajor, coreMinor, corePatch] = parse(core);
if (packageMajor !== coreMajor || packageMinor !== coreMinor || packagePatch < corePatch) {
  console.error(`.NET package ${pkg} must use Core ${core} major.minor and an equal or newer patch`);
  process.exit(1);
}
NODE

out_dir="$artifact_root/nuget"
mkdir -p "$out_dir"
dotnet pack "$repo_root/bindings/dotnet/src/Zlink/Zlink.csproj" \
  -c "$configuration" -o "$out_dir" \
  -p:ZLinkLinuxX64NativeRoot="$native_root" \
  -p:ZLinkCoreVersion="$core_version" \
  -p:ZLinkCoreProvenancePath="$manifest" \
  "${pack_args[@]}"

package="$out_dir/Systems.Zlink.$package_version.nupkg"
"$script_dir/verify-consumer.sh" --package "$package" --core-prefix "$core_prefix" \
  --core-provenance-sha256 "$provenance_sha" \
  --core-candidate-manifest-sha256 "$candidate_sha" --evidence "$evidence"

echo "-- .NET local NuGet package: $package"
echo "-- Evidence: $evidence"
