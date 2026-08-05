#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
BINDING_ROOT="${REPO_ROOT}/bindings/go"
MODULE_PATH="$(sed -n 's/^module //p' "${BINDING_ROOT}/go.mod" | head -n1)"
CORE_VERSION="$(sed -n 's/^#define ZLINK_VERSION_MAJOR //p' "${BINDING_ROOT}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_MINOR //p' "${BINDING_ROOT}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_PATCH //p' "${BINDING_ROOT}/include/zlink.h" | head -n1)"
PACKAGE_VERSION="v${CORE_VERSION}"
PLATFORMS="linux-x86_64"
OUTPUT_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-${REPO_ROOT}/.artifacts/wsl}/go"
CORE_CANDIDATE_MANIFEST=""
CORE_PACKAGE_EVIDENCE=""

usage() {
  cat <<'EOF'
Usage: scripts/local-package/go/build-wsl.sh [options]

Options:
  --package-version VERSION  Go module version, including the v prefix.
  --platforms LIST           Comma-separated native payload directories.
                             Current candidate support: linux-x86_64.
  --core-candidate-manifest FILE
                             Approved V11-M3-CORE-VERIFY candidate manifest.
  --core-package-evidence FILE
                             Matching V11-M3-CORE-PKG pass evidence.
  --output-root DIR          Absolute output directory.
  -h, --help

The command creates a standard file proxy layout and runs a clean consumer
without a replace directive. It never uses core/build as a runtime input.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --package-version) PACKAGE_VERSION="${2:-}"; shift 2 ;;
    --platforms) PLATFORMS="${2:-}"; shift 2 ;;
    --core-candidate-manifest) CORE_CANDIDATE_MANIFEST="${2:-}"; shift 2 ;;
    --core-package-evidence) CORE_PACKAGE_EVIDENCE="${2:-}"; shift 2 ;;
    --output-root) OUTPUT_ROOT="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "${OUTPUT_ROOT}" = /* ]] || { echo "--output-root must be absolute" >&2; exit 2; }
