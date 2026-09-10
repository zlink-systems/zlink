#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
COMMON_E2E_DIR="${REPO_ROOT}/framework/doc/framework/common/e2e"
UNIMPLEMENTED_INVENTORY="${SCRIPT_DIR}/unimplemented-scenarios.tsv"

# These prefixes are the scenario families defined by the common E2E documents.
# Restricting the expression avoids treating prose such as UTF-8 as a scenario ID.
ID_PATTERN='(RM|SM|PS|RC|RL|SF|MON|TD|TA|ST|OBS|CH|SA|IS)-[A-Z0-9]+(-[A-Z0-9]+)*'

CONFIGS=(
  "1:RegistryMessaging:config-1-location-messaging"
  "2:SpotService:config-2-spot-service"
  "3:PubSub:config-3-pubsub"
  "4:RegistrationCodec:config-4-registration-codec"
  "5:ResilienceLifecycle:config-5-resilience-lifecycle"
  "6:DiscoveryRegistryHa:config-6-store-failure-recovery"
  "7:RuntimeMonitoring:config-7-monitoring"
  "8:AutomaticTurnDispatch:config-8-execution-turn"
  "9:ToActorMessaging:config-9-to-actor-messaging"
  "10:SpotActorTransfer:config-10-spot-actor-relocation"
  "11:ObservabilityOps:config-11-observability-ops"
  "12:ChannelEgressRouting:config-12-channel-egress-routing"
  "13:SubmitAdmission:config-13-submit-admission"
  "14:InstanceSpot:config-14-instance-spot"
)

extract_common_ids() {
  rg '^#### ' "$1" | rg --pcre2 -o "(?<![A-Z0-9-])${ID_PATTERN}(?![A-Z0-9-])" | sort -u || true
}

extract_feature_ids() {
  rg --pcre2 -o "(?<![A-Z0-9-])${ID_PATTERN}(?![A-Z0-9-])" "$1" | sort -u || true
}

extract_source_ids() {
  local config_dir="$1"
  rg --pcre2 -o "(?<![A-Z0-9-])${ID_PATTERN}(?![A-Z0-9-])" \
    --no-filename \
    --glob '!*.md' \
    --glob '!*.json' \
    --glob '!*.log' \
    --glob '!logs/**' \
    --glob '!build/**' \
    --glob '!artifacts/**' \
    "$config_dir" | sort -u || true
}

extract_feature_status() {
  local scenario_id="$1"
  local feature_map="$2"
  awk -F '|' -v scenario_id="${scenario_id}" '
    /^\|/ {
      id = $2
      gsub(/^[[:space:]`]+|[[:space:]`]+$/, "", id)
      if (id == scenario_id) {
        status = $3
        gsub(/^[[:space:]`]+|[[:space:]`]+$/, "", status)
        print status
      }
    }
  ' "${feature_map}"
}

if [[ ! -f "${UNIMPLEMENTED_INVENTORY}" ]]; then
  echo "[cpp-e2e-inventory] missing C++ unimplemented inventory: ${UNIMPLEMENTED_INVENTORY}" >&2
  exit 1
fi

declare -A unimplemented=()
declare -A unimplemented_seen=()
while IFS=$'\t' read -r directory scenario_id evidence remainder; do
  [[ -z "${directory}" || "${directory}" == \#* ]] && continue
  if [[ -z "${scenario_id}" || -z "${evidence}" || -n "${remainder}" ]]; then
    echo "[cpp-e2e-inventory] malformed unimplemented inventory row: ${directory} ${scenario_id} ${evidence}" >&2
    exit 1
  fi
  if [[ ! "${scenario_id}" =~ ^${ID_PATTERN}$ ]]; then
    echo "[cpp-e2e-inventory] malformed scenario ID in unimplemented inventory: ${scenario_id}" >&2
    exit 1
  fi
  case "${evidence}" in
    feature-map:*|common-only:no-feature-or-source|common-only:legacy-atd-id-is-not-evidence|selector-placeholder-only:no-feature) ;;
    *)
      echo "[cpp-e2e-inventory] unsupported unimplemented evidence: ${directory}:${scenario_id}:${evidence}" >&2
      exit 1
      ;;
  esac
  key="${directory}:${scenario_id}"
  if [[ -n "${unimplemented[${key}]+x}" ]]; then
    echo "[cpp-e2e-inventory] duplicate unimplemented inventory row: ${key}" >&2
    exit 1
  fi
  unimplemented["${key}"]="${evidence}"
