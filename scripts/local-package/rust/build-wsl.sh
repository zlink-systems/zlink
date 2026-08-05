#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
BINDING_ROOT="${REPO_ROOT}/bindings/rust"
PACKAGE_VERSION="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "${BINDING_ROOT}/Cargo.toml" | head -n1)"
CORE_VERSION="$(sed -n 's/^#define ZLINK_VERSION_MAJOR //p' "${BINDING_ROOT}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_MINOR //p' "${BINDING_ROOT}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_PATCH //p' "${BINDING_ROOT}/include/zlink.h" | head -n1)"
PLATFORMS="linux-x86_64"
OUTPUT_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-${REPO_ROOT}/.artifacts/wsl}/rust"
CORE_CANDIDATE_MANIFEST=""
CORE_PACKAGE_EVIDENCE=""

usage() {
  cat <<'EOF'
Usage: scripts/local-package/rust/build-wsl.sh [options]

Options:
  --package-version VERSION  Rust crate version. Defaults to Cargo.toml.
  --platforms LIST           Comma-separated native payload directories.
                             Current candidate support: linux-x86_64.
  --core-candidate-manifest FILE
                             Approved V11-M3-CORE-VERIFY candidate manifest.
  --core-package-evidence FILE
                             Matching V11-M3-CORE-PKG pass evidence.
  --output-root DIR          Absolute output directory.
  -h, --help

The command packages a committed Rust source snapshot with the approved Core
runtime, verifies cargo package contents, runs source tests and clippy, and
builds a clean consumer from a temporary sparse registry. It never discovers
or uses repository core/build or an existing bindings/rust/native payload.
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
[[ "${PACKAGE_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "--package-version must use MAJOR.MINOR.PATCH" >&2
  exit 2
}
[[ "${PACKAGE_VERSION}" == "${CORE_VERSION}" ]] || {
  echo "Rust package version ${PACKAGE_VERSION} must match Core ${CORE_VERSION}" >&2
  exit 1
}

if ! git -C "${REPO_ROOT}" diff --quiet -- bindings/rust ':(exclude)bindings/rust/native/**' || \
   ! git -C "${REPO_ROOT}" diff --cached --quiet -- bindings/rust ':(exclude)bindings/rust/native/**'; then
  echo "Rust package source must be committed before package materialization" >&2
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
  echo "Rust package headers do not match the approved Core package" >&2
  exit 1
}

platform_source_dir() {
  case "$1" in
    linux-x86_64)
      printf '%s\n' "$1"
      ;;
    *)
      echo "Rust package platform is not present in the supplied Core candidate: $1" >&2
      exit 2
      ;;
  esac
}

STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/zlink-rust-package.XXXXXX")"
CONSUMER_ROOT=""
REGISTRY_ROOT=""
REGISTRY_PID=""
OUTPUT_ROOT="$(realpath -m -- "${OUTPUT_ROOT}")"
mkdir -p "${OUTPUT_ROOT}"

cleanup() {
  local status=$?
  if [[ -n "${REGISTRY_PID}" ]] && kill -0 "${REGISTRY_PID}" 2>/dev/null; then
    kill "${REGISTRY_PID}" 2>/dev/null || true
    wait "${REGISTRY_PID}" 2>/dev/null || true
  fi
  if [[ -n "${CONSUMER_ROOT}" && -d "${CONSUMER_ROOT}" ]]; then
    chmod -R u+w -- "${CONSUMER_ROOT}" 2>/dev/null || true
    rm -rf -- "${CONSUMER_ROOT:?}"
  fi
  if [[ -d "${STAGE_ROOT}" ]]; then
    chmod -R u+w -- "${STAGE_ROOT:?}" 2>/dev/null || true
    rm -rf -- "${STAGE_ROOT:?}"
  fi
  exit "${status}"
}
trap cleanup EXIT

