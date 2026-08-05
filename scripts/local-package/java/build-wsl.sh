#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
out_dir="$artifact_root/maven"
bindings_dir="$repo_root/bindings/java"
core_prefix=""
core_package_evidence=""

usage() {
  cat <<'EOF'
Usage: build-wsl.sh --core-prefix ABSOLUTE_DIR \
  --core-package-evidence ABSOLUTE_JSON [--maven-repository ABSOLUTE_DIR]

Publishes the version declared by bindings/java/build.gradle only to the
selected local Maven repository after matching the exact approved Core 11
package identity.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --core-package-evidence) core_package_evidence="${2:-}"; shift 2 ;;
    --maven-repository) out_dir="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$core_prefix" = /* ]] || {
  echo "--core-prefix must name the approved absolute Core 11 install prefix" >&2
  exit 2
}
[[ "$core_package_evidence" = /* ]] || {
  echo "--core-package-evidence must name an absolute V11-M3-CORE-PKG result" >&2
  exit 2
}
[[ "$out_dir" = /* ]] || {
  echo "--maven-repository must be absolute when specified" >&2
  exit 2
}

core_summary="$(node "$script_dir/verify-core-input.mjs" \
  --prefix "$core_prefix" --core-package-evidence "$core_package_evidence")"

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd -P)"

(
  cd "$bindings_dir"
  export ZLINK_CORE_PACKAGE_PREFIX="$core_prefix"
  export ZLINK_CORE_PACKAGE_SUMMARY="$core_summary"
  MAVEN_REPOSITORY_URL="file://$out_dir" ./gradlew --no-daemon \
    :clean :publishMavenJavaPublicationToReleaseRepoRepository
)

echo "-- Java local Maven repository output: $out_dir"