[[ -n "${CORE_CANDIDATE_MANIFEST}" && -n "${CORE_PACKAGE_EVIDENCE}" ]] || {
  echo "--core-candidate-manifest and --core-package-evidence are required" >&2
  exit 2
}
[[ "${CORE_CANDIDATE_MANIFEST}" = /* && "${CORE_PACKAGE_EVIDENCE}" = /* ]] || {
  echo "Core candidate and package evidence paths must be absolute" >&2
  exit 2
}
CORE_CANDIDATE_MANIFEST="$(realpath -e -- "${CORE_CANDIDATE_MANIFEST}")" || {
  echo "Core candidate manifest does not exist" >&2
  exit 1
}
CORE_PACKAGE_EVIDENCE="$(realpath -e -- "${CORE_PACKAGE_EVIDENCE}")" || {
  echo "Core package evidence does not exist" >&2
  exit 1
}
[[ "${PACKAGE_VERSION}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "--package-version must use vMAJOR.MINOR.PATCH" >&2
  exit 2
}
[[ "${MODULE_PATH}" == "zlink.systems/zlink/v11" ]] || {
  echo "Unexpected Go module path: ${MODULE_PATH}" >&2
  exit 1
}
[[ "${PACKAGE_VERSION}" == "v${CORE_VERSION}" ]] || {
  echo "Go package version ${PACKAGE_VERSION} must match Core ${CORE_VERSION}" >&2
  exit 1
}
if ! git -C "${REPO_ROOT}" diff --quiet -- bindings/go ':(exclude)bindings/go/native/**' || \
   ! git -C "${REPO_ROOT}" diff --cached --quiet -- bindings/go ':(exclude)bindings/go/native/**'; then
  echo "Go package source must be committed before package materialization" >&2
  exit 1
fi

dir_hash() {
  local root="$1"
  (
    cd "${root}"
    find . -type f -print0 | sort -z | while IFS= read -r -d '' file; do
      printf '%s  %s\n' "$(sha256sum "${file}" | awk '{print $1}')" "${file#./}"
    done
  ) | sha256sum | awk '{print $1}'
}

CORE_PACKAGE_FIELDS="$(EXPECTED_CORE_VERSION="${CORE_VERSION}" node - "${CORE_PACKAGE_EVIDENCE}" "${CORE_CANDIDATE_MANIFEST}" <<'NODE'
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const evidencePath = path.resolve(process.argv[2]);
const candidatePath = path.resolve(process.argv[3]);
const expectedVersion = process.env.EXPECTED_CORE_VERSION;

function fail(message) {
  throw new Error(message);
}

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    fail(`cannot read JSON ${file}: ${error.message}`);
  }
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function requireFile(file, label) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) fail(`${label} is missing: ${file}`);
}

requireFile(evidencePath, 'Core package evidence');
requireFile(candidatePath, 'Core candidate manifest');
const evidence = readJson(evidencePath);
const candidate = readJson(candidatePath);

if (evidence.schema !== 1 || evidence.ledgerId !== 'V11-M3-CORE-PKG' || evidence.status !== 'pass') {
  fail('Core package evidence is not a passing V11-M3-CORE-PKG record');
}
if (evidence.version !== expectedVersion) fail(`Core package version mismatch: ${evidence.version} != ${expectedVersion}`);
if (candidate.schema !== 'zlink-v11-ledger-candidate-v1' || candidate.ledgerId !== 'V11-M3-CORE-VERIFY') {
  fail('Core candidate manifest is not a V11-M3-CORE-VERIFY record');
}
if (sha256(candidatePath) !== evidence.candidate?.manifestSha256) fail('Candidate manifest SHA-256 does not match package evidence');
if (candidate.aggregateSha256 !== evidence.candidate?.aggregateSha256) fail('Candidate aggregate SHA-256 does not match package evidence');
if (evidence.approval?.ledgerId !== 'V11-R2' || evidence.approval.candidateManifestSha256 !== evidence.candidate.manifestSha256) {
  fail('Core package approval does not identify the same candidate');
}
const approvalPath = path.resolve(evidence.approval.evidencePath);
requireFile(approvalPath, 'Core review evidence');
if (sha256(approvalPath) !== evidence.approval.evidenceSha256) fail('Core review evidence SHA-256 mismatch');
const approval = readJson(approvalPath);
if (approval.status !== 'passed' || approval.candidateManifestSha256 !== evidence.candidate.manifestSha256) {
  fail('Core review evidence is not a pass for the packaged candidate');
}
for (const [name, value] of Object.entries(evidence.checks ?? {})) {
  if (typeof value === 'boolean' && !value) fail(`Core package check failed: ${name}`);
}
for (const [name, value] of Object.entries(evidence.consumer?.checks ?? {})) {
  if (typeof value === 'boolean' && !value) fail(`Core consumer check failed: ${name}`);
}
if (evidence.consumer?.status !== 'pass') fail('Core package consumer is not a pass');

const prefix = path.resolve(evidence.output?.prefix ?? '');
const provenancePath = path.resolve(evidence.output?.provenanceManifest ?? '');
requireFile(provenancePath, 'Core provenance manifest');
if (sha256(provenancePath) !== evidence.output.provenanceSha256) fail('Core provenance manifest SHA-256 mismatch');
const provenance = readJson(provenancePath);
if (provenance.candidate?.manifestSha256 !== evidence.candidate.manifestSha256 ||
    provenance.candidate?.aggregateSha256 !== evidence.candidate.aggregateSha256) {
  fail('Core provenance manifest identifies a different candidate');
}

const runtimePath = path.resolve(evidence.consumer?.runtime?.path ?? '');
requireFile(runtimePath, 'Core runtime');
const runtimeSha256 = sha256(runtimePath);
if (runtimeSha256 !== evidence.consumer.runtime.sha256) fail('Core runtime SHA-256 mismatch');
if (evidence.consumer.runtime.version !== expectedVersion || evidence.consumer.runtime.soname !== 'libzlink.so.11') {
  fail('Core runtime version or SONAME mismatch');
}
const runtimeRelative = path.relative(prefix, runtimePath).split(path.sep).join('/');
const provenanceRuntime = (provenance.files ?? []).find(record => record.path === runtimeRelative);
if (!provenanceRuntime || provenanceRuntime.sha256 !== runtimeSha256) fail('Core provenance does not contain the verified runtime');
if (!fs.existsSync(path.join(prefix, 'include', 'zlink.h'))) fail('Core package include directory is missing');

process.stdout.write([
  prefix,
  runtimePath,
  runtimeSha256,
  provenancePath,
  evidence.output.provenanceSha256,
  evidence.candidate.manifestSha256,
  evidence.candidate.aggregateSha256,
  evidence.approval.evidenceSha256,
].join('\t'));
NODE
)"
IFS=$'\t' read -r CORE_PACKAGE_PREFIX CORE_RUNTIME_SOURCE CORE_RUNTIME_SHA256 CORE_PROVENANCE_PATH \
  CORE_PROVENANCE_SHA256 CORE_CANDIDATE_MANIFEST_SHA256 CORE_CANDIDATE_AGGREGATE_SHA256 \
  CORE_APPROVAL_EVIDENCE_SHA256 <<< "${CORE_PACKAGE_FIELDS}"
[[ -n "${CORE_PACKAGE_PREFIX}" && -n "${CORE_RUNTIME_SOURCE}" && -n "${CORE_RUNTIME_SHA256}" ]] || {
  echo "Core package evidence did not provide a runtime" >&2
  exit 1
}
[[ "$(dir_hash "${BINDING_ROOT}/include")" == "$(dir_hash "${CORE_PACKAGE_PREFIX}/include")" ]] || {
  echo "Go package headers do not match the approved Core package" >&2
  exit 1
}

platform_source_dir() {
  case "$1" in
    linux-x86_64)
      printf '%s/native/%s\n' "${BINDING_ROOT}" "$1"
      ;;
    *)
      echo "Go package platform is not present in the supplied Core candidate: $1" >&2
      exit 2
      ;;
  esac
}

copy_platform_payload() {
  local platform="$1"
  local source_dir
  local target_dir
  source_dir="$(platform_source_dir "${platform}")"
  target_dir="${STAGE_MODULE}/native/${platform}"
  mkdir -p "${target_dir}"
  local versioned="${source_dir}/libzlink.so.${CORE_VERSION}"
  local major="${source_dir}/libzlink.so.${CORE_VERSION%%.*}"
  local linker="${source_dir}/libzlink.so"
  for file in "${linker}" "${major}" "${versioned}"; do
    [[ -f "${file}" ]] || {
      echo "Missing Go package runtime: ${file}" >&2
      exit 1
    }
    [[ "$(sha256sum "${file}" | awk '{print $1}')" == "${CORE_RUNTIME_SHA256}" ]] || {
      echo "Go package runtime is not the approved Core candidate: ${file}" >&2
      exit 1
    }
  done
  cp -L "${CORE_RUNTIME_SOURCE}" "${target_dir}/libzlink.so"
  cp -L "${CORE_RUNTIME_SOURCE}" "${target_dir}/libzlink.so.${CORE_VERSION%%.*}"
  cp -L "${CORE_RUNTIME_SOURCE}" "${target_dir}/libzlink.so.${CORE_VERSION}"
}

STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/zlink-go-package.XXXXXX")"
CONSUMER_ROOT=""
cleanup() {
  local status=$?
  if [[ -n "${CONSUMER_ROOT}" && -d "${CONSUMER_ROOT}" ]]; then
    chmod -R u+w -- "${CONSUMER_ROOT}" 2>/dev/null || true
    rm -rf -- "${CONSUMER_ROOT:?}"
  fi
  if [[ -d "${STAGE_ROOT}" ]]; then
    chmod -R u+w -- "${STAGE_ROOT}" 2>/dev/null || true
    rm -rf -- "${STAGE_ROOT:?}"
  fi
  exit "${status}"
}
trap cleanup EXIT

STAGE_MODULE="${STAGE_ROOT}/${MODULE_PATH}@${PACKAGE_VERSION}"
SOURCE_SNAPSHOT="${STAGE_ROOT}/source"
mkdir -p "${SOURCE_SNAPSHOT}"
git -C "${REPO_ROOT}" archive --format=tar HEAD -- bindings/go | tar -x -C "${SOURCE_SNAPSHOT}"
SNAPSHOT_BINDING_ROOT="${SOURCE_SNAPSHOT}/bindings/go"
SNAPSHOT_MODULE_PATH="$(sed -n 's/^module //p' "${SNAPSHOT_BINDING_ROOT}/go.mod" | head -n1)"
SNAPSHOT_CORE_VERSION="$(sed -n 's/^#define ZLINK_VERSION_MAJOR //p' "${SNAPSHOT_BINDING_ROOT}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_MINOR //p' "${SNAPSHOT_BINDING_ROOT}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_PATCH //p' "${SNAPSHOT_BINDING_ROOT}/include/zlink.h" | head -n1)"
[[ "${SNAPSHOT_MODULE_PATH}" == "${MODULE_PATH}" ]] || {
  echo "Go module path changed between worktree and source snapshot" >&2
  exit 1
}
[[ "${SNAPSHOT_CORE_VERSION}" == "${CORE_VERSION}" ]] || {
  echo "Go header version changed between worktree and source snapshot" >&2
  exit 1
}
mkdir -p "${STAGE_MODULE}"
cp -a "${SNAPSHOT_BINDING_ROOT}/." "${STAGE_MODULE}/"
rm -rf -- "${STAGE_MODULE:?}/native"
mkdir -p "${STAGE_MODULE}/native"

IFS=',' read -r -a PLATFORM_LIST <<< "${PLATFORMS}"
for platform in "${PLATFORM_LIST[@]}"; do
  [[ -n "${platform}" ]] || { echo "--platforms contains an empty entry" >&2; exit 2; }
  copy_platform_payload "${platform}"
done

if find "${STAGE_MODULE}" -type f \( -path '*/zlink/service/*' -o -name '*service*.h' \) -print -quit | grep -q .; then
  echo "Service header found in Go module stage" >&2
  exit 1
fi
if find "${STAGE_MODULE}/native" -type f \( -name 'libzlink.so.9*' -o -name 'libzlink.so.10*' \) -print -quit | grep -q .; then
  echo "Old Core runtime found in Go module stage" >&2
  exit 1
fi

PROXY_ROOT="${OUTPUT_ROOT}/proxy"
VERSION_ROOT="${PROXY_ROOT}/${MODULE_PATH}/@v"
MODULE_ZIP="${VERSION_ROOT}/${PACKAGE_VERSION}.zip"
MODULE_ZIP_TMP="${STAGE_ROOT}/module.zip"
mkdir -p "${VERSION_ROOT}"
(
  cd "${STAGE_ROOT}"
  zip -q -r -X "${MODULE_ZIP_TMP}" "${MODULE_PATH}@${PACKAGE_VERSION}"
)
mv -- "${MODULE_ZIP_TMP}" "${MODULE_ZIP}"
cp "${STAGE_MODULE}/go.mod" "${VERSION_ROOT}/${PACKAGE_VERSION}.mod"
printf '{"Version":"%s","Time":"%s"}\n' \
  "${PACKAGE_VERSION}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  > "${VERSION_ROOT}/${PACKAGE_VERSION}.info"

CONSUMER_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/zlink-go-consumer.XXXXXX")"
MODCACHE="${CONSUMER_ROOT}/gomodcache"
GOCACHE_DIR="${CONSUMER_ROOT}/gocache"
cat > "${CONSUMER_ROOT}/go.mod" <<EOF
module zlink-go-clean-consumer

go 1.25.12

require ${MODULE_PATH} ${PACKAGE_VERSION}
EOF
cat > "${CONSUMER_ROOT}/main.go" <<'EOF'
package main

import (
	"fmt"
	"os"

	zlink "zlink.systems/zlink/v11"
)

func main() {
	version := zlink.RuntimeVersion()
	if version.Major != 11 || version.Minor != 1 || version.Patch != 0 {
		panic(fmt.Sprintf("unexpected runtime version: %+v", version))
	}

	ctx, err := zlink.NewContext()
	if err != nil {
		panic(err)
	}
	defer ctx.Close()
	left, err := ctx.PairSocket()
	if err != nil {
		panic(err)
	}
	defer left.Close()
	right, err := ctx.PairSocket()
	if err != nil {
		panic(err)
	}
	defer right.Close()
	endpoint := "inproc://go-clean-consumer"
	if err := left.Bind(endpoint); err != nil {
		panic(err)
	}
	if err := right.Connect(endpoint); err != nil {
		panic(err)
	}
	message, err := zlink.NewMessageString("clean-consumer")
	if err != nil {
		panic(err)
	}
	if _, err := right.Send().Message(message).Submit(nil); err != nil {
		panic(err)
	}
	var received zlink.Received
	if _, err := left.Recv(&received, zlink.RecvFlagsNone); err != nil {
		panic(err)
	}
	defer received.Close()
	part, err := received.SinglePartOrError()
	if err != nil || string(part.Data()) != "clean-consumer" {
		panic(fmt.Sprintf("unexpected payload: %v", err))
	}
	fmt.Fprintf(os.Stdout, "%d.%d.%d clean-consumer-ok\n", version.Major, version.Minor, version.Patch)
}
EOF

export GOPROXY="file://${PROXY_ROOT},off"
export GOSUMDB=off
export GOMODCACHE="${MODCACHE}"
export GOCACHE="${GOCACHE_DIR}"
(cd "${CONSUMER_ROOT}" && \
  env -u LD_LIBRARY_PATH go mod download "${MODULE_PATH}@${PACKAGE_VERSION}" >/dev/null && \
  env -u LD_LIBRARY_PATH go build -o "${CONSUMER_ROOT}/consumer" .)

if [[ "$(uname -s)" == Linux* ]]; then
  expected_cache_dir="${MODCACHE}/${MODULE_PATH}@${PACKAGE_VERSION}/native/linux-x86_64"
  if printf '%s\n' "${PLATFORMS}" | tr ',' '\n' | grep -qx 'linux-x86_64'; then
    ldd_output="$(ldd "${CONSUMER_ROOT}/consumer")"
    printf '%s\n' "${ldd_output}"
    resolved_runtime="$(printf '%s\n' "${ldd_output}" | sed -n 's/^[[:space:]]*libzlink\.so\.11 => \([^[:space:]]*\).*/\1/p')"
    [[ -n "${resolved_runtime}" ]] || {
      echo "Clean consumer did not resolve libzlink.so.11" >&2
      exit 1
    }
    [[ "$(realpath "${resolved_runtime}")" == "$(realpath "${expected_cache_dir}/libzlink.so.11")" ]] || {
      echo "Clean consumer resolved outside the module cache: ${resolved_runtime}" >&2
      exit 1
    }
  fi
fi
"${CONSUMER_ROOT}/consumer"

mkdir -p "${OUTPUT_ROOT}"
ZIP_SHA256="$(sha256sum "${MODULE_ZIP}" | awk '{print $1}')"
HEADER_SHA256="$(dir_hash "${STAGE_MODULE}/include")"
SOURCE_SHA256="$(dir_hash "${STAGE_MODULE}")"
EVIDENCE="${OUTPUT_ROOT}/go-package-${PACKAGE_VERSION}.json"
SOURCE_REVISION="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
SOURCE_MANIFEST="${OUTPUT_ROOT}/go-source-manifest-${PACKAGE_VERSION}.json"
PACKAGE_SCRIPT_SHA256="$(sha256sum "${SCRIPT_DIR}/build-wsl.sh" | awk '{print $1}')"
MODULE_PATH="${MODULE_PATH}" PACKAGE_VERSION="${PACKAGE_VERSION}" SOURCE_REVISION="${SOURCE_REVISION}" STAGE_MODULE="${STAGE_MODULE}" SOURCE_MANIFEST="${SOURCE_MANIFEST}" node <<'NODE'
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

function filesUnder(root, relative = '') {
  const current = path.join(root, relative);
  return fs.readdirSync(current, {withFileTypes: true}).sort((a, b) => a.name.localeCompare(b.name)).flatMap(entry => {
    const entryRelative = path.join(relative, entry.name);
    if (entry.isDirectory()) return filesUnder(root, entryRelative);
    const content = fs.readFileSync(path.join(root, entryRelative));
    return [{
      path: entryRelative.split(path.sep).join('/'),
      sha256: crypto.createHash('sha256').update(content).digest('hex'),
      mode: fs.statSync(path.join(root, entryRelative)).mode & 0o777,
    }];
  });
}

const manifest = {
  schema: 1,
  module: process.env.MODULE_PATH,
  version: process.env.PACKAGE_VERSION,
  sourceRevision: process.env.SOURCE_REVISION,
  files: filesUnder(process.env.STAGE_MODULE),
};
fs.writeFileSync(process.env.SOURCE_MANIFEST, JSON.stringify(manifest, null, 2) + '\n');
NODE
SOURCE_MANIFEST_SHA256="$(sha256sum "${SOURCE_MANIFEST}" | awk '{print $1}')"
MODULE_ZIP="${MODULE_ZIP}" MODULE_ZIP_SHA256="${ZIP_SHA256}" MODULE_PATH="${MODULE_PATH}" PACKAGE_VERSION="${PACKAGE_VERSION}" PLATFORMS="${PLATFORMS}" HEADER_SHA256="${HEADER_SHA256}" SOURCE_SHA256="${SOURCE_SHA256}" SOURCE_REVISION="${SOURCE_REVISION}" SOURCE_MANIFEST="${SOURCE_MANIFEST}" SOURCE_MANIFEST_SHA256="${SOURCE_MANIFEST_SHA256}" PACKAGE_SCRIPT="${SCRIPT_DIR}/build-wsl.sh" PACKAGE_SCRIPT_SHA256="${PACKAGE_SCRIPT_SHA256}" EVIDENCE="${EVIDENCE}" CORE_CANDIDATE_MANIFEST="${CORE_CANDIDATE_MANIFEST}" CORE_PACKAGE_EVIDENCE="${CORE_PACKAGE_EVIDENCE}" CORE_CANDIDATE_MANIFEST_SHA256="${CORE_CANDIDATE_MANIFEST_SHA256}" CORE_CANDIDATE_AGGREGATE_SHA256="${CORE_CANDIDATE_AGGREGATE_SHA256}" CORE_APPROVAL_EVIDENCE_SHA256="${CORE_APPROVAL_EVIDENCE_SHA256}" CORE_PROVENANCE_PATH="${CORE_PROVENANCE_PATH}" CORE_PROVENANCE_SHA256="${CORE_PROVENANCE_SHA256}" CORE_RUNTIME_SOURCE="${CORE_RUNTIME_SOURCE}" CORE_RUNTIME_SHA256="${CORE_RUNTIME_SHA256}" node <<'NODE'
const fs = require('fs');
const record = {
  format: 1,
  module: process.env.MODULE_PATH,
  version: process.env.PACKAGE_VERSION,
  coreCandidate: {
    candidateManifest: process.env.CORE_CANDIDATE_MANIFEST,
    candidateManifestSha256: process.env.CORE_CANDIDATE_MANIFEST_SHA256,
    candidateAggregateSha256: process.env.CORE_CANDIDATE_AGGREGATE_SHA256,
    packageEvidence: process.env.CORE_PACKAGE_EVIDENCE,
    approvalEvidenceSha256: process.env.CORE_APPROVAL_EVIDENCE_SHA256,
    provenanceManifest: process.env.CORE_PROVENANCE_PATH,
    provenanceSha256: process.env.CORE_PROVENANCE_SHA256,
    runtime: process.env.CORE_RUNTIME_SOURCE,
    runtimeSha256: process.env.CORE_RUNTIME_SHA256,
  },
  sourceRevision: process.env.SOURCE_REVISION,
  sourceManifest: process.env.SOURCE_MANIFEST,
  sourceManifestSha256: process.env.SOURCE_MANIFEST_SHA256,
  packageScript: process.env.PACKAGE_SCRIPT,
  packageScriptSha256: process.env.PACKAGE_SCRIPT_SHA256,
  moduleZip: process.env.MODULE_ZIP,
  moduleZipSha256: process.env.MODULE_ZIP_SHA256,
  platforms: process.env.PLATFORMS.split(','),
  headerSha256: process.env.HEADER_SHA256,
  sourceSha256: process.env.SOURCE_SHA256,
  cleanConsumer: 'pass',
};
fs.writeFileSync(process.env.EVIDENCE, JSON.stringify(record, null, 2) + '\n');
NODE

echo "module=${MODULE_PATH}"
echo "version=${PACKAGE_VERSION}"
echo "zip=${MODULE_ZIP}"
echo "zip_sha256=${ZIP_SHA256}"
echo "platforms=${PLATFORMS}"
echo "evidence=${EVIDENCE}"
