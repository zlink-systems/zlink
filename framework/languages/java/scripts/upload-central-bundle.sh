#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
java_root="$(cd "$script_dir/.." && pwd)"
version="$(sed -n 's/^version = "\([^"]*\)"/\1/p' "$java_root/build.gradle.kts" | head -n1)"
staging_dir="${MAVEN_CENTRAL_BUNDLE_DIR:-$java_root/build/central-staging}"
bundle_path="${MAVEN_CENTRAL_BUNDLE_PATH:-$java_root/build/zlink-framework-java-$version-central-bundle.zip}"

if [[ -z "$version" ]]; then
  echo "framework Java version is missing from build.gradle.kts" >&2
  exit 1
fi
if [[ ! -d "$staging_dir" ]]; then
  echo "Central staging repository is missing: $staging_dir" >&2
  exit 1
fi

work_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/zlink-framework-java-central.XXXXXX")"
trap 'rm -rf -- "$work_dir"' EXIT

artifact_ids=(
  zlink-framework-binding-internal
  zlink-framework-codec-msgpack
  zlink-framework-codec-protobuf
  zlink-framework-core
  zlink-framework-json-internal
  zlink-framework-kotlin
  zlink-framework-locations-redis
  zlink-framework-provider-abstractions
  zlink-framework-spring-boot-starter
  zlink-framework-testkit
  zlink-http-client
  zlink-http-client-kotlin
  zlink-stream-connector
)

for artifact_id in "${artifact_ids[@]}"; do
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

  for suffix in .jar -sources.jar -javadoc.jar .pom; do
    artifact="$target_dir/$artifact_id-$version$suffix"
    if [[ ! -f "$artifact" || ! -f "$artifact.asc" ]]; then
      echo "Required signed Maven artifact is missing: $artifact" >&2
      exit 1
    fi
  done
done

while IFS= read -r -d '' artifact; do
  md5sum "$artifact" | awk '{print $1}' >"$artifact.md5"
  sha1sum "$artifact" | awk '{print $1}' >"$artifact.sha1"
done < <(find "$work_dir" -type f ! -name '*.asc' -print0)

mkdir -p "$(dirname "$bundle_path")"
rm -f -- "$bundle_path"
(
  cd "$work_dir"
  zip -q -r "$bundle_path" systems
)

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
  "https://central.sonatype.com/api/v1/publisher/upload?name=zlink-framework-java-$version&publishingType=USER_MANAGED")"
unset authorization

echo "Central Portal deployment created (USER_MANAGED): $deployment_id"
