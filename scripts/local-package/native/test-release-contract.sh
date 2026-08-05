#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARSER="${SCRIPT_DIR}/release-tag.sh"
VERIFIER="${SCRIPT_DIR}/verify-release-provenance.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

expect_failure() {
  if "$@" >"${tmp}/unexpected.stdout" 2>"${tmp}/unexpected.stderr"; then
    echo "expected failure but command passed: $*" >&2
    exit 1
  fi
}

stable="$(${PARSER} core/v10.7.0)"
grep -Fxq 'tag=core/v10.7.0' <<<"${stable}"
grep -Fxq 'runtime_version=10.7.0' <<<"${stable}"
grep -Fxq 'channel=stable' <<<"${stable}"

rc="$(${PARSER} 'https://github.com/kairos-code-dev/zlink/releases/tag/core/v10.7.0-rc.2')"
grep -Fxq 'tag=core/v10.7.0-rc.2' <<<"${rc}"
grep -Fxq 'runtime_version=10.7.0' <<<"${rc}"
grep -Fxq 'channel=rc' <<<"${rc}"
grep -Fxq 'sequence=2' <<<"${rc}"
expect_failure "${PARSER}" core/v10.7.0-rc.0
expect_failure "${PARSER}" core/v10.7.0-preview.1
expect_failure "${PARSER}" core/v10.7
expect_failure "${PARSER}" core/v10.7.0-rc.1-extra

make_fixture() {
  local tag="$1"
  local runtime="$2"
  local source_sha="$3"
  local encoded_tag="${tag//\//%2F}"
  local dir="${tmp}/${encoded_tag}"
  mkdir -p "${dir}"
  printf 'payload-checksums\n' >"${dir}/checksums.txt"
  printf 'source-archive\n' >"${dir}/zlink-${runtime}-source.tar.gz"
  IFS=. read -r major minor patch <<<"${runtime}"
  printf '#define ZLINK_VERSION_MAJOR %s\n#define ZLINK_VERSION_MINOR %s\n#define ZLINK_VERSION_PATCH %s\n' \
    "${major}" "${minor}" "${patch}" >"${dir}/zlink.h"
  checksums_sha="$(sha256sum "${dir}/checksums.txt" | awk '{print $1}')"
  archive_sha="$(sha256sum "${dir}/zlink-${runtime}-source.tar.gz" | awk '{print $1}')"
  printf '%s\n' \
    'schema=1' \
    "tag=${tag}" \
    "runtime_version=${runtime}" \
    "source_sha=${source_sha}" \
    "checksums_sha256=${checksums_sha}" \
    "source_archive=zlink-${runtime}-source.tar.gz" \
    "source_archive_sha256=${archive_sha}" \
    "asset_url=https://github.com/kairos-code-dev/zlink/releases/download/${encoded_tag}/zlink-${runtime}-source.tar.gz" \
    >"${dir}/release-provenance.txt"
  printf '%s\n' "${dir}"
}

sha='0123456789abcdef0123456789abcdef01234567'
for tag in core/v10.7.0 core/v10.7.0-rc.2; do
  dir="$(make_fixture "${tag}" 10.7.0 "${sha}")"
  "${VERIFIER}" \
    --tag "${tag}" \
    --repo kairos-code-dev/zlink \
    --manifest "${dir}/release-provenance.txt" \
    --checksums "${dir}/checksums.txt" \
    --source-archive "${dir}/zlink-10.7.0-source.tar.gz" \
    --header "${dir}/zlink.h" \
    --resolved-source-sha "${sha}" >/dev/null
done

rc_dir="${tmp}/core%2Fv10.7.0-rc.2"
cp "${rc_dir}/zlink.h" "${rc_dir}/zlink.bad-suffix.h"
printf '#define ZLINK_VERSION_SUFFIX "-rc.2"\n' >>"${rc_dir}/zlink.bad-suffix.h"
expect_failure "${VERIFIER}" --tag core/v10.7.0-rc.2 --repo kairos-code-dev/zlink \
  --manifest "${rc_dir}/release-provenance.txt" --checksums "${rc_dir}/checksums.txt" \
  --source-archive "${rc_dir}/zlink-10.7.0-source.tar.gz" --header "${rc_dir}/zlink.bad-suffix.h"

cp "${rc_dir}/checksums.txt" "${rc_dir}/checksums.bad.txt"
printf 'tampered\n' >>"${rc_dir}/checksums.bad.txt"
expect_failure "${VERIFIER}" --tag core/v10.7.0-rc.2 --repo kairos-code-dev/zlink \
  --manifest "${rc_dir}/release-provenance.txt" --checksums "${rc_dir}/checksums.bad.txt" \
  --source-archive "${rc_dir}/zlink-10.7.0-source.tar.gz" --header "${rc_dir}/zlink.h"

expect_failure "${VERIFIER}" --tag core/v10.7.0-rc.2 --repo kairos-code-dev/zlink \
  --manifest "${rc_dir}/release-provenance.txt" --checksums "${rc_dir}/checksums.txt" \
  --source-archive "${rc_dir}/zlink-10.7.0-source.tar.gz" --header "${rc_dir}/zlink.h" \
  --resolved-source-sha fedcba9876543210fedcba9876543210fedcba98

cp "${rc_dir}/release-provenance.txt" "${rc_dir}/release-provenance.bad-url.txt"
sed -i 's#asset_url=.*#asset_url=https://example.invalid/source.tar.gz#' "${rc_dir}/release-provenance.bad-url.txt"
expect_failure "${VERIFIER}" --tag core/v10.7.0-rc.2 --repo kairos-code-dev/zlink \
  --manifest "${rc_dir}/release-provenance.bad-url.txt" --checksums "${rc_dir}/checksums.txt" \
  --source-archive "${rc_dir}/zlink-10.7.0-source.tar.gz" --header "${rc_dir}/zlink.h"

echo "release contract fixtures passed: stable=1 rc=1 negative=8"
