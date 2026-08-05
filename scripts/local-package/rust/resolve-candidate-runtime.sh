#!/usr/bin/env bash
set -euo pipefail

# Resolve the runtime recorded by a Rust package evidence file. Perf runners
# source this function so candidate identity and runtime ownership stay in one
# package-boundary module instead of being reimplemented per benchmark shape.
resolve_rust_package_runtime() {
    local evidence_path="$1"
    local platform="$2"
    local source_revision="$3"
    local repo_root
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
    local package_version
    package_version="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "${repo_root}/bindings/rust/Cargo.toml" | head -n1)"

    [[ "${evidence_path}" = /* ]] || {
        echo "Rust package evidence path must be absolute" >&2
        return 2
    }
    evidence_path="$(realpath -e -- "${evidence_path}")" || {
        echo "Rust package evidence does not exist" >&2
        return 1
    }

    local fields
    if ! fields="$(EXPECTED_PLATFORM="${platform}" EXPECTED_SOURCE_REVISION="${source_revision}" EXPECTED_PACKAGE_VERSION="${package_version}" node - "${evidence_path}" <<'NODE'
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const evidencePath = path.resolve(process.argv[2]);
const expectedPlatform = process.env.EXPECTED_PLATFORM;
const expectedSourceRevision = process.env.EXPECTED_SOURCE_REVISION;
const expectedPackageVersion = process.env.EXPECTED_PACKAGE_VERSION;

function fail(message) {
    throw new Error(message);
}

function readJson(file) {
    try {
        return JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
        fail(`cannot read Rust package evidence: ${error.message}`);
    }
}

function sha256(file) {
    return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function requireFile(file, label) {
    if (!fs.existsSync(file) || !fs.statSync(file).isFile()) fail(`${label} is missing: ${file}`);
}

requireFile(evidencePath, 'Rust package evidence');
const evidence = readJson(evidencePath);
if (evidence.format !== 1 || evidence.version !== expectedPackageVersion) {
    fail(`Rust package evidence is not a zlink ${expectedPackageVersion} record`);
}
if (path.basename(evidence.crate ?? '') !== `zlink-${expectedPackageVersion}.crate`) {
    fail(`unexpected Rust crate path: ${evidence.crate}`);
}
requireFile(path.resolve(evidence.crate), 'Rust crate');
if (evidence.sourceRevision !== expectedSourceRevision) {
    fail(`Rust package source revision mismatch: ${evidence.sourceRevision} != ${expectedSourceRevision}`);
}
if (!Array.isArray(evidence.platforms) || !evidence.platforms.includes(expectedPlatform)) {
    fail(`Rust package evidence does not cover platform ${expectedPlatform}`);
}
for (const [name, record] of Object.entries(evidence.verification ?? {})) {
    if (record && typeof record === 'object' && record.status && record.status !== 'pass') {
        fail(`Rust package verification is not a pass: ${name}`);
    }
}
const candidate = evidence.coreCandidate ?? {};
const candidateManifest = path.resolve(candidate.candidateManifest ?? '');
requireFile(candidateManifest, 'Core candidate manifest');
if (sha256(candidateManifest) !== candidate.candidateManifestSha256) {
    fail('Core candidate manifest SHA-256 mismatch');
}
const runtime = path.resolve(candidate.runtime ?? '');
requireFile(runtime, 'Core candidate runtime');
if (sha256(runtime) !== candidate.runtimeSha256) fail('Core candidate runtime SHA-256 mismatch');
if (path.basename(runtime) !== `libzlink.so.${expectedPackageVersion}`) fail(`unexpected Linux Rust runtime: ${runtime}`);
const cleanConsumer = evidence.verification?.cleanConsumer;
if (cleanConsumer?.status !== 'pass' || cleanConsumer?.runtimeEnv !== 'LD_LIBRARY_PATH=unset') {
    fail('Rust package clean-consumer evidence is incomplete');
}

process.stdout.write([
    runtime,
    candidate.runtimeSha256,
    path.dirname(runtime),
    candidate.candidateManifestSha256,
    candidate.candidateAggregateSha256,
    evidence.sourceRevision,
].join('\t'));
NODE
    )"; then
        echo "Rust package evidence validation failed: ${evidence_path}" >&2
        return 1
    fi
    [[ -n "${fields}" ]] || {
        echo "Rust package evidence returned no runtime identity: ${evidence_path}" >&2
        return 1
    }
    IFS=$'\t' read -r RUST_CANDIDATE_RUNTIME RUST_CANDIDATE_RUNTIME_SHA256 \
        RUST_CANDIDATE_NATIVE_DIR RUST_CANDIDATE_MANIFEST_SHA256 \
        RUST_CANDIDATE_AGGREGATE_SHA256 RUST_CANDIDATE_SOURCE_REVISION <<< "${fields}"
    export RUST_CANDIDATE_RUNTIME RUST_CANDIDATE_RUNTIME_SHA256 RUST_CANDIDATE_NATIVE_DIR
    export RUST_CANDIDATE_MANIFEST_SHA256 RUST_CANDIDATE_AGGREGATE_SHA256
    export RUST_CANDIDATE_SOURCE_REVISION
}
