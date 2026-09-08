#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
java_root="$(cd "$script_dir/.." && pwd)"
repo_root="$(git -C "$java_root" rev-parse --show-toplevel)"
version="$(sed -n 's/^ZLINK_BINDINGS_VERSION=//p' "$repo_root/BINDINGS_VERSION")"
staging_dir="${MAVEN_CENTRAL_BUNDLE_DIR:-$java_root/build/central-staging}"
bundle_path="${MAVEN_CENTRAL_BUNDLE_PATH:-$java_root/build/zlink-java-$version-central-bundle.zip}"
prepare_only=false

if [[ "${1:-}" == "--prepare-only" ]]; then
  prepare_only=true
  shift
fi
if [[ $# -ne 0 ]]; then
  echo "Usage: upload-central-bundle.sh [--prepare-only]" >&2
  exit 2
fi
if [[ -z "$version" ]]; then
  echo "BINDINGS_VERSION does not contain ZLINK_BINDINGS_VERSION" >&2
  exit 1
fi
if [[ ! -d "$staging_dir" ]]; then
  echo "Central staging repository is missing: $staging_dir" >&2
  exit 1
fi

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zlink-java-central.XXXXXX")"
cleanup() {
  find "$work_dir" -mindepth 1 -delete
  rmdir "$work_dir"
}
trap cleanup EXIT

for artifact_id in zlink zlink-ext-netty; do
  source_dir="$staging_dir/systems/zlink/$artifact_id/$version"
  target_dir="$work_dir/systems/zlink/$artifact_id/$version"
  if [[ ! -d "$source_dir" ]]; then
    echo "Staged Maven component is missing: systems.zlink:$artifact_id:$version" >&2
    exit 1
  fi
  mkdir -p "$target_dir"
  cp -a "$source_dir/." "$target_dir/"
  find "$target_dir" -type f \( \
    -name '*.md5' -o -name '*.sha1' -o \
    -name '*.sha256' -o -name '*.sha512' \) -delete

  for suffix in .jar -sources.jar -javadoc.jar .pom .module; do
    artifact="$target_dir/$artifact_id-$version$suffix"
    if [[ ! -f "$artifact" ]]; then
      echo "Required Maven artifact is missing: $artifact" >&2
      exit 1
    fi
    if [[ "$prepare_only" == false && ! -f "$artifact.asc" ]]; then
      echo "Required Maven signature is missing: $artifact.asc" >&2
      exit 1
    fi
  done
done

while IFS= read -r -d '' artifact; do
  md5sum "$artifact" | awk '{print $1}' >"$artifact.md5"
  sha1sum "$artifact" | awk '{print $1}' >"$artifact.sha1"
done < <(find "$work_dir" -type f \
  ! -name '*.asc' -print0)

mkdir -p "$(dirname "$bundle_path")"
rm -f -- "$bundle_path"
(
  cd "$work_dir"
  zip -q -r "$bundle_path" systems
)
echo "Central bundle: $bundle_path"

if [[ "$prepare_only" == true ]]; then
  exit 0
fi

for secret_name in MAVEN_CENTRAL_USERNAME MAVEN_CENTRAL_PASSWORD; do
  if [[ -z "${!secret_name:-}" ]]; then
    echo "Required environment variable is missing: $secret_name" >&2
    exit 1
  fi
done

authorization="$(printf '%s:%s' \
  "$MAVEN_CENTRAL_USERNAME" "$MAVEN_CENTRAL_PASSWORD" \
  | base64 | tr -d '\n')"
deployment_id="$(curl --fail-with-body --silent --show-error \
  --request POST \
  --header "Authorization: Bearer $authorization" \
  --form "bundle=@$bundle_path;type=application/octet-stream" \
  "https://central.sonatype.com/api/v1/publisher/upload?name=zlink-java-$version&publishingType=USER_MANAGED")"
unset authorization

echo "Central Portal deployment created (USER_MANAGED): $deployment_id"