SOURCE_SNAPSHOT="${STAGE_ROOT}/source"
STAGE_CRATE="${STAGE_ROOT}/crate"
mkdir -p "${SOURCE_SNAPSHOT}"
git -C "${REPO_ROOT}" archive --format=tar HEAD -- bindings/rust | tar -x -C "${SOURCE_SNAPSHOT}"
SNAPSHOT_BINDING_ROOT="${SOURCE_SNAPSHOT}/bindings/rust"
mkdir -p "${STAGE_CRATE}"
cp -a "${SNAPSHOT_BINDING_ROOT}/." "${STAGE_CRATE}/"

SNAPSHOT_VERSION="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "${STAGE_CRATE}/Cargo.toml" | head -n1)"
SNAPSHOT_CORE_VERSION="$(sed -n 's/^#define ZLINK_VERSION_MAJOR //p' "${STAGE_CRATE}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_MINOR //p' "${STAGE_CRATE}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_PATCH //p' "${STAGE_CRATE}/include/zlink.h" | head -n1)"
[[ "${SNAPSHOT_VERSION}" == "${PACKAGE_VERSION}" ]] || {
  echo "Rust crate version changed between worktree and source snapshot" >&2
  exit 1
}
[[ "${SNAPSHOT_CORE_VERSION}" == "${CORE_VERSION}" ]] || {
  echo "Rust header version changed between worktree and source snapshot" >&2
  exit 1
}

rm -rf -- "${STAGE_CRATE:?}/native"
mkdir -p "${STAGE_CRATE}/native/linux-x86_64"
for name in libzlink.so libzlink.so.11 "libzlink.so.${CORE_VERSION}"; do
  cp -L "${CORE_RUNTIME_SOURCE}" "${STAGE_CRATE}/native/linux-x86_64/${name}"
  [[ "$(sha256sum "${STAGE_CRATE}/native/linux-x86_64/${name}" | awk '{print $1}')" == "${CORE_RUNTIME_SHA256}" ]] || {
    echo "Staged Rust runtime is not the approved Core candidate: ${name}" >&2
    exit 1
  }
done

IFS=',' read -r -a PLATFORM_LIST <<< "${PLATFORMS}"
for platform in "${PLATFORM_LIST[@]}"; do
  [[ -n "${platform}" ]] || { echo "--platforms contains an empty entry" >&2; exit 2; }
  platform_source_dir "${platform}" >/dev/null
  [[ "${platform}" == "linux-x86_64" ]] || exit 2
done

if find "${STAGE_CRATE}" -type f \( -path '*/service/*' -o -path '*/spot/*' -o -iname '*actor*' \) -print -quit | grep -q .; then
  echo "Service, Spot, or Actor path found in Rust crate stage" >&2
  exit 1
fi
if rg -n --glob '!README.rustdoc.md' \
  'zlink_service|SpotNode|ActorModel|pub mod (service|spot)|crate::(service|spot)|zlink::(service|spot)|libzlink_c|libzlink\.so\.(9|10)' \
  "${STAGE_CRATE}/src" "${STAGE_CRATE}/samples" "${STAGE_CRATE}/perf" "${STAGE_CRATE}/Cargo.toml"; then
  echo "Legacy service or pre-Core-11 symbol found in Rust crate stage" >&2
  exit 1
fi

CRATE_TARGET="${STAGE_ROOT}/cargo-target"
PACKAGE_LOG="${OUTPUT_ROOT}/rust-package-${PACKAGE_VERSION}.log"
TEST_LOG="${OUTPUT_ROOT}/rust-tests-${PACKAGE_VERSION}.log"
CLIPPY_LOG="${OUTPUT_ROOT}/rust-clippy-${PACKAGE_VERSION}.log"
SAMPLE_LOG="${OUTPUT_ROOT}/rust-samples-${PACKAGE_VERSION}.log"
VENDOR_ROOT="${STAGE_ROOT}/vendor"
VENDOR_LOG="${OUTPUT_ROOT}/rust-vendor-${PACKAGE_VERSION}.log"
mkdir -p "${OUTPUT_ROOT}/logs"