done < "${UNIMPLEMENTED_INVENTORY}"

failure_count=0
config_count=0
scenario_count=0
feature_missing_count=0
source_missing_count=0
status_gap_count=0
implemented_count=0
unimplemented_count=0

for config in "${CONFIGS[@]}"; do
  IFS=: read -r number directory document <<<"${config}"
  config_count=$((config_count + 1))
  common_document_en="${COMMON_E2E_DIR}/${document}.en.md"
  common_document_ko="${COMMON_E2E_DIR}/${document}.ko.md"
  cpp_directory="${SCRIPT_DIR}/${directory}"
  feature_map="${cpp_directory}/feature-map.ko.md"
  runner="${cpp_directory}/run_e2e.sh"

  if [[ ! -f "${common_document_en}" || ! -f "${common_document_ko}" ]]; then
    echo "[cpp-e2e-inventory] missing common document pair: ${document}.en.md/.ko.md" >&2
    failure_count=$((failure_count + 1))
    continue
  fi

  mapfile -t common_ids_en < <(extract_common_ids "${common_document_en}")
  mapfile -t common_ids_ko < <(extract_common_ids "${common_document_ko}")
  mapfile -t en_only_ids < <(
    comm -23 \
      <(printf '%s\n' "${common_ids_en[@]}") \
      <(printf '%s\n' "${common_ids_ko[@]}")
  )
  mapfile -t ko_only_ids < <(
    comm -13 \
      <(printf '%s\n' "${common_ids_en[@]}") \
      <(printf '%s\n' "${common_ids_ko[@]}")
  )
  if [[ "${#en_only_ids[@]}" -gt 0 || "${#ko_only_ids[@]}" -gt 0 ]]; then
    echo "[cpp-e2e-inventory] common document ID parity mismatch: ${document}" >&2
    [[ "${#en_only_ids[@]}" -eq 0 ]] || printf '  EN-only IDs: %s\n' "${en_only_ids[*]}" >&2
    [[ "${#ko_only_ids[@]}" -eq 0 ]] || printf '  KO-only IDs: %s\n' "${ko_only_ids[*]}" >&2
    failure_count=$((failure_count + 1))
  fi
  common_ids=("${common_ids_en[@]}")
  scenario_count=$((scenario_count + ${#common_ids[@]}))
  implemented_ids=()
  unimplemented_ids=()
  for scenario_id in "${common_ids[@]}"; do
    key="${directory}:${scenario_id}"
    if [[ -n "${unimplemented[${key}]+x}" ]]; then
      unimplemented_ids+=("${scenario_id}")
      unimplemented_seen["${key}"]=1
    else
      implemented_ids+=("${scenario_id}")
    fi
  done
  implemented_count=$((implemented_count + ${#implemented_ids[@]}))
  unimplemented_count=$((unimplemented_count + ${#unimplemented_ids[@]}))
  printf '[cpp-e2e-inventory] config-%02d %-24s common=%2d' \
    "${number}" "${directory}" "${#common_ids[@]}"

  if [[ ! -d "${cpp_directory}" ]]; then
    echo " cpp=missing"
    failure_count=$((failure_count + 1))
    feature_missing_count=$((feature_missing_count + ${#common_ids[@]}))
    source_missing_count=$((source_missing_count + ${#common_ids[@]}))
    continue
  fi

  if [[ ! -f "${feature_map}" ]]; then
    echo " feature-map=missing"
    failure_count=$((failure_count + 1))
    feature_missing_count=$((feature_missing_count + ${#common_ids[@]}))
  else
    mapfile -t feature_ids < <(extract_feature_ids "${feature_map}")
    declare -A feature_id_set=()
    for scenario_id in "${feature_ids[@]}"; do
      feature_id_set["${scenario_id}"]=1
    done
    missing_feature_ids=()
    if [[ "${#implemented_ids[@]}" -gt 0 ]]; then
      mapfile -t missing_feature_ids < <(
        comm -23 \
          <(printf '%s\n' "${implemented_ids[@]}" | sort -u) \
          <(printf '%s\n' "${feature_ids[@]}")
      )
    fi
    if [[ "${#missing_feature_ids[@]}" -gt 0 ]]; then
      echo " feature-map-missing=${#missing_feature_ids[@]}"
      printf '  missing feature-map IDs: %s\n' "${missing_feature_ids[*]}" >&2
      failure_count=$((failure_count + ${#missing_feature_ids[@]}))
      feature_missing_count=$((feature_missing_count + ${#missing_feature_ids[@]}))
    else
      printf ' feature-map=complete'
    fi

    for scenario_id in "${implemented_ids[@]}"; do
      feature_status="$(extract_feature_status "${scenario_id}" "${feature_map}")"
      if [[ "${feature_status}" =~ 미구현|부분|blocked|deferred|component[[:space:]]+only|not-supported|전환|gap|재검증 ]]; then
        printf '  implemented inventory contradicts feature-map status: %s (%s)\n' \
          "${scenario_id}" "${feature_status}" >&2
        status_gap_count=$((status_gap_count + 1))
        failure_count=$((failure_count + 1))
      fi
    done

    for scenario_id in "${unimplemented_ids[@]}"; do
      feature_status="$(extract_feature_status "${scenario_id}" "${feature_map}")"
      if [[ "${feature_status}" =~ ^(구현|implemented|통과)$ ]]; then
        printf '  unimplemented inventory contradicts feature-map status: %s (%s)\n' \
          "${scenario_id}" "${feature_status}" >&2
        status_gap_count=$((status_gap_count + 1))
        failure_count=$((failure_count + 1))
      fi
    done
  fi

  if [[ ! -f "${runner}" ]]; then
    echo " runner=missing"
    failure_count=$((failure_count + 1))
  else
    mapfile -t source_ids < <(extract_source_ids "${cpp_directory}")
    declare -A source_id_set=()
    for scenario_id in "${source_ids[@]}"; do
      source_id_set["${scenario_id}"]=1
    done
    missing_source_ids=()
    if [[ "${#implemented_ids[@]}" -gt 0 ]]; then
      mapfile -t missing_source_ids < <(
        comm -23 \
          <(printf '%s\n' "${implemented_ids[@]}" | sort -u) \
          <(printf '%s\n' "${source_ids[@]}")
      )
    fi
    if [[ "${#missing_source_ids[@]}" -gt 0 ]]; then
      echo " source-missing=${#missing_source_ids[@]}"
      printf '  missing source/runner IDs: %s\n' "${missing_source_ids[*]}" >&2
      failure_count=$((failure_count + ${#missing_source_ids[@]}))
      source_missing_count=$((source_missing_count + ${#missing_source_ids[@]}))
    else
      echo " source=referenced"
    fi
  fi

  for scenario_id in "${unimplemented_ids[@]}"; do
    key="${directory}:${scenario_id}"
    evidence="${unimplemented[${key}]}"
    case "${evidence}" in
      feature-map:*)
        if [[ -z "${feature_id_set[${scenario_id}]+x}" ]]; then
          printf '  unimplemented evidence lacks feature-map reference: %s (%s)\n' \
            "${scenario_id}" "${evidence}" >&2
          failure_count=$((failure_count + 1))
        fi
        ;;
      common-only:*)
        if [[ -n "${feature_id_set[${scenario_id}]+x}" \
              || -n "${source_id_set[${scenario_id}]+x}" ]]; then
          printf '  common-only evidence has an exact feature/source reference: %s (%s)\n' \
            "${scenario_id}" "${evidence}" >&2
          failure_count=$((failure_count + 1))
        fi
        ;;
      selector-placeholder-only:no-feature)
        if [[ -n "${feature_id_set[${scenario_id}]+x}" \
              || -z "${source_id_set[${scenario_id}]+x}" ]]; then
          printf '  selector-placeholder evidence shape changed: %s (%s)\n' \
            "${scenario_id}" "${evidence}" >&2
          failure_count=$((failure_count + 1))
        fi
        ;;
    esac
  done
done


for key in "${!unimplemented[@]}"; do
  if [[ -z "${unimplemented_seen[${key}]+x}" ]]; then
    echo "[cpp-e2e-inventory] unimplemented inventory ID is not in its common config: ${key}" >&2
    failure_count=$((failure_count + 1))
  fi
done

printf '[cpp-e2e-inventory] configs=%d scenarios=%d implemented=%d unimplemented=%d feature-map-missing=%d source-missing=%d status-conflicts=%d\n' \
  "${config_count}" "${scenario_count}" "${implemented_count}" "${unimplemented_count}" \
  "${feature_missing_count}" "${source_missing_count}" "${status_gap_count}"

if [[ "${failure_count}" -ne 0 ]]; then
  echo "[cpp-e2e-inventory] FAIL: ${failure_count} required inventory conditions are open" >&2
  exit 1
fi

echo "[cpp-e2e-inventory] PASS: every common ID is either implemented with feature/source evidence or explicitly unimplemented"
