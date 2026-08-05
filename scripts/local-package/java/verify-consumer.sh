#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
mode=""
dry_run=false
evidence=""
core_prefix=""
core_package_evidence=""
maven_repository=""
workspace=""
#  Both versions are derived, never hardcoded: the binding version from its
#  own Gradle metadata and the Core version from the repository VERSION file.
binding_version=""
core_version=""

cleanup() {
  [[ -z "$workspace" ]] || rm -rf "$workspace"
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage:
  verify-consumer.sh --self-test --dry-run --evidence ABSOLUTE_JSON
  verify-consumer.sh --actual --core-prefix ABSOLUTE_DIR \
    --core-package-evidence ABSOLUTE_JSON \
    --maven-repository ABSOLUTE_DIR --evidence ABSOLUTE_JSON

Self-test validates the package gate without publishing. Actual mode publishes
11.1.1 to an empty local Maven repository, then resolves, compiles, and runs an
isolated public-only Gradle consumer.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --self-test) mode="self-test"; shift ;;
    --actual) mode="actual"; shift ;;
    --dry-run) dry_run=true; shift ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --core-package-evidence) core_package_evidence="${2:-}"; shift 2 ;;
    --maven-repository) maven_repository="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$mode" == self-test || "$mode" == actual ]] || {
  echo "Exactly one of --self-test or --actual is required" >&2
  exit 2
}
[[ "$evidence" = /* ]] || { echo "--evidence must be an absolute path" >&2; exit 2; }

project="$repo_root/bindings/java/build.gradle"
binding_version="$(sed -n "s/^version *= *'\\(.*\\)'.*/\\1/p" "$repo_root/bindings/java/build.gradle" | head -1)"
core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
[[ -n "$binding_version" && -n "$core_version" ]] || {
  echo "could not derive binding or Core version" >&2; exit 1; }
IFS=. read -r core_major core_minor core_patch <<<"$core_version"
build_script="$script_dir/build-wsl.sh"
validator="$script_dir/verify-core-input.mjs"
fixture_dir="$script_dir/fixtures/public-consumer"
fixture_project="$fixture_dir/build.gradle"
fixture_source="$fixture_dir/src/main/java/consumer/PublicConsumer.java"

checks=()
check() {
  local name="$1"
  shift
  if "$@"; then checks+=("$name=pass"); else echo "check failed: $name" >&2; exit 1; fi
}
has_literal() { grep -Fq -- "$1" "$2"; }
has_no_match() { ! grep -Eq -- "$1" "$2"; }

static_checks() {
  check package-coordinate has_literal "group = 'systems.zlink'" "$project"
  check package-version has_literal "version = '$binding_version'" "$project"
  check exact-core-version has_literal "metadata.version != '$core_version'" "$project"
  check exact-provenance has_literal 'zlink.core.provenance-sha256' "$project"
  check exact-candidate has_literal 'zlink.core.candidate-manifest-sha256' "$project"
  check exact-soname has_literal "approved.runtime.soname != 'libzlink.so.11'" "$project"
  check no-core-source-include has_no_match 'core/(src|include|external/boost)|ZLINK_CORE_BUILD_DIR' "$project"
  check approved-core-evidence has_literal '--core-package-evidence' "$build_script"
  check package-dependency has_literal "implementation 'systems.zlink:zlink:@BINDING_VERSION@'" "$fixture_project"
  check local-maven-resolver has_literal 'ZLINK_LOCAL_MAVEN_REPOSITORY' "$fixture_project"
  check no-project-reference has_no_match 'project\(|bindings/java|sourceSets' "$fixture_project"
  check public-only-source has_no_match 'systems\.zlink\.(runtime|internal)|java\.lang\.foreign|Native[A-Z]|System\.load' "$fixture_source"
  check core-load-capability has_literal 'Zlink.version()' "$fixture_source"
  check multipart-capability has_literal 'SendOperation' "$fixture_source"
  check monitor-capability has_literal 'monitorOpen()' "$fixture_source"
  check stream-capability has_literal 'stream.onPacket' "$fixture_source"
  check ready-capability has_literal 'status.isReady()' "$fixture_source"
  check shutdown-capability has_literal 'context.shutdown()' "$fixture_source"
  check no-service-projection has_no_match 'MeshNode|Spot|ActorRef|Heartbeat|StreamSession' "$fixture_source"
}

write_evidence() {
  local status="$1"
  local result_mode="$2"
  local published="$3"
  local details_file="${4:-}"
  mkdir -p "$(dirname "$evidence")"
  CHECKS="$(printf '%s\n' "${checks[@]}")" EVIDENCE="$evidence" \
  STATUS="$status" RESULT_MODE="$result_mode" PUBLISHED="$published" \
  DETAILS_FILE="$details_file" REPOSITORY_REVISION="$(git -C "$repo_root" rev-parse HEAD)" \
  node <<'NODE'
const fs = require('node:fs');
const checks = Object.fromEntries(process.env.CHECKS.trim().split('\n').map(line => {
  const split = line.indexOf('=');
  return [line.slice(0, split), line.slice(split + 1)];
}));
const details = process.env.DETAILS_FILE
  ? JSON.parse(fs.readFileSync(process.env.DETAILS_FILE, 'utf8')) : {};
const result = {
  schema: 1,
  ledgerId: 'V11-M4-BIND-JVM',
  command: 'BIND-PKG-TEST',
  language: 'java',
  status: process.env.STATUS,
  mode: process.env.RESULT_MODE,
  completedAt: new Date().toISOString(),
  repositoryRevision: process.env.REPOSITORY_REVISION,
  packagePublished: process.env.PUBLISHED === 'true',
  externalPackagePublished: false,
  checks,
  ...details,
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE
}

static_checks

if [[ "$mode" == self-test ]]; then
  $dry_run || { echo "--dry-run is required for --self-test" >&2; exit 2; }
  workspace="$(mktemp -d)"
  prefix="$workspace/core"
  mkdir -p "$prefix/include" "$prefix/lib" "$prefix/share/zlink"
  printf '%s\n' '#pragma once' >"$prefix/include/zlink.h"
  cat >"$workspace/zlink.c" <<'EOF'
void zlink_version(int *major, int *minor, int *patch) {
  *major = 11; *minor = 0; *patch = 0;
}
EOF
  cc -fPIC -shared "$workspace/zlink.c" -Wl,-soname,libzlink.so.11 \
    -o "$prefix/lib/libzlink.so.$core_version"
  ln -s "libzlink.so.$core_version" "$prefix/lib/libzlink.so.11"
  ln -s libzlink.so.11 "$prefix/lib/libzlink.so"
  fixture_evidence="$workspace/core-package.json"
  PREFIX="$prefix" EVIDENCE="$fixture_evidence" CORE_VERSION="$core_version" node <<'NODE'
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const prefix = fs.realpathSync(process.env.PREFIX);
const runtime = fs.realpathSync(path.join(prefix, 'lib/libzlink.so'));
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const CORE_VERSION = process.env.CORE_VERSION;
const candidate = {
  ledgerId: 'V11-M3-CORE-VERIFY', baseRevision: '1'.repeat(40),
  manifestSha256: '2'.repeat(64), aggregateSha256: '3'.repeat(64),
};
const approval = {
  ledgerId: 'V11-R2', evidenceSha256: '4'.repeat(64),
  candidateManifestSha256: candidate.manifestSha256,
};
const provenance = {
  schema: 1, package: 'zlink-core', version: CORE_VERSION,
  candidate: {...candidate, approvalEvidenceSha256: approval.evidenceSha256},
  createdAt: new Date().toISOString(),
  files: [{path: `lib/libzlink.so.${CORE_VERSION}`, sha256: digest(runtime)}],
};
const provenancePath = path.join(prefix, 'share/zlink/core-package-provenance.json');
fs.writeFileSync(provenancePath, `${JSON.stringify(provenance, null, 2)}\n`);
const provenanceSha = digest(provenancePath);
const runtimeInfo = {path: runtime, sha256: digest(runtime), version: CORE_VERSION, soname: 'libzlink.so.11'};
const evidence = {
  schema: 1, ledgerId: 'V11-M3-CORE-PKG', command: 'CORE-PKG', status: 'pass',
  version: CORE_VERSION, candidate, approval,
  output: {prefix, provenanceManifest: provenancePath, provenanceSha256: provenanceSha},
  consumer: {candidate, approval, provenance: {path: provenancePath, sha256: provenanceSha}, runtime: runtimeInfo},
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(evidence, null, 2)}\n`);
NODE
  node "$validator" --prefix "$prefix" --core-package-evidence "$fixture_evidence" >/dev/null
  cp "$fixture_evidence" "$workspace/core-package.saved"
  CORE_VERSION="$core_version" node - "$fixture_evidence" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, 'utf8'));
value.output.provenanceSha256 = '0'.repeat(64);
fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
NODE
  if node "$validator" --prefix "$prefix" --core-package-evidence "$fixture_evidence" >/dev/null 2>&1; then
    echo "Core validator accepted a different provenance SHA-256" >&2; exit 1
  fi
  mv "$workspace/core-package.saved" "$fixture_evidence"
  cp "$fixture_evidence" "$workspace/core-package.saved"
  CORE_VERSION="$core_version" node - "$fixture_evidence" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, 'utf8'));
value.candidate.aggregateSha256 = '0'.repeat(64);
fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
NODE
  if node "$validator" --prefix "$prefix" --core-package-evidence "$fixture_evidence" >/dev/null 2>&1; then
    echo "Core validator accepted a different candidate identity" >&2; exit 1
  fi
  mv "$workspace/core-package.saved" "$fixture_evidence"
  cp "$fixture_evidence" "$workspace/core-package.saved"
  CORE_VERSION="$core_version" node - "$fixture_evidence" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, 'utf8'));
//  Deliberately wrong: the validator must reject a Core version that does not
//  match the repository VERSION. Bump the minor so this stays wrong at any version.
const [major, minor, patch] = process.env.CORE_VERSION.split('.');
value.version = `${major}.${Number(minor) + 1}.${patch}`;
fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
NODE
  if node "$validator" --prefix "$prefix" --core-package-evidence "$fixture_evidence" >/dev/null 2>&1; then
    echo "Core validator accepted a different Core version" >&2; exit 1
  fi
  mv "$workspace/core-package.saved" "$fixture_evidence"
  cp "$fixture_evidence" "$workspace/core-package.saved"
  CORE_VERSION="$core_version" node - "$fixture_evidence" <<'NODE'
const fs = require('node:fs');
const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, 'utf8'));
value.consumer.runtime.soname = 'libzlink.so.10';
fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
NODE
  if node "$validator" --prefix "$prefix" --core-package-evidence "$fixture_evidence" >/dev/null 2>&1; then
    echo "Core validator accepted a different Core SONAME" >&2; exit 1
  fi
  mv "$workspace/core-package.saved" "$fixture_evidence"
  checks+=(
    "coreValidatorPositiveFixture=pass"
    "provenanceMutationRejected=pass"
    "candidateIdentityMutationRejected=pass"
    "versionMutationRejected=pass"
    "sonameMutationRejected=pass"
  )
  write_evidence pass self-test-dry-run false
  echo "java binding package consumer self-test passed"
  echo "Evidence: $evidence"
  exit 0
fi

$dry_run && { echo "--dry-run cannot be used with --actual" >&2; exit 2; }
[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
[[ "$core_package_evidence" = /* ]] || { echo "--core-package-evidence must be absolute" >&2; exit 2; }
[[ "$maven_repository" = /* ]] || { echo "--maven-repository must be absolute" >&2; exit 2; }
if [[ -e "$maven_repository" ]] && find "$maven_repository" -mindepth 1 -print -quit | grep -q .; then
  echo "--maven-repository must be absent or empty for an actual verification" >&2
  exit 2
fi
mkdir -p "$maven_repository"

workspace="$(mktemp -d)"
consumer="$workspace/consumer"
gradle_home="$workspace/gradle-home"
cp -R "$fixture_dir" "$consumer"
find "$consumer" -type f \( -name '*.java' -o -name '*.gradle' \) -print0 | xargs -0 sed -i \
  -e "s/@BINDING_VERSION@/$binding_version/g" \
  -e "s/@CORE_VERSION@/$core_version/g" \
  -e "s/@CORE_MAJOR@/$core_major/g" \
  -e "s/@CORE_MINOR@/$core_minor/g" \
  -e "s/@CORE_PATCH@/$core_patch/g"
core_summary="$workspace/core-summary.json"
node "$validator" --prefix "$core_prefix" \
  --core-package-evidence "$core_package_evidence" >"$core_summary"
"$build_script" --core-prefix "$core_prefix" \
  --core-package-evidence "$core_package_evidence" \
  --maven-repository "$maven_repository"

coordinate_dir="$maven_repository/systems/zlink/zlink/$binding_version"
jar="$coordinate_dir/zlink-$binding_version.jar"
pom="$coordinate_dir/zlink-$binding_version.pom"
module="$coordinate_dir/zlink-$binding_version.module"
for artifact in "$jar" "$pom" "$module"; do
  [[ -f "$artifact" ]] || { echo "Published Maven metadata is missing: $artifact" >&2; exit 1; }
done

embedded="$workspace/core-package-provenance.json"
unzip -p "$jar" META-INF/zlink/core-package-provenance.json >"$embedded"
expected_provenance_sha="$(node -e 'const s=require(process.argv[1]);process.stdout.write(s.provenanceSha256)' "$core_summary")"
[[ "$(sha256sum "$embedded" | awk '{print $1}')" == "$expected_provenance_sha" ]] || {
  echo "Published jar contains different Core provenance" >&2; exit 1;
}
expected_runtime_sha="$(node -e 'const s=require(process.argv[1]);process.stdout.write(s.runtime.sha256)' "$core_summary")"
packaged_runtime="$workspace/libzlink.so"
unzip -p "$jar" native/linux-x86_64/libzlink.so >"$packaged_runtime"
[[ "$(sha256sum "$packaged_runtime" | awk '{print $1}')" == "$expected_runtime_sha" ]] || {
  echo "Published jar contains a different Core runtime" >&2; exit 1;
}
grep -Fq "<zlink.core.version>$core_version</zlink.core.version>" "$pom"
grep -Fq "<zlink.core.provenance-sha256>$expected_provenance_sha</zlink.core.provenance-sha256>" "$pom"
grep -Fq "zlink-$binding_version.jar" "$module"
grep -Fq "\"systems.zlink.core.provenance-sha256\": \"$expected_provenance_sha\"" "$module"
grep -Fq '"systems.zlink.core.soname": "libzlink.so.11"' "$module"

run_output="$workspace/run-output.txt"
env -u ZLINK_LIBRARY_PATH -u LD_LIBRARY_PATH -u CLASSPATH \
  ZLINK_LOCAL_MAVEN_REPOSITORY="$maven_repository" \
  GRADLE_USER_HOME="$gradle_home" \
  "$repo_root/bindings/java/gradlew" --no-daemon --refresh-dependencies \
  -p "$consumer" clean run >"$run_output"
grep -Fq "ZLINK_CORE_VERSION=$core_version" "$run_output"

checks+=(
  "actualMavenArtifact=pass"
  "actualGradleModuleMetadata=pass"
  "embeddedProvenanceSha256=pass"
  "embeddedRuntimeSha256=pass"
  "isolatedCleanConsumerResolve=pass"
  "isolatedCleanConsumerCompile=pass"
  "isolatedCleanConsumerRun=pass"
  "packagedCore11Load=pass"
)
details="$workspace/details.json"
CORE_SUMMARY="$core_summary" MAVEN_REPOSITORY="$maven_repository" \
 BINDING_VERSION="$binding_version" \
JAR="$jar" POM="$pom" MODULE="$module" node <<'NODE' >"$details"
const crypto = require('node:crypto');
const fs = require('node:fs');
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const core = JSON.parse(fs.readFileSync(process.env.CORE_SUMMARY, 'utf8'));
process.stdout.write(`${JSON.stringify({
  core,
  maven: {
    repository: process.env.MAVEN_REPOSITORY,
    coordinate: `systems.zlink:zlink:${process.env.BINDING_VERSION}`,
    jar: {path: process.env.JAR, sha256: digest(process.env.JAR)},
    pom: {path: process.env.POM, sha256: digest(process.env.POM)},
    module: {path: process.env.MODULE, sha256: digest(process.env.MODULE)},
  },
  consumer: {
    isolatedGradleUserHome: true,
    projectReferenceCount: 0,
    runtimeVersion: process.env.CORE_VERSION,
    bindingVersion: process.env.BINDING_VERSION,
  },
}, null, 2)}\n`);
NODE
write_evidence pass actual-local true "$details"
echo "java binding actual Maven consumer verification passed"
echo "Evidence: $evidence"
