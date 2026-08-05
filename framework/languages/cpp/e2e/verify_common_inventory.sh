#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
COMMON_E2E_DIR="${REPO_ROOT}/framework/doc/framework/common/e2e"

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
  rg '^#### ' "$1" | rg -o "$ID_PATTERN" | sort -u || true
}

extract_feature_ids() {
  rg -o "$ID_PATTERN" "$1" | sort -u || true
}

extract_source_ids() {
  local config_dir="$1"
  rg -o "$ID_PATTERN" \
    --no-filename \
    --glob '!*.md' \
    --glob '!*.json' \
    --glob '!*.log' \
    --glob '!logs/**' \
    --glob '!build/**' \
    --glob '!artifacts/**' \
    "$config_dir" | sort -u || true
}

failure_count=0
config_count=0
scenario_count=0
feature_missing_count=0
source_missing_count=0
status_gap_count=0

for config in "${CONFIGS[@]}"; do
  IFS=: read -r number directory document <<<"${config}"
  config_count=$((config_count + 1))
  common_document="${COMMON_E2E_DIR}/${document}.en.md"
  cpp_directory="${SCRIPT_DIR}/${directory}"
  feature_map="${cpp_directory}/feature-map.ko.md"
  runner="${cpp_directory}/run_e2e.sh"

  if [[ ! -f "${common_document}" ]]; then
    echo "[cpp-e2e-inventory] missing common document: ${common_document}" >&2
    failure_count=$((failure_count + 1))
    continue
  fi

  mapfile -t common_ids < <(extract_common_ids "${common_document}")
  scenario_count=$((scenario_count + ${#common_ids[@]}))
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
    mapfile -t missing_feature_ids < <(
      comm -23 \
        <(printf '%s\n' "${common_ids[@]}") \
        <(printf '%s\n' "${feature_ids[@]}")
    )
    if [[ "${#missing_feature_ids[@]}" -gt 0 ]]; then
      echo " feature-map-missing=${#missing_feature_ids[@]}"
      printf '  missing feature-map IDs: %s\n' "${missing_feature_ids[*]}" >&2
      failure_count=$((failure_count + ${#missing_feature_ids[@]}))
      feature_missing_count=$((feature_missing_count + ${#missing_feature_ids[@]}))
    else
      printf ' feature-map=complete'
    fi

    for scenario_id in "${common_ids[@]}"; do
      feature_row="$(rg -n --pcre2 \
        "(^|[^A-Za-z0-9_])${scenario_id}([^A-Za-z0-9_]|$)" \
        "${feature_map}" || true)"
      if [[ "${feature_row}" =~ 미구현|부분|blocked|deferred|component[[:space:]]+only|not-supported ]]; then
        printf '  incomplete feature-map status: %s\n' "${scenario_id}" >&2
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
    mapfile -t missing_source_ids < <(
      comm -23 \
        <(printf '%s\n' "${common_ids[@]}") \
        <(printf '%s\n' "${source_ids[@]}")
    )
    if [[ "${#missing_source_ids[@]}" -gt 0 ]]; then
      echo " source-missing=${#missing_source_ids[@]}"
      printf '  missing source/runner IDs: %s\n' "${missing_source_ids[*]}" >&2
      failure_count=$((failure_count + ${#missing_source_ids[@]}))
      source_missing_count=$((source_missing_count + ${#missing_source_ids[@]}))
    else
      echo " source=referenced"
    fi
  fi
done

printf '[cpp-e2e-inventory] configs=%d scenarios=%d feature-map-missing=%d source-missing=%d incomplete-status=%d\n' \
  "${config_count}" "${scenario_count}" "${feature_missing_count}" \
  "${source_missing_count}" "${status_gap_count}"

if [[ "${scenario_count}" -ne 374 ]]; then
  echo "[cpp-e2e-inventory] common scenario inventory changed: expected 374" >&2
  failure_count=$((failure_count + 1))
fi

if [[ "${failure_count}" -ne 0 ]]; then
  echo "[cpp-e2e-inventory] FAIL: ${failure_count} required inventory conditions are open" >&2
  exit 1
fi

echo "[cpp-e2e-inventory] PASS: all common configs, IDs, source references and statuses are complete"