(
  cd "${STAGE_CRATE}"
  CARGO_TARGET_DIR="${CRATE_TARGET}" cargo package --locked --allow-dirty --manifest-path "${STAGE_CRATE}/Cargo.toml"
) >"${PACKAGE_LOG}" 2>&1 || {
  tail -n 160 "${PACKAGE_LOG}" >&2
  exit 1
}

CRATE_ARCHIVE="${CRATE_TARGET}/package/zlink-${PACKAGE_VERSION}.crate"
[[ -f "${CRATE_ARCHIVE}" ]] || {
  echo "cargo package did not produce ${CRATE_ARCHIVE}" >&2
  exit 1
}
mapfile -t ARCHIVE_ENTRIES < <(tar -tzf "${CRATE_ARCHIVE}")
printf '%s\n' "${ARCHIVE_ENTRIES[@]}" > "${OUTPUT_ROOT}/rust-package-contents-${PACKAGE_VERSION}.txt"
CRATE_ARCHIVE_PREFIX="zlink-${PACKAGE_VERSION}/"
for archive_entry in "${ARCHIVE_ENTRIES[@]}"; do
  case "${archive_entry}" in
    "${CRATE_ARCHIVE_PREFIX}native/"|"${CRATE_ARCHIVE_PREFIX}native/linux-x86_64/"*) ;;
    "${CRATE_ARCHIVE_PREFIX}native/"*)
      echo "Rust crate contains an unsupported native platform: ${archive_entry}" >&2
      exit 1
      ;;
  esac
done
if printf '%s\n' "${ARCHIVE_ENTRIES[@]}" | rg -q '/(service|spot|actor)|libzlink_c|libzlink\.so\.(9|10)' ; then
  echo "Rust crate contains a removed API path or old runtime" >&2
  exit 1
fi
for name in libzlink.so libzlink.so.11 "libzlink.so.${CORE_VERSION}"; do
  printf '%s\n' "${ARCHIVE_ENTRIES[@]}" | grep -Fq "/native/linux-x86_64/${name}" || {
    echo "Rust crate is missing native/linux-x86_64/${name}" >&2
    exit 1
  }
done

(
  cd "${STAGE_CRATE}"
  export CARGO_TARGET_DIR="${STAGE_ROOT}/test-target"
  export LD_LIBRARY_PATH="${STAGE_CRATE}/native/linux-x86_64"
  cargo test --locked --workspace --all-targets -- --test-threads=1
) >"${TEST_LOG}" 2>&1 || {
  tail -n 220 "${TEST_LOG}" >&2
  exit 1
}
(
  cd "${STAGE_CRATE}"
  export CARGO_TARGET_DIR="${STAGE_ROOT}/clippy-target"
  export LD_LIBRARY_PATH="${STAGE_CRATE}/native/linux-x86_64"
  cargo clippy --locked --all-targets -- -D warnings
) >"${CLIPPY_LOG}" 2>&1 || {
  tail -n 220 "${CLIPPY_LOG}" >&2
  exit 1
}
(
  cd "${STAGE_CRATE}"
  export CARGO_TARGET_DIR="${STAGE_ROOT}/sample-target"
  export LD_LIBRARY_PATH="${STAGE_CRATE}/native/linux-x86_64"
  ./samples/run_samples.sh
) >"${SAMPLE_LOG}" 2>&1 || {
  tail -n 220 "${SAMPLE_LOG}" >&2
  exit 1
}
(
  cd "${STAGE_CRATE}"
  cargo vendor --locked --versioned-dirs "${VENDOR_ROOT}"
) >"${VENDOR_LOG}" 2>&1 || {
  tail -n 220 "${VENDOR_LOG}" >&2
  exit 1
}

