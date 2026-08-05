#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
build_dir=""
output_root=""
evidence=""
candidate_manifest=""
review_evidence=""
configuration="${CONFIGURATION:-Release}"
snapshot_workspace=""
consumer_evidence=""

cleanup() {
  [[ -z "$snapshot_workspace" ]] || rm -rf "$snapshot_workspace"
  [[ -z "$consumer_evidence" ]] || rm -f "$consumer_evidence"
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage: build-wsl.sh --build-dir DIR --output-root ABSOLUTE_DIR \
  --candidate-manifest ABSOLUTE_JSON --review-evidence ABSOLUTE_JSON \
  --evidence ABSOLUTE_JSON

Installs the already-built Core candidate into a versioned local prefix and
verifies its raw-only headers, provenance, and clean C consumer. It does not
publish externally.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) build_dir="${2:-}"; shift 2 ;;
    --output-root) output_root="${2:-}"; shift 2 ;;
    --candidate-manifest) candidate_manifest="${2:-}"; shift 2 ;;
    --review-evidence) review_evidence="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$build_dir" ]] || { echo "--build-dir is required" >&2; exit 2; }
[[ "$output_root" = /* ]] || { echo "--output-root must be an absolute path" >&2; exit 2; }
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

reject_dot_segments "$output_root" "--output-root"
reject_dot_segments "$candidate_manifest" "--candidate-manifest"
reject_dot_segments "$review_evidence" "--review-evidence"
reject_dot_segments "$evidence" "--evidence"

repo_root="$(readlink -f "$repo_root")"
output_root="$(realpath -m "$output_root")"
candidate_manifest="$(readlink -f "$candidate_manifest")"
review_evidence="$(readlink -f "$review_evidence")"
evidence="$(realpath -m "$evidence")"
[[ -f "$candidate_manifest" ]] || { echo "Candidate manifest is missing" >&2; exit 1; }
[[ -f "$review_evidence" ]] || { echo "Review evidence is missing" >&2; exit 1; }

candidate_summary="$(node "$script_dir/verify-candidate.mjs" \
  --candidate-manifest "$candidate_manifest" --review-evidence "$review_evidence" \
  --repo-root "$repo_root")"

if [[ "$build_dir" != /* ]]; then
  build_dir="$repo_root/$build_dir"
fi
build_dir="$(readlink -f "$build_dir")"
[[ -f "$build_dir/CMakeCache.txt" ]] || { echo "Core CMake build is missing: $build_dir" >&2; exit 1; }

source_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$build_dir/CMakeCache.txt")"
[[ "$(readlink -f "$source_dir")" == "$(readlink -f "$repo_root/core")" ]] || {
  echo "--build-dir is not configured from this repository's core source" >&2
  exit 1
}

version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
[[ "$version" =~ ^11\.[0-9]+\.[0-9]+$ ]] || {
  echo "Core package requires an 11.x numeric VERSION, found: ${version:-<missing>}" >&2
  exit 1
}

build_version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build_dir/CMakeCache.txt")"
[[ "$build_version" == "$version" ]] || {
  echo "Core build version $build_version does not match repository VERSION $version" >&2
  exit 1
}

build_jobs="${ZLINK_CORE_PACKAGE_BUILD_JOBS:-4}"
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || {
  echo "ZLINK_CORE_PACKAGE_BUILD_JOBS must be a positive integer" >&2
  exit 2
}

# Materialize source from the immutable base revision and overlay only the
# content-addressed candidate records. Build and install use this private
# snapshot, so concurrent changes in the shared worktree cannot affect the
# approved package artifact.
snapshot_workspace="$(mktemp -d)"
snapshot_source="$snapshot_workspace/source"
snapshot_build="$snapshot_workspace/build"
mkdir -p "$snapshot_source"
base_revision="$(node -e 'const c=require(process.argv[1]); process.stdout.write(c.baseRevision)' "$candidate_manifest")"
git -C "$repo_root" archive "$base_revision" LICENSE VERSION core | tar -xf - -C "$snapshot_source"
node "$script_dir/materialize-build-source.mjs" \
  "$candidate_manifest" "$repo_root" "$snapshot_source"

build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$build_dir/CMakeCache.txt")"
with_tls="$(sed -n 's/^WITH_TLS:BOOL=//p' "$build_dir/CMakeCache.txt")"
build_shared="$(sed -n 's/^BUILD_SHARED:BOOL=//p' "$build_dir/CMakeCache.txt")"
build_static="$(sed -n 's/^BUILD_STATIC:BOOL=//p' "$build_dir/CMakeCache.txt")"
[[ -n "$build_type" ]] || build_type="$configuration"
cmake -S "$snapshot_source/core" -B "$snapshot_build" \
  -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_TESTS=OFF -DZLINK_BUILD_TESTS=OFF \
  -DBUILD_BENCHMARKS=OFF -DWITH_DOCS=OFF -DWITH_TLS="${with_tls:-ON}" \
  -DBUILD_SHARED="${build_shared:-ON}" -DBUILD_STATIC="${build_static:-ON}"
cmake --build "$snapshot_build" --parallel "$build_jobs"

[[ "$output_root" != / && "$output_root" != "$repo_root" ]] || {
  echo "Unsafe --output-root: $output_root" >&2
  exit 2
}
prefix="$(realpath -m "$output_root/install/zlink-core/$version")"
case "$prefix" in "$output_root"/*) ;; *) echo "Install prefix escapes output root" >&2; exit 2 ;; esac
case "$prefix" in "$repo_root"|/) echo "Unsafe install prefix: $prefix" >&2; exit 2 ;; esac
case "$evidence" in "$prefix"|"$prefix"/*) echo "--evidence must be outside the install prefix" >&2; exit 2 ;; esac

rm -rf "$prefix"
mkdir -p "$prefix" "$(dirname "$evidence")"
cmake --install "$snapshot_build" --prefix "$prefix"

manifest="$prefix/share/zlink/core-package-provenance.json"
mkdir -p "$(dirname "$manifest")"
PREFIX="$prefix" MANIFEST="$manifest" VERSION="$version" \
CANDIDATE_SUMMARY="$candidate_summary" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const root = process.env.PREFIX;
const candidate = JSON.parse(process.env.CANDIDATE_SUMMARY);
function files(directory) {
  return fs.readdirSync(directory, {withFileTypes: true}).flatMap(entry => {
    const full = path.join(directory, entry.name);
    return entry.isDirectory() ? files(full) : [full];
  });
}
const records = files(root)
  .filter(file => file !== process.env.MANIFEST)
  .map(file => ({
    path: path.relative(root, file).split(path.sep).join('/'),
    sha256: crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'),
  }))
  .sort((left, right) => left.path.localeCompare(right.path, 'en'));
const manifest = {
  schema: 1,
  package: 'zlink-core',
  version: process.env.VERSION,
  candidate: {
    ledgerId: candidate.ledgerId,
    baseRevision: candidate.baseRevision,
    manifestSha256: candidate.manifestSha256,
    aggregateSha256: candidate.aggregateSha256,
    approvalEvidenceSha256: candidate.approval.evidenceSha256,
  },
  createdAt: new Date().toISOString(),
  files: records,
};
fs.writeFileSync(process.env.MANIFEST, `${JSON.stringify(manifest, null, 2)}\n`);
NODE

consumer_evidence="$(mktemp)"
"$repo_root/scripts/v11/verify-core-package-consumer.sh" \
  --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
  --review-evidence "$review_evidence" \
  --evidence "$consumer_evidence"

PREFIX="$prefix" EVIDENCE="$evidence" VERSION="$version" MANIFEST="$manifest" \
CONSUMER_EVIDENCE="$consumer_evidence" CANDIDATE_SUMMARY="$candidate_summary" \
BUILD_JOBS="$build_jobs" BUILD_TYPE="$build_type" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const consumer = JSON.parse(fs.readFileSync(process.env.CONSUMER_EVIDENCE, 'utf8'));
const candidate = JSON.parse(process.env.CANDIDATE_SUMMARY);
const result = {
  schema: 1,
  ledgerId: 'V11-M3-CORE-PKG',
  command: 'CORE-PKG',
  status: 'pass',
  completedAt: new Date().toISOString(),
  candidate: {
    ledgerId: candidate.ledgerId,
    baseRevision: candidate.baseRevision,
    manifestSha256: candidate.manifestSha256,
    aggregateSha256: candidate.aggregateSha256,
  },
  approval: candidate.approval,
  version: process.env.VERSION,
  build: {
    source: 'isolated candidate snapshot',
    cleanBuildDirectory: true,
    jobs: Number(process.env.BUILD_JOBS),
    buildType: process.env.BUILD_TYPE,
  },
  output: {
    prefix: process.env.PREFIX,
    provenanceManifest: process.env.MANIFEST,
    provenanceSha256: crypto.createHash('sha256').update(fs.readFileSync(process.env.MANIFEST)).digest('hex'),
  },
  checks: consumer.checks,
  consumer,
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE

echo "Core local package passed: $prefix"
echo "Evidence: $evidence"
