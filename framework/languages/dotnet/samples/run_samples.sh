#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAMPLES=(TicTacToe Bingo SupportChat ShoppingMall DeliveryDispatch GameQuest ZoneWorld)

# A locally rebuilt binding can retain the same package version. Keep the
# default sample run independent of a stale user-level NuGet extraction while
# still honoring an explicitly selected cache (for CI or package debugging).
# The cache must survive the process: project assets retain absolute package
# paths for subsequent --no-restore builds.
if [[ -z "${NUGET_PACKAGES:-}" ]]; then
  REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
  PACKAGE_VERSION="$(awk -F= '/^LIBZLINK_VERSION=/{ print $2; exit }' "${REPOSITORY_ROOT}/VERSION")"
  if [[ -z "${PACKAGE_VERSION}" ]]; then
    echo "Unable to resolve the local Systems.Zlink package version." >&2
    exit 1
  fi
  BINDING_PACKAGE="${REPOSITORY_ROOT}/.artifacts/wsl/nuget/Systems.Zlink.${PACKAGE_VERSION}.nupkg"
  if [[ ! -f "${BINDING_PACKAGE}" ]]; then
    echo "Missing local Systems.Zlink package: ${BINDING_PACKAGE}" >&2
    exit 1
  fi
  PACKAGE_HASH="$(sha256sum "${BINDING_PACKAGE}" | cut -d ' ' -f 1)"
  SAMPLE_NUGET_PACKAGES="${TMPDIR:-/tmp}/zlink-dotnet-samples-nuget-${PACKAGE_HASH:0:16}"
  mkdir -p "${SAMPLE_NUGET_PACKAGES}"
  export NUGET_PACKAGES="${SAMPLE_NUGET_PACKAGES}"
fi

if (( $# > 0 )); then
  SAMPLES=("$@")
fi

RUNNER_STATE_DIR="$(mktemp -d)"
trap 'rm -rf "${RUNNER_STATE_DIR}"' EXIT

for sample in "${SAMPLES[@]}"; do
  case "${sample}" in
    TicTacToe|Bingo|SupportChat|ShoppingMall|DeliveryDispatch|GameQuest|ZoneWorld)
      ;;
    *)
      echo "Unknown .NET sample '${sample}'." >&2
      exit 2
      ;;
  esac
  output_file="${RUNNER_STATE_DIR}/${sample}.output"
  teardown_status_file="${RUNNER_STATE_DIR}/${sample}.teardown"
  : >"${teardown_status_file}"
  runner_status=0
  ZLINK_SAMPLE_TEARDOWN_STATUS_FILE="${teardown_status_file}" \
    bash "${SCRIPT_DIR}/${sample}/run_sample.sh" >"${output_file}" 2>&1 || runner_status=$?
  if [[ -s "${teardown_status_file}" ]]; then
    while IFS=$'\t' read -r pid role; do
      echo "Sample role ${role} (pid ${pid}) exited during cleanup with status 137 (SIGKILL)." >&2
    done <"${teardown_status_file}"
    exit 137
  fi
  cat "${output_file}"
  if (( runner_status != 0 )); then
    exit "${runner_status}"
  fi
done