REGISTRY_ROOT="${STAGE_ROOT}/registry"
REGISTRY_CRATES="${REGISTRY_ROOT}/crates"
mkdir -p "${REGISTRY_CRATES}"
cp -L "${CRATE_ARCHIVE}" "${REGISTRY_CRATES}/zlink-${PACKAGE_VERSION}.crate"
CRATE_SHA256="$(sha256sum "${CRATE_ARCHIVE}" | awk '{print $1}')"
REGISTRY_PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
mkdir -p "${REGISTRY_ROOT}/zl/in"
REGISTRY_PORT="${REGISTRY_PORT}" REGISTRY_ROOT="${REGISTRY_ROOT}" PACKAGE_VERSION="${PACKAGE_VERSION}" CRATE_SHA256="${CRATE_SHA256}" node <<'NODE'
const fs = require('fs');
const path = require('path');

const root = process.env.REGISTRY_ROOT;
const port = process.env.REGISTRY_PORT;
const record = {
  name: 'zlink',
  vers: process.env.PACKAGE_VERSION,
  deps: [
    {name: 'libc', req: '^0.2', features: [], optional: false, default_features: true, target: null, kind: 'normal', registry: 'https://github.com/rust-lang/crates.io-index'},
    {name: 'smol_str', req: '^0.3', features: [], optional: false, default_features: true, target: null, kind: 'normal', registry: 'https://github.com/rust-lang/crates.io-index'},
  ],
  cksum: process.env.CRATE_SHA256,
  features: {},
  yanked: false,
  links: null,
  v: 1,
};
fs.writeFileSync(path.join(root, 'config.json'), JSON.stringify({
  dl: `http://127.0.0.1:${port}/crates/{crate}-{version}.crate`,
}) + '\n');
fs.writeFileSync(path.join(root, 'zl/in/zlink'), JSON.stringify(record) + '\n');
NODE

python3 -m http.server "${REGISTRY_PORT}" --bind 127.0.0.1 --directory "${REGISTRY_ROOT}" \
  >"${OUTPUT_ROOT}/rust-registry-${PACKAGE_VERSION}.log" 2>&1 &
REGISTRY_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "${REGISTRY_PID}" 2>/dev/null; then
    cat "${OUTPUT_ROOT}/rust-registry-${PACKAGE_VERSION}.log" >&2
    exit 1
  fi
  if curl --fail --silent "http://127.0.0.1:${REGISTRY_PORT}/config.json" >/dev/null; then
    break
  fi
  sleep 0.1
done
curl --fail --silent "http://127.0.0.1:${REGISTRY_PORT}/zl/in/zlink" >/dev/null

CONSUMER_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/zlink-rust-consumer.XXXXXX")"
mkdir -p "${CONSUMER_ROOT}/.cargo" "${CONSUMER_ROOT}/cargo-home" "${CONSUMER_ROOT}/src"
REGISTRY_PORT="${REGISTRY_PORT}" CONSUMER_ROOT="${CONSUMER_ROOT}" PACKAGE_VERSION="${PACKAGE_VERSION}" VENDOR_ROOT="${VENDOR_ROOT}" node <<'NODE'
const fs = require('fs');
const path = require('path');

const root = process.env.CONSUMER_ROOT;
fs.writeFileSync(path.join(root, 'Cargo.toml'), `[package]
name = "zlink-rust-clean-consumer"
version = "0.1.0"
edition = "2024"

[dependencies]
zlink = { version = "${process.env.PACKAGE_VERSION}", registry = "candidate" }
`);
fs.writeFileSync(path.join(root, '.cargo/config.toml'), `[registries.candidate]
index = "sparse+http://127.0.0.1:${process.env.REGISTRY_PORT}/"

[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "${process.env.VENDOR_ROOT}"
`);
fs.writeFileSync(path.join(root, 'src/main.rs'), `use zlink::{Context, Message, Received, RecvFlags};

fn main() {
    let expected_version = "${process.env.PACKAGE_VERSION}";
    let expected = expected_version
        .split('.')
        .map(|part| part.parse::<i32>().expect("numeric package version"))
        .collect::<Vec<_>>();
    assert_eq!(expected.len(), 3);
    assert_eq!(zlink::version(), (expected[0], expected[1], expected[2]));
    let context = Context::new().expect("context creation failed");
    let receiver = context.pair_socket().expect("receiver creation failed");
    let sender = context.pair_socket().expect("sender creation failed");
    let endpoint = "inproc://rust-clean-consumer";
    receiver.bind(endpoint).expect("bind failed");
    sender.connect(endpoint).expect("connect failed");
    let message = Message::try_from(b"rust-clean-consumer").expect("message creation failed");
    sender.send().message(message).submit().expect("send failed");
    let mut received = Received::empty();
    receiver.recv(&mut received, RecvFlags::NONE).expect("recv failed");
    assert_eq!(received.parts()[0].as_str().expect("utf8 error"), "rust-clean-consumer");
    println!("{} rust-clean-consumer-ok", expected_version);
}
`);
NODE

