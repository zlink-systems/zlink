#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]

Publishes systems.zlink:zlink:0.10.1 to the local Maven repository.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
core_prefix="$(readlink -f "$core_prefix")"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
manifest="$core_prefix/share/zlink/core-package-provenance.json"
manifest_sha="$(sha256sum "$manifest" | awk '{print $1}')"
runtime_sha="$(sha256sum "$core_prefix/lib/libzlink.so.$version" | awk '{print $1}')"
summary="$artifact_root/maven/core-package-summary.json"
mkdir -p "$artifact_root/maven"
cat >"$summary" <<EOF
{
  "version": "$version",
  "prefix": "$core_prefix",
  "provenanceSha256": "$manifest_sha",
  "runtime": {"sha256": "$runtime_sha", "soname": "libzlink.so.0"}
}
EOF

(
  cd "$repo_root/bindings/java"
  ZLINK_CORE_PACKAGE_PREFIX="$core_prefix" \
  ZLINK_CORE_PACKAGE_SUMMARY="$(tr -d '\n' < "$summary")" \
  MAVEN_REPOSITORY_URL="file://$artifact_root/maven" \
    ./gradlew --no-daemon clean publishMavenJavaPublicationToReleaseRepoRepository
)

[[ -f "$artifact_root/maven/systems/zlink/zlink/$version/zlink-$version.jar" ]] || {
  echo "Maven binding package is missing" >&2
  exit 1
}
echo "Java local package: $artifact_root/maven/systems/zlink/zlink/$version"
