# SPDX-License-Identifier: Elastic-2.0
# Sourced by run_cross_language_smoke.sh.

start_actor_destroy_host() {
  local language="$1" name="$2" role="$3"
  shift 3
  case "${language}" in
    cpp) start_cpp "${name}" "actor-destroy-${role}" "$@" --ready-file "${RUN_DIR}/${name}.ready" ;;
    dotnet) start_dotnet "${name}" "actor-destroy-${role}" "$@" ;;
    java) start_java "${name}" "actor-destroy-${role}" "$@" ;;
    node) start_node_user_spot_join "${name}" "actor-destroy-${role}" "$@" ;;
    *) echo "unknown Actor destroy host: ${language}" >&2; return 1 ;;
  esac
}

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

  start_redis "${run_id}-redis" "${redis_port}" || return
  "${PYTHON_BIN[@]}" "${SCRIPT_DIR}/actor_destroy_wire.py" "${RUN_DIR}" "${run_id}" \
    "${scenario}" "${owner_port}" "${caller_port}" "${ACTOR_DESTROY_CONTROL_DIR}" \
    >"${RUN_DIR}/${run_id}.wire.log" 2>&1 &
  PIDS+=("$!")
  wait_for_ready "${RUN_DIR}/${run_id}.wire.ready" || return
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
    start_actor_destroy_host "${language}" "${run_id}.${role}" "${role}" "${common[@]}" \
      --node-rid "actor-destroy-${role}" \
      --peer-rid "actor-destroy-${peer}" \
      --bind-endpoint "tcp://127.0.0.2:${port}" --advertise-host 127.0.0.1 \
      --event-file "${RUN_DIR}/${run_id}.${role}.events" \
      --actor-destroy-control-prefix "${ACTOR_DESTROY_CONTROL_DIR}/${run_id}.${role}" || return
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
    "${run_id}" "${scenario}" "${ACTOR_DESTROY_CONTROL_DIR}"
}

stage_actor_destroy() {
  local caller owner scenario started cell_started failed=0
  local matrix=(cpp:dotnet dotnet:java java:node node:cpp)
  if [[ "${ZLINK_CPP_CROSS_ACTOR_DESTROY_CPP_ONLY:-}" == 1 ]]; then matrix=(cpp:cpp); fi
  local scenarios=(a b d e f admissionCheck)
  case "${ZLINK_CPP_CROSS_ACTOR_DESTROY_CELL:-}" in
    "") ;;
    cpp:dotnet|dotnet:java|java:node|node:cpp|cpp:cpp)
      matrix=("${ZLINK_CPP_CROSS_ACTOR_DESTROY_CELL}")
      ;;
    *) echo "unknown Actor destroy cell: ${ZLINK_CPP_CROSS_ACTOR_DESTROY_CELL}" >&2; return 1 ;;
  esac
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
    for scenario in "${scenarios[@]}"; do
      started=${SECONDS}
      if ! actor_destroy_scenario "${caller}" "${owner}" "${scenario}"; then
        failed=1
        echo "Actor destroy ${caller} -> ${owner} (${scenario}): FAIL" >&2
      fi
      stop_all
      echo "Actor destroy ${caller} -> ${owner} (${scenario}): $((SECONDS - started)) s"
    done
    echo "Actor destroy ${caller} -> ${owner}: $((SECONDS - cell_started)) s"
  done
  return "${failed}"
}