CONSUMER_TARGET="${CONSUMER_ROOT}/target"
(
  cd "${CONSUMER_ROOT}"
  env -u LD_LIBRARY_PATH -u CARGO_NET_OFFLINE CARGO_HOME="${CONSUMER_ROOT}/cargo-home" \
    CARGO_TARGET_DIR="${CONSUMER_TARGET}" \
    cargo generate-lockfile
  env -u LD_LIBRARY_PATH -u CARGO_NET_OFFLINE CARGO_HOME="${CONSUMER_ROOT}/cargo-home" \
    CARGO_TARGET_DIR="${CONSUMER_TARGET}" \
    cargo fetch --locked
  env -u LD_LIBRARY_PATH -u CARGO_NET_OFFLINE CARGO_HOME="${CONSUMER_ROOT}/cargo-home" \
    CARGO_TARGET_DIR="${CONSUMER_TARGET}" \
    cargo metadata --locked --no-deps --format-version 1 >/dev/null
) >"${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.log" 2>&1 || {
  tail -n 220 "${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.log" >&2
  exit 1
}

mapfile -t CONSUMER_PACKAGE_ROOTS < <(
  find "${CONSUMER_ROOT}/cargo-home/registry/src" -type f \
    -path "*/zlink-${PACKAGE_VERSION}/Cargo.toml" -printf '%h\n' | sort -u
)
[[ "${#CONSUMER_PACKAGE_ROOTS[@]}" -eq 1 ]] || {
  echo "Cargo did not materialize exactly one candidate zlink source" >&2
  printf '%s\n' "${CONSUMER_PACKAGE_ROOTS[@]}" >&2
  exit 1
}
CONSUMER_PACKAGE_ROOT="${CONSUMER_PACKAGE_ROOTS[0]}"
CONSUMER_RPATH="${CONSUMER_PACKAGE_ROOT}/native/linux-x86_64"
[[ -f "${CONSUMER_RPATH}/libzlink.so.11" ]] || {
  echo "Candidate Cargo source has no native/linux-x86_64/libzlink.so.11" >&2
  exit 1
}
(
  cd "${CONSUMER_ROOT}"
  env -u LD_LIBRARY_PATH -u CARGO_NET_OFFLINE CARGO_HOME="${CONSUMER_ROOT}/cargo-home" \
    CARGO_TARGET_DIR="${CONSUMER_TARGET}" \
    RUSTFLAGS="-C link-arg=-Wl,-rpath,${CONSUMER_RPATH}" \
    cargo build --locked --release
) >>"${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.log" 2>&1 || {
  tail -n 220 "${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.log" >&2
  exit 1
}

