# SPDX-License-Identifier: Elastic-2.0
# Sourced by run_cross_language_smoke.sh.

actor_destroy_scenario() {
  local caller="$1" owner="$2" scenario="$3"
  local redis_port owner_port caller_port run_id start_file role language port peer
  local -A host_pids=()
  read -r redis_port owner_port caller_port <<<"$(free_port 3)"
  run_id="actor-destroy-${caller}-${owner}-${scenario}"
  start_file="${RUN_DIR}/${run_id}.start"
  local common=(--mesh-name cross.actor-destroy --redis-endpoint "127.0.0.1:${redis_port}"
    --redis-key-prefix "zlink-cross-${run_id}" --actor-id "${run_id}"
    --actor-destroy-scenario "${scenario}" --actor-destroy-run-id "${run_id}"
    --start-file "${start_file}")

  start_redis "${run_id}-redis" "${redis_port}" || return 1
  "${PYTHON_BIN[@]}" "${SCRIPT_DIR}/actor_destroy_wire.py" "${RUN_DIR}" "${run_id}" \
    "${scenario}" "${owner_port}" "${caller_port}" "${ACTOR_DESTROY_CONTROL_DIR}" \
    >"${RUN_DIR}/${run_id}.wire.log" 2>&1 &
  PIDS+=("$!")
  wait_for_ready "${RUN_DIR}/${run_id}.wire.ready" 60 "$!" || return 1
  for role in owner caller; do
    if [[ "${role}" == owner ]]; then
      language="${owner}"
      port="${owner_port}"
      peer=caller
    else
      language="${caller}"
      port="${caller_port}"
      peer=owner
    fi
    start_cross_language_host "${language}" "${run_id}.${role}" "actor-destroy-${role}" "${common[@]}" \
      --node-rid "actor-destroy-${role}" \
      --peer-rid "actor-destroy-${peer}" \
      --bind-endpoint "tcp://127.0.0.2:${port}" --advertise-host 127.0.0.1 \
      --event-file "${RUN_DIR}/${run_id}.${role}.events" \
      --actor-destroy-control-prefix "${ACTOR_DESTROY_CONTROL_DIR}/${run_id}.${role}" || return 1
    host_pids[${role}]="$!"
  done
  for role in owner caller; do
    if ! wait_for_ready "${RUN_DIR}/${run_id}.${role}.ready" 180 "${host_pids[${role}]}"; then
      echo "${run_id}: ${role} did not report ready" >&2
      tail -40 "${RUN_DIR}/${run_id}.${role}.log" >&2
      return 1
    fi
  done
  touch "${start_file}"
  "${PYTHON_BIN[@]}" "${SCRIPT_DIR}/actor_destroy_stage.py" \
    "${REPO_ROOT}/framework/runtime/conformance/actor-destroy-v1.json" "${RUN_DIR}" \
    "${run_id}" "${scenario}" "${ACTOR_DESTROY_CONTROL_DIR}" "${caller}"
}

stage_actor_destroy() {
  local caller owner scenario started cell_started cell_failed cell_unexpected status result failed=0
  local matrix=() scenarios=(a b d e f admissionCheck) cells
  cells="$("${PYTHON_BIN[@]}" -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["crossLanguageCells"]))' \
    "${REPO_ROOT}/framework/runtime/conformance/actor-destroy-v1.json")" || return 1
  [[ -n "${cells}" ]] || { echo "Actor destroy fixture has no cells" >&2; return 1; }
  mapfile -t matrix <<< "${cells}"
  if [[ -n "${ZLINK_CPP_CROSS_ACTOR_DESTROY_CELL:-}" ]]; then
    local selected="${ZLINK_CPP_CROSS_ACTOR_DESTROY_CELL}" found=0
    for cell in "${matrix[@]}"; do
      [[ "${cell}" != "${selected}" ]] || found=1
    done
    if (( ! found )); then
      echo "unknown Actor destroy cell: ${selected}" >&2
      return 1
    fi
    matrix=("${selected}")
  fi
  case "${ZLINK_CPP_CROSS_ACTOR_DESTROY_SCENARIO:-}" in
    "") ;;
    a|b|d|e|f|admissionCheck) scenarios=("${ZLINK_CPP_CROSS_ACTOR_DESTROY_SCENARIO}") ;;
    *) echo "unknown Actor destroy scenario: ${ZLINK_CPP_CROSS_ACTOR_DESTROY_SCENARIO}" >&2; return 1 ;;
  esac
  # Command replacement needs local POSIX rename semantics while a host reads
  # the preceding command. Logs can still be kept on the requested run volume.
  ACTOR_DESTROY_CONTROL_DIR="$(mktemp -d /tmp/zlink-actor-destroy.XXXXXXXX)" || return
  for cell in "${matrix[@]}"; do
    IFS=: read -r caller owner <<<"${cell}"
    cell_started=${SECONDS}
    cell_failed=0
    cell_unexpected=0
    status="$("${PYTHON_BIN[@]}" -c 'import json,sys; print(json.load(open(sys.argv[1]))["crossLanguageCells"][sys.argv[2]])' \
      "${REPO_ROOT}/framework/runtime/conformance/actor-destroy-v1.json" "${cell}")" || return 1
    for scenario in "${scenarios[@]}"; do
      started=${SECONDS}
      result=0
      actor_destroy_scenario "${caller}" "${owner}" "${scenario}" || result=$?
      if (( result )); then
        if [[ "${status}" == expected-failure-before-4522 && "${result}" == 2 ]]; then
          cell_failed=1
          echo "Actor destroy ${caller} -> ${owner} (${scenario}): XFAIL" >&2
        else
          failed=1
          cell_unexpected=1
          echo "Actor destroy ${caller} -> ${owner} (${scenario}): FAIL" >&2
        fi
      fi
      stop_all
      echo "Actor destroy ${caller} -> ${owner} (${scenario}): $((SECONDS - started)) s"
    done
    if [[ "${status}" == expected-failure-before-4522 ]]; then
      if (( cell_failed && cell_unexpected )); then
        echo "Actor destroy ${caller} -> ${owner}: FAIL (includes ${status})" >&2
      elif (( cell_failed )); then
        echo "Actor destroy ${caller} -> ${owner}: XFAIL (${status})"
      elif [[ -z "${ZLINK_CPP_CROSS_ACTOR_DESTROY_SCENARIO:-}" && "${cell_unexpected}" == 0 ]]; then
        echo "Actor destroy ${caller} -> ${owner}: XPASS (${status})" >&2
        failed=1
      fi
    elif (( cell_failed )); then
      failed=1
    fi
    echo "Actor destroy ${caller} -> ${owner}: $((SECONDS - cell_started)) s"
  done
  return "${failed}"
}
