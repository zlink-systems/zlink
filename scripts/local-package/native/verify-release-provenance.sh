#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=release-tag.sh
source "${SCRIPT_DIR}/release-tag.sh"

usage() {
  cat <<'USAGE'
Usage:
  verify-release-provenance.sh --tag TAG --repo OWNER/REPO \
    --manifest FILE --checksums FILE --source-archive FILE --header FILE \
    [--resolved-source-sha SHA]
USAGE
}

tag_ref=""
repo=""
manifest=""
checksums=""
source_archive=""
header=""
resolved_source_sha=""
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --tag) tag_ref="${2:-}"; shift ;;
    --repo) repo="${2:-}"; shift ;;
    --manifest) manifest="${2:-}"; shift ;;
    --checksums) checksums="${2:-}"; shift ;;
    --source-archive) source_archive="${2:-}"; shift ;;
    --header) header="${2:-}"; shift ;;
    --resolved-source-sha) resolved_source_sha="${2:-}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Error: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

for value_name in tag_ref repo manifest checksums source_archive header; do
  if [[ -z "${!value_name}" ]]; then
    echo "Error: --${value_name//_/-} is required." >&2
    exit 2
  fi
done
if [[ ! "${repo}" =~ ^[^/[:space:]]+/[^/[:space:]]+$ ]]; then
  echo "Error: repository must use owner/repo: ${repo}" >&2
  exit 2
fi
for file in "${manifest}" "${checksums}" "${source_archive}" "${header}"; do
  if [[ ! -f "${file}" ]]; then
    echo "Error: provenance input not found: ${file}" >&2
    exit 1
  fi
done

zlink_parse_core_release_ref "${tag_ref}"

declare -A fields=()
while IFS='=' read -r key value; do
  [[ -z "${key}" ]] && continue
  if [[ ! "${key}" =~ ^[a-z0-9_]+$ || -v "fields[${key}]" ]]; then
    echo "Error: malformed or duplicate provenance field: ${key}" >&2
    exit 1
  fi
  fields["${key}"]="${value}"
done < "${manifest}"

required_fields=(
  schema tag runtime_version source_sha checksums_sha256
  source_archive source_archive_sha256 asset_url
)
for key in "${required_fields[@]}"; do
  if [[ -z "${fields[${key}]:-}" ]]; then
    echo "Error: provenance field is missing: ${key}" >&2
    exit 1
  fi
done
if [[ "${#fields[@]}" -ne "${#required_fields[@]}" ]]; then
  echo "Error: provenance contains unsupported fields." >&2
  exit 1
fi

expected_archive="zlink-${ZLINK_CORE_RUNTIME_VERSION}-source.tar.gz"
expected_url="https://github.com/${repo}/releases/download/${ZLINK_CORE_RELEASE_TAG_ENCODED}/${expected_archive}"
checksums_sha="$(sha256sum "${checksums}" | awk '{print $1}')"
archive_sha="$(sha256sum "${source_archive}" | awk '{print $1}')"

[[ "${fields[schema]}" == "1" ]] || { echo "Error: unsupported provenance schema." >&2; exit 1; }
[[ "${fields[tag]}" == "${ZLINK_CORE_RELEASE_TAG}" ]] || { echo "Error: provenance tag mismatch." >&2; exit 1; }
[[ "${fields[runtime_version]}" == "${ZLINK_CORE_RUNTIME_VERSION}" ]] || { echo "Error: provenance runtime version mismatch." >&2; exit 1; }
[[ "${fields[source_archive]}" == "${expected_archive}" ]] || { echo "Error: provenance source archive mismatch." >&2; exit 1; }
[[ "$(basename "${source_archive}")" == "${expected_archive}" ]] || { echo "Error: source archive filename mismatch." >&2; exit 1; }
[[ "${fields[checksums_sha256]}" == "${checksums_sha}" ]] || { echo "Error: checksums digest mismatch." >&2; exit 1; }
[[ "${fields[source_archive_sha256]}" == "${archive_sha}" ]] || { echo "Error: source archive digest mismatch." >&2; exit 1; }
[[ "${fields[asset_url]}" == "${expected_url}" ]] || { echo "Error: source asset URL mismatch." >&2; exit 1; }
if [[ ! "${fields[source_sha]}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "Error: source SHA must be a lowercase 40-character Git commit id." >&2
  exit 1
fi
if [[ -n "${resolved_source_sha}" && "${fields[source_sha]}" != "${resolved_source_sha}" ]]; then
  echo "Error: source SHA does not match the release tag commit." >&2
  exit 1
fi

major="${ZLINK_CORE_RUNTIME_VERSION%%.*}"
minor_patch="${ZLINK_CORE_RUNTIME_VERSION#*.}"
minor="${minor_patch%%.*}"
patch="${ZLINK_CORE_RUNTIME_VERSION##*.}"
for entry in "MAJOR:${major}" "MINOR:${minor}" "PATCH:${patch}"; do
  macro="${entry%%:*}"
  expected="${entry##*:}"
  actual="$(awk -v name="ZLINK_VERSION_${macro}" '$1 == "#define" && $2 == name {print $3}' "${header}")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "Error: ${macro} version marker mismatch (actual=${actual:-missing}, expected=${expected})." >&2
    exit 1
  fi
done
if grep -Eqi -- '-rc\.[0-9]+' "${header}"; then
  echo "Error: RC suffix leaked into the numeric runtime header." >&2
  exit 1
fi

echo "release provenance verified: tag=${ZLINK_CORE_RELEASE_TAG} runtime=${ZLINK_CORE_RUNTIME_VERSION} source=${fields[source_sha]}"