CONSUMER_BINARY="${CONSUMER_TARGET}/release/zlink-rust-clean-consumer"
[[ -x "${CONSUMER_BINARY}" ]] || {
  echo "Rust clean consumer binary was not built" >&2
  exit 1
}
if [[ "$(uname -s)" == Linux* ]]; then
  CONSUMER_LDD="$(ldd "${CONSUMER_BINARY}")"
  printf '%s\n' "${CONSUMER_LDD}" | tee "${OUTPUT_ROOT}/rust-clean-consumer-ldd-${PACKAGE_VERSION}.txt"
  RESOLVED_RUNTIME="$(printf '%s\n' "${CONSUMER_LDD}" | sed -n 's/^[[:space:]]*libzlink\.so\.11 => \([^[:space:]]*\).*/\1/p')"
  [[ -n "${RESOLVED_RUNTIME}" && "${RESOLVED_RUNTIME}" != "not" && -f "${RESOLVED_RUNTIME}" ]] || {
    echo "Rust clean consumer did not resolve libzlink.so.11" >&2
    exit 1
  }
  EXPECTED_RUNTIME="$(realpath "${RESOLVED_RUNTIME}")"
  case "${EXPECTED_RUNTIME}" in
    */zlink-${PACKAGE_VERSION}/native/linux-x86_64/libzlink.so.11) ;;
    *)
      echo "Rust clean consumer resolved outside the candidate crate: ${EXPECTED_RUNTIME}" >&2
      exit 1
      ;;
  esac
  [[ "$(sha256sum "${EXPECTED_RUNTIME}" | awk '{print $1}')" == "${CORE_RUNTIME_SHA256}" ]] || {
    echo "Rust clean consumer loaded a runtime with the wrong SHA-256" >&2
    exit 1
  }
fi
env -u LD_LIBRARY_PATH "${CONSUMER_BINARY}" | tee "${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.txt"

