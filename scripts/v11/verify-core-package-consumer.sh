#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
prefix=""
evidence=""
candidate_manifest=""
review_evidence=""

usage() {
  cat <<'EOF'
Usage: verify-core-package-consumer.sh --prefix ABSOLUTE_DIR \
  --candidate-manifest ABSOLUTE_JSON --review-evidence ABSOLUTE_JSON \
  --evidence ABSOLUTE_JSON

Verifies that a Core install tree contains no service headers and that a clean
C consumer compiles, links, and loads libzlink only from that install tree.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) prefix="${2:-}"; shift 2 ;;
    --candidate-manifest) candidate_manifest="${2:-}"; shift 2 ;;
    --review-evidence) review_evidence="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$prefix" = /* ]] || { echo "--prefix must be an absolute path" >&2; exit 2; }
[[ "$candidate_manifest" = /* ]] || { echo "--candidate-manifest must be an absolute path" >&2; exit 2; }
[[ "$review_evidence" = /* ]] || { echo "--review-evidence must be an absolute path" >&2; exit 2; }
[[ "$evidence" = /* ]] || { echo "--evidence must be an absolute path" >&2; exit 2; }

reject_dot_segments() {
  local value="$1"
  local label="$2"
  case "/${value#/}/" in
    */./*|*/../*) echo "$label must not contain dot path segments: $value" >&2; exit 2 ;;
  esac
}

reject_dot_segments "$prefix" "--prefix"
reject_dot_segments "$candidate_manifest" "--candidate-manifest"
reject_dot_segments "$review_evidence" "--review-evidence"
reject_dot_segments "$evidence" "--evidence"
repo_root="$(readlink -f "$repo_root")"
prefix="$(readlink -f "$prefix")"
candidate_manifest="$(readlink -f "$candidate_manifest")"
review_evidence="$(readlink -f "$review_evidence")"
evidence="$(realpath -m "$evidence")"
[[ -d "$prefix" ]] || { echo "Core install prefix does not exist: $prefix" >&2; exit 1; }
[[ -f "$candidate_manifest" ]] || { echo "Candidate manifest is missing" >&2; exit 1; }
[[ -f "$review_evidence" ]] || { echo "Review evidence is missing" >&2; exit 1; }
[[ -f "$prefix/include/zlink.h" ]] || { echo "Installed zlink.h is missing" >&2; exit 1; }
case "$evidence" in "$prefix"|"$prefix"/*) echo "--evidence must be outside the install prefix" >&2; exit 2 ;; esac

candidate_summary="$(node "$repo_root/scripts/local-package/core/verify-candidate.mjs" \
  --candidate-manifest "$candidate_manifest" --review-evidence "$review_evidence" \
  --repo-root "$repo_root")"

manifest="$prefix/share/zlink/core-package-provenance.json"
[[ -f "$manifest" ]] || { echo "Core package provenance manifest is missing" >&2; exit 1; }
manifest_summary="$(CANDIDATE_SUMMARY="$candidate_summary" node - "$prefix" "$manifest" <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const prefix = process.argv[2];
const manifestPath = process.argv[3];
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const expectedCandidate = JSON.parse(process.env.CANDIDATE_SUMMARY);
for (const key of ['schema', 'package', 'version', 'candidate', 'createdAt', 'files']) {
  if (!(key in manifest)) throw new Error(`provenance manifest is missing ${key}`);
}
if (manifest.schema !== 1 || manifest.package !== 'zlink-core'
    || !/^11\.[0-9]+\.[0-9]+$/.test(manifest.version)
    || Number.isNaN(Date.parse(manifest.createdAt)) || !Array.isArray(manifest.files)) {
  throw new Error('provenance manifest metadata is invalid');
}
const candidate = manifest.candidate;
if (!candidate || candidate.ledgerId !== expectedCandidate.ledgerId
    || candidate.baseRevision !== expectedCandidate.baseRevision
    || candidate.manifestSha256 !== expectedCandidate.manifestSha256
    || candidate.aggregateSha256 !== expectedCandidate.aggregateSha256
    || candidate.approvalEvidenceSha256 !== expectedCandidate.approval.evidenceSha256) {
  throw new Error('provenance candidate identity does not match the approved candidate');
}
const listed = new Set();
for (const record of manifest.files) {
  if (!record || typeof record.path !== 'string' || !/^[0-9a-f]{64}$/.test(record.sha256)
      || path.isAbsolute(record.path) || record.path.split('/').includes('..') || listed.has(record.path)) {
    throw new Error(`invalid provenance file record: ${JSON.stringify(record)}`);
  }
  listed.add(record.path);
  const file = path.join(prefix, ...record.path.split('/'));
  if (!fs.statSync(file).isFile()) throw new Error(`provenance file is missing: ${record.path}`);
  const digest = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  if (digest !== record.sha256) throw new Error(`provenance hash mismatch: ${record.path}`);
}
function files(directory) {
  return fs.readdirSync(directory, {withFileTypes: true}).flatMap(entry => {
    const full = path.join(directory, entry.name);
    return entry.isDirectory() ? files(full) : [full];
  });
}
const actual = files(prefix)
  .filter(file => path.resolve(file) !== path.resolve(manifestPath))
  .map(file => path.relative(prefix, file).split(path.sep).join('/'));
for (const file of actual) if (!listed.has(file)) throw new Error(`unlisted package file: ${file}`);
if (listed.size !== actual.length) throw new Error('provenance file set is incomplete');
const manifestSha = crypto.createHash('sha256').update(fs.readFileSync(manifestPath)).digest('hex');
process.stdout.write(`${manifestSha} ${actual.length} ${manifest.version} ${candidate.manifestSha256}`);
NODE
)"
read -r manifest_sha256 manifest_file_count manifest_version manifest_candidate_sha256 <<<"$manifest_summary"

mapfile -d '' service_headers < <(
  find "$prefix/include" -type f \( -path '*/zlink/service/*' -o -name '*service*.h' \) -print0
)
if (( ${#service_headers[@]} != 0 )); then
  printf 'Installed service headers are forbidden:\n' >&2
  printf '  %s\n' "${service_headers[@]}" >&2
  exit 1
fi

lib_dir=""
for candidate in "$prefix/lib" "$prefix/lib64"; do
  if [[ -f "$candidate/libzlink.so" ]]; then
    lib_dir="$candidate"
    break
  fi
done
[[ -n "$lib_dir" ]] || { echo "Installed libzlink.so is missing" >&2; exit 1; }

compiler="${CC:-cc}"
command -v "$compiler" >/dev/null 2>&1 || { echo "C compiler is unavailable: $compiler" >&2; exit 1; }
command -v ldd >/dev/null 2>&1 || { echo "ldd is required" >&2; exit 1; }
command -v readelf >/dev/null 2>&1 || { echo "readelf is required" >&2; exit 1; }

consumer_dir="$(mktemp -d)"
trap 'rm -rf "$consumer_dir"' EXIT
cat >"$consumer_dir/main.c" <<'EOF'
#include <zlink.h>
#include <stdio.h>

int main(void)
{
    int major = -1;
    int minor = -1;
    int patch = -1;
    zlink_version(&major, &minor, &patch);
    if (major < 0 || minor < 0 || patch < 0)
        return 1;
    printf("%d.%d.%d\n", major, minor, patch);
    return 0;
}
EOF

env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH -u LD_LIBRARY_PATH \
  "$compiler" -std=c11 -Werror -I"$prefix/include" "$consumer_dir/main.c" \
  -L"$lib_dir" -Wl,-rpath,"$lib_dir" -lzlink -o "$consumer_dir/consumer"

load_map="$(env -u LD_LIBRARY_PATH ldd "$consumer_dir/consumer")"
resolved_runtime="$(printf '%s\n' "$load_map" | sed -n 's/^[[:space:]]*libzlink\.so[^=]*=>[[:space:]]*\([^[:space:]]*\).*/\1/p' | head -n1)"
[[ -n "$resolved_runtime" && -f "$resolved_runtime" ]] || {
  echo "Clean consumer did not resolve an installed libzlink runtime" >&2
  exit 1
}
resolved_runtime="$(readlink -f "$resolved_runtime")"
case "$resolved_runtime" in
  "$(readlink -f "$lib_dir")"/*) ;;
  *) echo "Clean consumer resolved libzlink outside the install prefix: $resolved_runtime" >&2; exit 1 ;;
esac
runtime_version="$(env -u LD_LIBRARY_PATH "$consumer_dir/consumer")"
[[ "$runtime_version" == "$manifest_version" ]] || {
  echo "Installed runtime version $runtime_version does not match package manifest $manifest_version" >&2
  exit 1
}
expected_soname="libzlink.so.${manifest_version%%.*}"
runtime_soname="$(readelf -d "$resolved_runtime" | sed -n 's/.*(SONAME).*\[\([^]]*\)\].*/\1/p' | head -n1)"
[[ "$runtime_soname" == "$expected_soname" ]] || {
  echo "Installed runtime SONAME $runtime_soname does not match expected $expected_soname" >&2
  exit 1
}

