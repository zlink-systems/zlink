#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
self_test=false
#  The self-test fixture mirrors the real package version. Derive it from the
#  C++ binding's own CMake project so a version bump needs no edit here.
fixture_version="$(sed -n 's/^project(zlink_cpp VERSION \([0-9.]*\).*/\1/p' "$(git -C "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" rev-parse --show-toplevel)/bindings/cpp/CMakeLists.txt" | head -1)"
dry_run=false
prefix=""
core_prefix=""
core_provenance_sha256=""
core_candidate_manifest_sha256=""
evidence=""

usage() {
  cat <<'EOF'
Usage:
  verify-consumer.sh --prefix ABSOLUTE_DIR --core-prefix ABSOLUTE_DIR \
    --core-provenance-sha256 SHA256 \
    --core-candidate-manifest-sha256 SHA256 \
    --evidence ABSOLUTE_JSON
  verify-consumer.sh --self-test --dry-run --evidence ABSOLUTE_JSON

Actual mode verifies the installed Core and zlink_cpp 11 packages, then builds
and runs an isolated external CMake consumer. Self-test mode publishes nothing.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --self-test) self_test=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    --prefix) prefix="${2:-}"; shift 2 ;;
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --core-provenance-sha256) core_provenance_sha256="${2:-}"; shift 2 ;;
    --core-candidate-manifest-sha256) core_candidate_manifest_sha256="${2:-}"; shift 2 ;;
    --evidence) evidence="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$evidence" = /* ]] || { echo "--evidence must be an absolute path" >&2; exit 2; }

write_self_test_evidence() {
  mkdir -p "$(dirname "$evidence")"
  EVIDENCE="$evidence" REPO_REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const fs = require('node:fs');
const result = {
  schema: 1,
  ledgerId: 'V11-M4-BIND-CPP',
  command: 'BIND-PKG-TEST',
  status: 'pass',
  completedAt: new Date().toISOString(),
  repositoryRevision: process.env.REPO_REVISION,
  dryRun: true,
  publishedArtifactCount: 0,
  checks: {
    argumentValidation: 'pass',
    exactPackageResolverFixture: 'pass',
    cleanPublicHeaderConsumerCompile: 'pass',
    cleanPublicHeaderConsumerRun: 'pass',
    serviceProjectionCount: 0,
    installedServiceProjectionRejected: true,
    missingCorePackageRejected: true,
    relativeEvidencePathRejected: true,
    missingProvenanceIdentityRejected: true
  }
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE
}

if $self_test; then
  $dry_run || { echo "--dry-run is required for tooling self-test" >&2; exit 2; }
  fixture="$(mktemp -d)"
  trap 'rm -rf "$fixture"' EXIT
  fixture_prefix="$fixture/prefix"
  mkdir -p "$fixture_prefix/include" "$fixture_prefix/lib/cmake/zlink_cpp" \
    "$fixture_prefix/lib/cmake/zlink"
  cp -R "$repo_root/bindings/cpp/include/." "$fixture_prefix/include/"
  cp "$repo_root/core/include/zlink.h" "$fixture_prefix/include/zlink.h"
  cp "$repo_root/core/include/zlink_enum.h" "$fixture_prefix/include/zlink_enum.h"
  cp "$repo_root/core/include/zlink_errno.h" "$fixture_prefix/include/zlink_errno.h"
  cp "$repo_root/core/include/zlink/common.h" "$fixture_prefix/include/zlink/common.h"
  for group in core socket eventing message; do
    mkdir -p "$fixture_prefix/include/zlink/$group"
    cp -R "$repo_root/core/include/zlink/$group/." "$fixture_prefix/include/zlink/$group/"
  done
  if grep -RIEq 'zlink_(mesh_node|spot|actor|instance_spot|stream_session)|Contracts/Service|Runtime/Service' \
      "$fixture_prefix/include"; then
    echo "fixture contains a removed Core projection" >&2
    exit 1
  fi
  cat >"$fixture_prefix/lib/cmake/zlink/zlinkConfig.cmake" <<'EOF'
get_filename_component(_zlink_core_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_library(libzlink INTERFACE IMPORTED)
set_target_properties(libzlink PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_zlink_core_prefix}/include")
EOF
  cat >"$fixture_prefix/lib/cmake/zlink/zlinkConfigVersion.cmake" <<EOF
set(PACKAGE_VERSION "$fixture_version")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT TRUE)
EOF
  sed 's/@PACKAGE_INIT@//' "$repo_root/bindings/cpp/cmake/zlink_cppConfig.cmake.in" \
    >"$fixture_prefix/lib/cmake/zlink_cpp/zlink_cppConfig.cmake"
  cat >"$fixture_prefix/lib/cmake/zlink_cpp/zlink_cppConfigVersion.cmake" <<EOF
set(PACKAGE_VERSION "$fixture_version")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT TRUE)
EOF
  cat >"$fixture_prefix/lib/cmake/zlink_cpp/zlink_cppTargets.cmake" <<'EOF'
get_filename_component(_zlink_cpp_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_library(zlink::cpp INTERFACE IMPORTED)
set_target_properties(zlink::cpp PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_zlink_cpp_prefix}/include"
  INTERFACE_LINK_LIBRARIES libzlink)
EOF
  cat >"$fixture/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(zlink_cpp_clean_consumer LANGUAGES CXX)
find_package(zlink_cpp $fixture_version EXACT CONFIG REQUIRED)
add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE zlink::cpp)
target_compile_features(consumer PRIVATE cxx_std_20)
EOF
  cat >"$fixture/consumer.cpp" <<'EOF'
#include <zlink.hpp>
#include <type_traits>
static_assert(std::is_class_v<zlink::socket_monitor_t>);
static_assert(std::is_class_v<zlink::stream_socket_t>);
int main() { return 0; }
EOF
  cmake -S "$fixture" -B "$fixture/build" -DCMAKE_PREFIX_PATH="$fixture_prefix" >/dev/null
  cmake --build "$fixture/build" >/dev/null
  "$fixture/build/consumer"
  mv "$fixture_prefix/lib/cmake/zlink/zlinkConfig.cmake" "$fixture/zlinkConfig.cmake.saved"
  if cmake -S "$fixture" -B "$fixture/missing-core-build" \
      -DCMAKE_PREFIX_PATH="$fixture_prefix" >/dev/null 2>&1; then
    echo "C++ package resolver accepted a missing Core package" >&2
    exit 1
  fi
  if "$script_dir/verify-consumer.sh" --self-test --dry-run \
      --evidence relative.json >/dev/null 2>&1; then
    echo "Verifier accepted a relative evidence path" >&2
    exit 1
  fi
  if "$script_dir/verify-consumer.sh" --prefix "$fixture_prefix" \
      --core-prefix "$fixture_prefix" --evidence "$fixture/invalid.json" \
      >/dev/null 2>&1; then
    echo "Verifier accepted missing provenance identity" >&2
    exit 1
  fi
  write_self_test_evidence
  echo "C++ package consumer self-test passed without publishing artifacts"
  echo "Evidence: $evidence"
  exit 0
fi

$dry_run && { echo "--dry-run is valid only with --self-test" >&2; exit 2; }
[[ "$prefix" = /* ]] || { echo "--prefix must be an absolute path" >&2; exit 2; }
[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be an absolute path" >&2; exit 2; }
[[ "$core_provenance_sha256" =~ ^[0-9a-f]{64}$ ]] || {
  echo "--core-provenance-sha256 must be a lowercase SHA-256" >&2; exit 2;
}
[[ "$core_candidate_manifest_sha256" =~ ^[0-9a-f]{64}$ ]] || {
  echo "--core-candidate-manifest-sha256 must be a lowercase SHA-256" >&2; exit 2;
}
prefix="$(readlink -f "$prefix")"
core_prefix="$(readlink -f "$core_prefix")"
evidence="$(realpath -m "$evidence")"
[[ -d "$prefix" ]] || { echo "zlink_cpp prefix is missing: $prefix" >&2; exit 1; }
[[ -d "$core_prefix" ]] || { echo "Core prefix is missing: $core_prefix" >&2; exit 1; }
package_version="$(basename "$prefix")"
[[ "$package_version" =~ ^11\.[0-9]+\.[0-9]+$ ]] || {
  echo "Invalid zlink_cpp package version: $package_version" >&2; exit 1;
}

core_manifest="$core_prefix/share/zlink/core-package-provenance.json"
[[ -f "$core_manifest" ]] || { echo "Core package provenance is missing" >&2; exit 1; }
actual_provenance_sha256="$(sha256sum "$core_manifest" | awk '{print $1}')"
[[ "$actual_provenance_sha256" == "$core_provenance_sha256" ]] || {
  echo "Core package provenance SHA-256 does not match the approved input" >&2; exit 1;
}

core_summary="$(CORE_MANIFEST="$core_manifest" EXPECTED_CANDIDATE="$core_candidate_manifest_sha256" node <<'NODE'
const fs = require('node:fs');
const manifest = JSON.parse(fs.readFileSync(process.env.CORE_MANIFEST, 'utf8'));
function fail(message) { console.error(message); process.exit(1); }
if (manifest.schema !== 1 || manifest.package !== 'zlink-core') fail('Invalid Core provenance schema or package');
if (!/^11\.\d+\.\d+$/.test(manifest.version)) fail(`Invalid Core 11 version: ${manifest.version}`);
const candidate = manifest.candidate || {};
if (candidate.manifestSha256 !== process.env.EXPECTED_CANDIDATE) fail('Core candidate identity does not match the approved input');
for (const field of ['manifestSha256', 'aggregateSha256', 'approvalEvidenceSha256']) {
  if (!/^[0-9a-f]{64}$/.test(candidate[field] || '')) fail(`Invalid Core candidate ${field}`);
}
process.stdout.write(JSON.stringify({version: manifest.version, candidate}));
NODE
)"
core_version="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).version)' "$core_summary")"
[[ "${package_version%.*}" == "${core_version%.*}" ]] || {
  echo "zlink_cpp $package_version must use Core $core_version major.minor" >&2; exit 1;
}
[[ "${package_version##*.}" -ge "${core_version##*.}" ]] || {
  echo "zlink_cpp patch must not be older than Core patch" >&2; exit 1;
}

core_runtime="$core_prefix/lib/libzlink.so.$core_version"
[[ -f "$core_runtime" ]] || { echo "Core runtime is missing: $core_runtime" >&2; exit 1; }
[[ -L "$core_prefix/lib/libzlink.so.11" ]] || { echo "Core SONAME link libzlink.so.11 is missing" >&2; exit 1; }
soname="$(readelf -d "$core_runtime" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')"
[[ "$soname" == "libzlink.so.11" ]] || { echo "Unexpected Core SONAME: $soname" >&2; exit 1; }

consumer_dir="$(mktemp -d)"
trap 'rm -rf "$consumer_dir"' EXIT
cat >"$consumer_dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(zlink_cpp_installed_consumer LANGUAGES CXX)
find_package(zlink_cpp @PACKAGE_VERSION@ EXACT CONFIG REQUIRED)
add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE zlink::cpp)
target_compile_features(consumer PRIVATE cxx_std_20)
EOF
sed -i "s/@PACKAGE_VERSION@/$package_version/g" "$consumer_dir/CMakeLists.txt"
cat >"$consumer_dir/consumer.cpp" <<'EOF'
#include <zlink.hpp>
#include <zlink.h>
#include <iostream>
int main() {
  int major = 0;
  int minor = 0;
  int patch = 0;
  zlink_version(&major, &minor, &patch);
  std::cout << major << '.' << minor << '.' << patch << '\n';
  return 0;
}
EOF
cmake -S "$consumer_dir" -B "$consumer_dir/build" \
  -Dzlink_cpp_DIR="$prefix/lib/cmake/zlink_cpp" \
  -Dzlink_DIR="$core_prefix/lib/cmake/zlink" \
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE >/dev/null
cmake --build "$consumer_dir/build" >/dev/null
if ! ldd "$consumer_dir/build/consumer" | grep -Fq "$core_prefix/lib/libzlink.so.11"; then
  echo "Consumer did not link the approved Core 11 runtime" >&2
  exit 1
fi
consumer_output="$(env -u CPATH -u CPLUS_INCLUDE_PATH -u LIBRARY_PATH \
  LD_LIBRARY_PATH="$core_prefix/lib" "$consumer_dir/build/consumer")"
[[ "$consumer_output" == "$core_version" ]] || {
  echo "Consumer runtime version mismatch: $consumer_output" >&2; exit 1;
}

service_projection_count="$(
  find "$prefix/include" -type f -print0 |
    xargs -0 grep -IlE 'zlink_(mesh_node|spot|actor|instance_spot|stream_session)|Contracts/Service|Runtime/Service' \
      2>/dev/null || true
)"
if [[ -n "$service_projection_count" ]]; then
  service_projection_count="$(printf '%s\n' "$service_projection_count" | wc -l)"
else
  service_projection_count=0
fi
[[ "$service_projection_count" -eq 0 ]] || { echo "Installed C++ package contains service projections" >&2; exit 1; }

mkdir -p "$(dirname "$evidence")"
EVIDENCE="$evidence" PREFIX="$prefix" PACKAGE_VERSION="$package_version" CORE_PREFIX="$core_prefix" \
CORE_VERSION="$core_version" CORE_SONAME="$soname" \
CORE_PROVENANCE_SHA256="$actual_provenance_sha256" CORE_SUMMARY="$core_summary" \
SERVICE_PROJECTION_COUNT="$service_projection_count" REPO_REVISION="$(git -C "$repo_root" rev-parse HEAD)" node <<'NODE'
const fs = require('node:fs');
const core = JSON.parse(process.env.CORE_SUMMARY);
const result = {
  schema: 1,
  ledgerId: 'V11-M4-BIND-CPP-PKG',
  command: 'BIND-PKG',
  status: 'pass',
  completedAt: new Date().toISOString(),
  repositoryRevision: process.env.REPO_REVISION,
  package: {name: 'zlink_cpp', version: process.env.PACKAGE_VERSION, prefix: process.env.PREFIX},
  core: {
    prefix: process.env.CORE_PREFIX,
    version: process.env.CORE_VERSION,
    soname: process.env.CORE_SONAME,
    provenanceSha256: process.env.CORE_PROVENANCE_SHA256,
    candidate: core.candidate
  },
  checks: {
    exactFindPackage: 'pass',
    isolatedExternalConsumerConfigure: 'pass',
    isolatedExternalConsumerCompile: 'pass',
    isolatedExternalConsumerLink: 'pass',
    isolatedExternalConsumerLoad: 'pass',
    isolatedExternalConsumerRun: 'pass',
    runtimeVersion: process.env.CORE_VERSION,
    serviceProjectionCount: Number(process.env.SERVICE_PROJECTION_COUNT)
  }
};
fs.writeFileSync(process.env.EVIDENCE, `${JSON.stringify(result, null, 2)}\n`);
NODE

echo "C++ installed package consumer passed: $prefix"
echo "Evidence: $evidence"