mkdir -p "${OUTPUT_ROOT}/crate"
cp -L "${CRATE_ARCHIVE}" "${OUTPUT_ROOT}/crate/zlink-${PACKAGE_VERSION}.crate"
CRATE_OUTPUT="${OUTPUT_ROOT}/crate/zlink-${PACKAGE_VERSION}.crate"
HEADER_SHA256="$(dir_hash "${STAGE_CRATE}/include")"
SOURCE_SHA256="$(dir_hash "${STAGE_CRATE}")"
SOURCE_REVISION="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
SOURCE_MANIFEST="${OUTPUT_ROOT}/rust-source-manifest-${PACKAGE_VERSION}.json"
PACKAGE_SCRIPT_SHA256="$(sha256sum "${SCRIPT_DIR}/build-wsl.sh" | awk '{print $1}')"
TEST_SHA256="$(sha256sum "${TEST_LOG}" | awk '{print $1}')"
CLIPPY_SHA256="$(sha256sum "${CLIPPY_LOG}" | awk '{print $1}')"
SAMPLE_SHA256="$(sha256sum "${SAMPLE_LOG}" | awk '{print $1}')"
CONSUMER_LOG_SHA256="$(sha256sum "${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.log" | awk '{print $1}')"
EVIDENCE="${OUTPUT_ROOT}/rust-package-${PACKAGE_VERSION}.json"
PACKAGE_VERSION="${PACKAGE_VERSION}" SOURCE_REVISION="${SOURCE_REVISION}" STAGE_CRATE="${STAGE_CRATE}" SOURCE_MANIFEST="${SOURCE_MANIFEST}" node <<'NODE'
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

function filesUnder(root, relative = '') {
  const current = path.join(root, relative);
  return fs.readdirSync(current, {withFileTypes: true}).sort((a, b) => a.name.localeCompare(b.name)).flatMap(entry => {
    const entryRelative = path.join(relative, entry.name);
    if (entry.isDirectory()) return filesUnder(root, entryRelative);
    const file = path.join(root, entryRelative);
    return [{
      path: entryRelative.split(path.sep).join('/'),
      sha256: crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'),
      mode: fs.statSync(file).mode & 0o777,
    }];
  });
}

fs.writeFileSync(process.env.SOURCE_MANIFEST, JSON.stringify({
  schema: 1,
  crate: 'zlink',
  version: process.env.PACKAGE_VERSION,
  sourceRevision: process.env.SOURCE_REVISION,
  files: filesUnder(process.env.STAGE_CRATE),
}, null, 2) + '\n');
NODE
SOURCE_MANIFEST_SHA256="$(sha256sum "${SOURCE_MANIFEST}" | awk '{print $1}')"
EVIDENCE="${EVIDENCE}" CRATE_OUTPUT="${CRATE_OUTPUT}" CRATE_SHA256="$(sha256sum "${CRATE_OUTPUT}" | awk '{print $1}')" \
PACKAGE_VERSION="${PACKAGE_VERSION}" PLATFORMS="${PLATFORMS}" HEADER_SHA256="${HEADER_SHA256}" SOURCE_SHA256="${SOURCE_SHA256}" \
SOURCE_REVISION="${SOURCE_REVISION}" SOURCE_MANIFEST="${SOURCE_MANIFEST}" SOURCE_MANIFEST_SHA256="${SOURCE_MANIFEST_SHA256}" \
PACKAGE_SCRIPT="${SCRIPT_DIR}/build-wsl.sh" PACKAGE_SCRIPT_SHA256="${PACKAGE_SCRIPT_SHA256}" \
CORE_CANDIDATE_MANIFEST="${CORE_CANDIDATE_MANIFEST}" CORE_PACKAGE_EVIDENCE="${CORE_PACKAGE_EVIDENCE}" \
CORE_CANDIDATE_MANIFEST_SHA256="${CORE_CANDIDATE_MANIFEST_SHA256}" CORE_CANDIDATE_AGGREGATE_SHA256="${CORE_CANDIDATE_AGGREGATE_SHA256}" \
CORE_APPROVAL_EVIDENCE_SHA256="${CORE_APPROVAL_EVIDENCE_SHA256}" CORE_PROVENANCE_PATH="${CORE_PROVENANCE_PATH}" \
CORE_PROVENANCE_SHA256="${CORE_PROVENANCE_SHA256}" CORE_RUNTIME_SOURCE="${CORE_RUNTIME_SOURCE}" \
CORE_RUNTIME_SHA256="${CORE_RUNTIME_SHA256}" TEST_LOG="${TEST_LOG}" TEST_SHA256="${TEST_SHA256}" \
CLIPPY_LOG="${CLIPPY_LOG}" CLIPPY_SHA256="${CLIPPY_SHA256}" SAMPLE_LOG="${SAMPLE_LOG}" SAMPLE_SHA256="${SAMPLE_SHA256}" \
CONSUMER_LOG="${OUTPUT_ROOT}/rust-clean-consumer-${PACKAGE_VERSION}.log" CONSUMER_LOG_SHA256="${CONSUMER_LOG_SHA256}" node <<'NODE'
const fs = require('fs');
const record = {
  format: 1,
  crate: 'zlink',
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
  crate: process.env.CRATE_OUTPUT,
  crateSha256: process.env.CRATE_SHA256,
  platforms: process.env.PLATFORMS.split(','),
  headerSha256: process.env.HEADER_SHA256,
  sourceSha256: process.env.SOURCE_SHA256,
  verification: {
    cargoPackage: 'pass',
    cargoTest: {status: 'pass', log: process.env.TEST_LOG, sha256: process.env.TEST_SHA256},
    cargoClippy: {status: 'pass', log: process.env.CLIPPY_LOG, sha256: process.env.CLIPPY_SHA256},
    samples: {status: 'pass', log: process.env.SAMPLE_LOG, sha256: process.env.SAMPLE_SHA256},
  cleanConsumer: {
    status: 'pass',
    log: process.env.CONSUMER_LOG,
    sha256: process.env.CONSUMER_LOG_SHA256,
    runtimeEnv: 'LD_LIBRARY_PATH=unset',
    linkerRpath: 'package-derived-rustflags',
  },
  },
};
fs.writeFileSync(process.env.EVIDENCE, JSON.stringify(record, null, 2) + '\n');
NODE

echo "crate=zlink-${PACKAGE_VERSION}.crate"
echo "crate_sha256=$(sha256sum "${CRATE_OUTPUT}" | awk '{print $1}')"
echo "platforms=${PLATFORMS}"
echo "evidence=${EVIDENCE}"