mkdir -p "$(dirname "$evidence")"
PREFIX="$prefix" EVIDENCE="$evidence" COMPILER="$compiler" \
RUNTIME="$resolved_runtime" SERVICE_HEADER_COUNT="${#service_headers[@]}" \
MANIFEST="$manifest" MANIFEST_SHA256="$manifest_sha256" MANIFEST_FILE_COUNT="$manifest_file_count" \
MANIFEST_VERSION="$manifest_version" RUNTIME_VERSION="$runtime_version" \
RUNTIME_SONAME="$runtime_soname" EXPECTED_SONAME="$expected_soname" \
CANDIDATE_SUMMARY="$candidate_summary" MANIFEST_CANDIDATE_SHA256="$manifest_candidate_sha256" \
RUNTIME_SHA256="$(sha256sum "$resolved_runtime" | awk '{print $1}')" \
node <<'NODE'
const fs = require('node:fs');
const path = require('node:path');
const candidate = JSON.parse(process.env.CANDIDATE_SUMMARY);
const result = {
  schema: 1,
  command: 'CORE-PKG-CONSUMER',
  status: 'pass',
  completedAt: new Date().toISOString(),
  candidate: {
    ledgerId: candidate.ledgerId,
    baseRevision: candidate.baseRevision,
    manifestSha256: candidate.manifestSha256,
    aggregateSha256: candidate.aggregateSha256,
  },
  approval: candidate.approval,
  prefix: process.env.PREFIX,
  checks: {
    installedZlinkHeader: true,
    provenanceManifestValid: true,
    provenanceCandidateMatches: process.env.MANIFEST_CANDIDATE_SHA256 === candidate.manifestSha256,
    provenanceFileCount: Number(process.env.MANIFEST_FILE_COUNT),
    serviceHeaderCount: Number(process.env.SERVICE_HEADER_COUNT),
    cleanCConsumerCompileExitCode: 0,
    cleanCConsumerRunExitCode: 0,
    runtimeResolvedInsidePrefix: true,
    runtimeVersionMatchesManifest: true,
    runtimeSonameMatchesMajor: process.env.RUNTIME_SONAME === process.env.EXPECTED_SONAME,
  },
  compiler: process.env.COMPILER,
  provenance: {
    path: process.env.MANIFEST,
    sha256: process.env.MANIFEST_SHA256,
  },
  runtime: {
    path: process.env.RUNTIME,
    sha256: process.env.RUNTIME_SHA256,
    version: process.env.RUNTIME_VERSION,
    soname: process.env.RUNTIME_SONAME,
  },
  manifestVersion: process.env.MANIFEST_VERSION,
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE

echo "Core package clean C consumer passed: $prefix"
