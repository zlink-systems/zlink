#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
self_test=false
dry_run=false
evidence=""

usage() {
  cat <<'EOF'
Usage: test-build-wsl.sh --self-test --dry-run --evidence ABSOLUTE_JSON

Runs fixture-only Core package tooling tests. No install artifact is published.
EOF
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

$self_test || { echo "--self-test is required" >&2; exit 2; }
$dry_run || { echo "--dry-run is required for tooling self-test" >&2; exit 2; }
[[ "$evidence" = /* ]] || { echo "--evidence must be an absolute path" >&2; exit 2; }

fixture="$(mktemp -d)"
direct_input_rel=".artifacts/v11/tmp/core-package-selftest-direct-input-$$"
direct_input="$repo_root/$direct_input_rel"
trap 'rm -rf "$fixture"; rm -f "$direct_input"' EXIT
prefix="$fixture/install/zlink-core/11.1.0"
mkdir -p "$prefix/include" "$prefix/lib" "$prefix/share/zlink"
mkdir -p "$(dirname "$direct_input")"
printf '%s\n' 'sealed review state' >"$direct_input"
direct_input_sha256="$(sha256sum "$direct_input" | awk '{print $1}')"

candidate_manifest="$fixture/core-candidate.json"
node - "$repo_root" "$candidate_manifest" "$direct_input_rel" "$direct_input_sha256" <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const root = process.argv[2];
const output = process.argv[3];
const directInputPath = process.argv[4];
const directInputSha256 = process.argv[5];
function git(args, encoding = 'utf8') {
  const result = cp.spawnSync('git', args, {cwd: root, encoding, maxBuffer: 128 * 1024 * 1024});
  if (result.status !== 0) throw new Error(result.stderr.toString());
  return result.stdout;
}
function sha(value) { return crypto.createHash('sha256').update(value).digest('hex'); }
function bytes(file) {
  const stat = fs.lstatSync(file);
  return stat.isSymbolicLink() ? Buffer.from(fs.readlinkSync(file), 'utf8') : fs.readFileSync(file);
}
const base = git(['rev-parse', 'HEAD']).trim();
const changed = new Set(git(['diff', '--name-only', base, '--', 'core']).trim().split('\n').filter(Boolean));
for (const file of git(['ls-files', '--others', '--exclude-standard', '--', 'core']).trim().split('\n').filter(Boolean)) changed.add(file);
const files = [...changed].sort().map(file => {
  const absolute = path.join(root, file);
  const current = fs.existsSync(absolute);
  const baseProbe = cp.spawnSync('git', ['cat-file', '-e', `${base}:${file}`], {cwd: root});
  const inBase = baseProbe.status === 0;
  const status = current ? (inBase ? 'modified' : 'added') : 'deleted';
  return {
    path: file,
    status,
    mode: current ? `100${(fs.lstatSync(absolute).mode & 0o777).toString(8).padStart(3, '0')}` : null,
    contentSha256: current ? sha(bytes(absolute)) : null,
    baseContentSha256: inBase ? sha(git(['show', `${base}:${file}`], null)) : null,
  };
});
const candidate = {
  schema: 'zlink-v11-ledger-candidate-v1',
  ledgerId: 'V11-M3-CORE-VERIFY',
  baseRevision: base,
  ownedPaths: ['core'],
  directInputs: [{path: directInputPath, contentSha256: directInputSha256}],
  pathCount: files.length,
  aggregateSha256: sha(JSON.stringify(files)),
  files,
};
fs.writeFileSync(output, `${JSON.stringify(candidate, null, 2)}\n`);
NODE
review_evidence="$fixture/r2-review-evidence.json"
CANDIDATE_MANIFEST="$candidate_manifest" REVIEW_EVIDENCE="$review_evidence" \
BASE_REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const result = {
  schema: 'zlink-v11-ledger-evidence-v1',
  ledgerId: 'V11-R2',
  status: 'passed',
  sourceRevision: process.env.BASE_REVISION,
  candidateManifestSha256: '1'.repeat(64),
  ownedPathManifestSha256: '0'.repeat(64),
  commands: [{name: 'INDEPENDENT-REVIEW', exitCode: 0, required: true}],
  completedAt: new Date().toISOString(),
  details: {
    approvedCandidateManifestSha256: crypto.createHash('sha256')
      .update(fs.readFileSync(process.env.CANDIDATE_MANIFEST)).digest('hex'),
  },
  issues: [],
};
fs.writeFileSync(process.env.REVIEW_EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE

# Review status is expected to advance after sealing. Package validation must
# retain the approved candidate identity without coupling it to these mutable
# direct-input bytes.
printf '%s\n' 'review completed after seal' >"$direct_input"
candidate_summary="$(node "$script_dir/verify-candidate.mjs" \
  --candidate-manifest "$candidate_manifest" --review-evidence "$review_evidence" \
  --repo-root "$repo_root")"
materialized_source="$fixture/materialized-source"
mkdir -p "$materialized_source"
fixture_base_revision="$(node -e 'const c=require(process.argv[1]); process.stdout.write(c.baseRevision)' "$candidate_manifest")"
git -C "$repo_root" archive "$fixture_base_revision" LICENSE VERSION core \
  | tar -xf - -C "$materialized_source"
node "$script_dir/materialize-build-source.mjs" \
  "$candidate_manifest" "$repo_root" "$materialized_source" >/dev/null
[[ -f "$materialized_source/LICENSE" ]] || {
  echo "Materialized Core build source is missing the repository license" >&2
  exit 1
}
cmp "$repo_root/core/CMakeLists.txt" "$materialized_source/core/CMakeLists.txt"
cp "$candidate_manifest" "$fixture/materialize-tampered.json"
node - "$fixture/materialize-tampered.json" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const candidate = JSON.parse(fs.readFileSync(file, 'utf8'));
const record = candidate.files.find(item => item.contentSha256 !== null && item.path.startsWith('core/'));
if (!record) throw new Error('fixture candidate has no materialized Core record');
record.contentSha256 = '0'.repeat(64);
fs.writeFileSync(file, `${JSON.stringify(candidate, null, 2)}\n`);
NODE
if node "$script_dir/materialize-build-source.mjs" \
    "$fixture/materialize-tampered.json" "$repo_root" "$fixture/materialized-tampered" \
    >/dev/null 2>&1; then
  echo "Materializer accepted candidate content with a mismatched hash" >&2
  exit 1
fi
cat >"$prefix/include/zlink.h" <<'EOF'
#ifndef ZLINK_H_INCLUDED
#define ZLINK_H_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif
void zlink_version(int *major, int *minor, int *patch);
#ifdef __cplusplus
}
#endif
#endif
EOF
cat >"$fixture/zlink.c" <<'EOF'
#include <zlink.h>
void zlink_version(int *major, int *minor, int *patch)
{
    *major = 11;
    *minor = 1;
    *patch = 0;
}
EOF
cc -std=c11 -fPIC -shared -I"$prefix/include" "$fixture/zlink.c" \
  -Wl,-soname,libzlink.so.11 -o "$prefix/lib/libzlink.so.11.1.0"
ln -s libzlink.so.11.1.0 "$prefix/lib/libzlink.so.11"
ln -s libzlink.so.11 "$prefix/lib/libzlink.so"

PREFIX="$prefix" CANDIDATE_SUMMARY="$candidate_summary" node <<'NODE'
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
const records = files(root).map(file => ({
  path: path.relative(root, file).split(path.sep).join('/'),
  sha256: crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'),
})).sort((left, right) => left.path.localeCompare(right.path, 'en'));
const manifest = {
  schema: 1,
  package: 'zlink-core',
  version: '11.1.0',
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
fs.writeFileSync(path.join(root, 'share/zlink/core-package-provenance.json'), `${JSON.stringify(manifest, null, 2)}\n`);
NODE

positive="$fixture/positive.json"
"$repo_root/scripts/v11/verify-core-package-consumer.sh" \
  --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
  --review-evidence "$review_evidence" \
  --evidence "$positive"

cp "$prefix/lib/libzlink.so.11.1.0" "$fixture/libzlink.so.11.1.0.saved"
cp "$prefix/share/zlink/core-package-provenance.json" "$fixture/manifest-soname.saved"
cc -std=c11 -fPIC -shared -I"$prefix/include" "$fixture/zlink.c" \
  -Wl,-soname,libzlink.so.10 -o "$prefix/lib/libzlink.so.11.1.0"
RUNTIME_SHA256="$(sha256sum "$prefix/lib/libzlink.so.11.1.0" | awk '{print $1}')" \
node - "$prefix/share/zlink/core-package-provenance.json" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const manifest = JSON.parse(fs.readFileSync(file, 'utf8'));
const runtime = manifest.files.find(record => record.path === 'lib/libzlink.so.11.1.0');
if (!runtime) throw new Error('fixture runtime is missing from provenance');
runtime.sha256 = process.env.RUNTIME_SHA256;
fs.writeFileSync(file, `${JSON.stringify(manifest, null, 2)}\n`);
NODE
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-soname.json" >/dev/null 2>&1; then
  echo "Verifier accepted a runtime SONAME that differs from the package major" >&2
  exit 1
fi
mv "$fixture/libzlink.so.11.1.0.saved" "$prefix/lib/libzlink.so.11.1.0"
mv "$fixture/manifest-soname.saved" "$prefix/share/zlink/core-package-provenance.json"

mkdir -p "$prefix/include/zlink/service"
printf '%s\n' '#error service headers must not be installed' >"$prefix/include/zlink/service/common.h"
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-service.json" >/dev/null 2>&1; then
  echo "Verifier accepted an installed service header" >&2
  exit 1
fi
rm -rf "$prefix/include/zlink/service"

mv "$prefix/lib/libzlink.so" "$fixture/libzlink.so.saved"
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-runtime.json" >/dev/null 2>&1; then
  echo "Verifier accepted a package without libzlink.so" >&2
  exit 1
fi
mv "$fixture/libzlink.so.saved" "$prefix/lib/libzlink.so"

cp "$prefix/share/zlink/core-package-provenance.json" "$fixture/manifest.saved"
node - "$prefix/share/zlink/core-package-provenance.json" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const manifest = JSON.parse(fs.readFileSync(file, 'utf8'));
manifest.files[0].sha256 = '0'.repeat(64);
fs.writeFileSync(file, `${JSON.stringify(manifest, null, 2)}\n`);
NODE
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-manifest.json" >/dev/null 2>&1; then
  echo "Verifier accepted an invalid provenance manifest" >&2
  exit 1
fi
mv "$fixture/manifest.saved" "$prefix/share/zlink/core-package-provenance.json"

cp "$prefix/share/zlink/core-package-provenance.json" "$fixture/manifest-candidate.saved"
node - "$prefix/share/zlink/core-package-provenance.json" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const manifest = JSON.parse(fs.readFileSync(file, 'utf8'));
manifest.candidate.aggregateSha256 = '0'.repeat(64);
fs.writeFileSync(file, `${JSON.stringify(manifest, null, 2)}\n`);
NODE
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-manifest-candidate.json" >/dev/null 2>&1; then
  echo "Verifier accepted provenance for a different candidate" >&2
  exit 1
fi
mv "$fixture/manifest-candidate.saved" "$prefix/share/zlink/core-package-provenance.json"

cp "$prefix/share/zlink/core-package-provenance.json" "$fixture/manifest-version.saved"
node - "$prefix/share/zlink/core-package-provenance.json" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const manifest = JSON.parse(fs.readFileSync(file, 'utf8'));
manifest.version = '11.0.1';
fs.writeFileSync(file, `${JSON.stringify(manifest, null, 2)}\n`);
NODE
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-version.json" >/dev/null 2>&1; then
  echo "Verifier accepted a runtime version that differs from the manifest" >&2
  exit 1
fi
mv "$fixture/manifest-version.saved" "$prefix/share/zlink/core-package-provenance.json"

cp "$candidate_manifest" "$fixture/candidate-tampered.json"
node - "$fixture/candidate-tampered.json" <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const file = process.argv[2];
const candidate = JSON.parse(fs.readFileSync(file, 'utf8'));
const record = candidate.files.find(item => item.contentSha256 !== null);
record.contentSha256 = '0'.repeat(64);
candidate.aggregateSha256 = crypto.createHash('sha256')
  .update(JSON.stringify(candidate.files)).digest('hex');
fs.writeFileSync(file, `${JSON.stringify(candidate, null, 2)}\n`);
NODE
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$fixture/candidate-tampered.json" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/negative-candidate.json" >/dev/null 2>&1; then
  echo "Verifier accepted a tampered candidate manifest" >&2
  exit 1
fi

if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --review-evidence "$review_evidence" \
    --evidence "$fixture/missing-candidate.json" >/dev/null 2>&1; then
  echo "Verifier accepted a missing candidate argument" >&2
  exit 1
fi

cp "$review_evidence" "$fixture/review-wrong-candidate.json"
node - "$fixture/review-wrong-candidate.json" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const review = JSON.parse(fs.readFileSync(file, 'utf8'));
review.details.approvedCandidateManifestSha256 = 'f'.repeat(64);
fs.writeFileSync(file, `${JSON.stringify(review, null, 2)}\n`);
NODE
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$fixture/review-wrong-candidate.json" \
    --evidence "$fixture/wrong-approval.json" >/dev/null 2>&1; then
  echo "Verifier accepted review evidence for a different candidate" >&2
  exit 1
fi
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --evidence "$fixture/missing-review.json" >/dev/null 2>&1; then
  echo "Verifier accepted a missing review-evidence argument" >&2
  exit 1
fi
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" \
    --prefix "$prefix" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$fixture/does-not-exist-review.json" \
    --evidence "$fixture/missing-review-file.json" >/dev/null 2>&1; then
  echo "Verifier accepted a missing review-evidence file" >&2
  exit 1
fi

if "$script_dir/build-wsl.sh" --build-dir core/build --output-root relative \
    --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/invalid.json" >/dev/null 2>&1; then
  echo "Builder accepted a relative output root" >&2
  exit 1
fi
if "$script_dir/build-wsl.sh" --build-dir core/build \
    --output-root "$fixture/output/../escaped" --candidate-manifest "$candidate_manifest" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/dot-segment.json" >/dev/null 2>&1; then
  echo "Builder accepted dot segments in the output root" >&2
  exit 1
fi
if "$script_dir/build-wsl.sh" --build-dir core/build --output-root "$fixture/output" \
    --review-evidence "$review_evidence" \
    --evidence "$fixture/missing-candidate-build.json" >/dev/null 2>&1; then
  echo "Builder accepted a missing candidate argument" >&2
  exit 1
fi
if "$script_dir/build-wsl.sh" --build-dir core/build --output-root "$fixture/output" \
    --candidate-manifest "$candidate_manifest" \
    --evidence "$fixture/missing-review-build.json" >/dev/null 2>&1; then
  echo "Builder accepted a missing review-evidence argument" >&2
  exit 1
fi
if "$repo_root/scripts/v11/verify-core-package-consumer.sh" --unknown \
    >/dev/null 2>&1; then
  echo "Verifier accepted an unknown argument" >&2
  exit 1
fi

node - "$positive" <<'NODE'
const fs = require('node:fs');
const result = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
for (const key of ['schema', 'command', 'status', 'completedAt', 'candidate', 'approval', 'prefix', 'checks', 'runtime']) {
  if (!(key in result)) throw new Error(`consumer evidence is missing ${key}`);
}
if (result.status !== 'pass' || result.checks.serviceHeaderCount !== 0
    || result.checks.cleanCConsumerCompileExitCode !== 0
    || result.checks.cleanCConsumerRunExitCode !== 0
    || result.checks.runtimeResolvedInsidePrefix !== true
    || result.checks.provenanceCandidateMatches !== true
    || result.approval.ledgerId !== 'V11-R2'
    || result.candidate.ledgerId !== 'V11-M3-CORE-VERIFY') {
  throw new Error('consumer evidence has invalid pass claims');
}
NODE

mkdir -p "$(dirname "$evidence")"
EVIDENCE="$evidence" POSITIVE="$positive" CANDIDATE_SUMMARY="$candidate_summary" node <<'NODE'
const fs = require('node:fs');
const consumer = JSON.parse(fs.readFileSync(process.env.POSITIVE, 'utf8'));
const candidate = JSON.parse(process.env.CANDIDATE_SUMMARY);
const result = {
  schema: 1,
  ledgerId: 'V11-M3-CORE-CLEAN',
  command: 'CORE-PKG-TEST',
  status: 'pass',
  completedAt: new Date().toISOString(),
  candidate: {
    ledgerId: candidate.ledgerId,
    baseRevision: candidate.baseRevision,
    manifestSha256: candidate.manifestSha256,
    aggregateSha256: candidate.aggregateSha256,
  },
  approval: candidate.approval,
  dryRun: true,
  publishedArtifactCount: 0,
  checks: {
    argumentValidation: 'pass',
    evidenceManifestValidation: 'pass',
    isolatedCandidateMaterialization: 'pass',
    isolatedLicenseMaterialization: 'pass',
    materializedHashMismatchRejected: true,
    cleanCConsumerFixture: 'pass',
    serviceHeaderCopyCount: 0,
    missingRuntimeRejected: true,
    installedServiceHeaderRejected: true,
    invalidProvenanceManifestRejected: true,
    provenanceCandidateMismatchRejected: true,
    runtimeVersionMismatchRejected: true,
    runtimeSonameMismatchRejected: true,
    missingCandidateRejected: true,
    tamperedCandidateRejected: true,
    dotSegmentOutputRootRejected: true,
    mutableDirectInputAcceptedWithExactApproval: true,
    wrongApprovalCandidateRejected: true,
    missingReviewEvidenceRejected: true,
  },
  consumerFixture: consumer,
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE

echo "Core package tooling self-test passed without publishing artifacts"
echo "Evidence: $evidence"
