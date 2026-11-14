#!/usr/bin/env bash
set -euo pipefail

HERE="/root/worktree/zlink-v21-s033/nbind/framework/bench/grpc/node"
REPO="$(cd "${HERE}/../../../.." && pwd)"
# shellcheck source=../runner_common.sh
source "/root/jobs/fx/cx/v21/s033/nbind/tools/node/runner_common.sh"
bench_init node "grpc-node,zlink-node,zlink-framework-node" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"
cd "${HERE}"

export NODE_PATH="${REPO}/framework/languages/node/node_modules${NODE_PATH:+:${NODE_PATH}}"

# Requests sent before the measured window (README §8.2).
NODE_PORT_LOW=5220
NODE_PORT_HIGH=5239

if [[ "${SKIP_BUILD}" != 1 ]]; then
  "${HERE}/build.sh"
fi

check_ports_free "${NODE_PORT_LOW}" "${NODE_PORT_HIGH}"
mkdir -p "${OUTPUT_DIR}"
b_pid=""
a_pid=""

trap cleanup_cell EXIT
binding_sched_snapshot() {
  local label="$1" root_pid="$2"
  record_sched_process() {
    local pid="$1" tid policy child
    [[ -d /proc/"$pid" ]] || return 0
    printf '%s_process_pid=%s\n' "$label" "$pid" >> "$BINDING_SCHED_FILE"
    for task in /proc/"$pid"/task/*; do
      [[ -d "$task" ]] || continue
      tid="${task##*/}"
      affinity="$(awk '/^Cpus_allowed_list:/ {print $2}' "$task/status" 2>/dev/null || true)"
      printf '%s_affinity pid=%s tid=%s cpus=%s\n' "$label" "$pid" "$tid" "${affinity:-unavailable}" >> "$BINDING_SCHED_FILE"
      printf '%s_tid=%s\n' "$label" "$tid" >> "$BINDING_SCHED_FILE"
      policy="$(chrt -p "$tid" 2>&1 || true)"
      printf '%s\n' "$policy" >> "$BINDING_SCHED_FILE"
      if ! grep -q 'current scheduling policy: SCHED_OTHER' <<<"$policy"; then printf 'scheduling_invalid=%s:%s\n' "$label" "$tid" >> "$BINDING_SCHED_FILE"; fi
    done
    for child in $(pgrep -P "$pid" 2>/dev/null || true); do record_sched_process "$child"; done
  }
  { printf '\n%s_utc=%s\n%s_pid=%s\n' "$label" "$(date -u +%FT%TZ)" "$label" "$root_pid"; } >> "$BINDING_SCHED_FILE"
  record_sched_process "$root_pid"
}
binding_process_split_ok() {
  local pid="$1" expected="$2" tid actual child
  [[ -r /proc/"$pid"/status ]] || return 0
  for task in /proc/"$pid"/task/*; do
    [[ -d "$task" ]] || continue
    tid="${task##*/}"
    actual="$(awk '/^Cpus_allowed_list:/ {print $2}' "$task/status" 2>/dev/null || true)"
    [[ "$actual" == "$expected" ]] || return 1
  done
  for child in $(pgrep -P "$pid" 2>/dev/null || true); do binding_process_split_ok "$child" "$expected" || return 1; done
}
binding_wait_for_cpu_split() {
  local ready=0
  for _ in $(seq 1 120); do
    if binding_process_split_ok "$BINDING_SOURCE_PID" '0-5' && binding_process_split_ok "$BINDING_TARGET_PID" '8-13'; then ready=1; break; fi
    sleep 0.25
  done
  [[ "$ready" == 1 ]] || { echo 'quiet guard did not narrow source/target CPU affinity to 0-5 / 8-13 before warmup' >&2; return 1; }
}
binding_sched_start() {
  local cpu state val
  : > "$BINDING_SCHED_FILE"
  binding_wait_for_cpu_split
  printf 'run_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$BINDING_SCHED_FILE"
  for cpu in 0 8; do for state in 2 3; do
    val="$(cat "/sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}/disable" 2>/dev/null || echo unavailable)"
    printf 'cpu%s.state%s.disable_at_run_start=%s\n' "$cpu" "$state" "$val" >> "$BINDING_SCHED_FILE"
    [[ "$val" == 1 ]] || { echo "C-state disable check failed cpu=$cpu state=$state value=$val" >&2; return 1; }
  done; done
  binding_sched_snapshot source_start "$BINDING_SOURCE_PID"
  binding_sched_snapshot target_start "$BINDING_TARGET_PID"
}


for run in $(seq 1 "${RUNS}"); do
  run_id="${RUN_STAMP}-run${run}"
  for impl in "${implementations[@]}"; do
    target_impl="${BENCH_TARGET_SERVER:-${impl}}"
    case "${target_impl}" in
      grpc-node)
        trigger_url="http://127.0.0.1:5220"
        source_stats_url="http://127.0.0.1:5221"
        target_endpoint="127.0.0.1:5222"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5223"
        target_command=(node grpc-server/main.js --url "${target_endpoint}" --metrics-url "${target_stats_url}")
        ;;
      zlink-node)
        trigger_url="http://127.0.0.1:5225"
        source_stats_url="http://127.0.0.1:5226"
        target_endpoint="tcp://127.0.0.1:5227"
        target_command_endpoint="tcp://127.0.0.1:5228"
        target_stats_url="http://127.0.0.1:5229"
        target_command=(node zlink-raw-server/main.js --endpoint "${target_endpoint}" \
          --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
        ;;
      zlink-framework-node)
        trigger_url="http://127.0.0.1:5232"
        source_stats_url="http://127.0.0.1:5233"
        target_endpoint="tcp://127.0.0.1:5234"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5235"
        target_command=(node zlink-framework-server/main.js --endpoint "${target_endpoint}" \
          --metrics-url "${target_stats_url}")
        l0_two_routers=0
        if [[ "${ZLINK_LADDER_L0_SERVER:-0}" == 1 && "${ZLINK_LADDER_L0_TWO_ROUTERS:-1}" != 0 ]]; then
          l0_two_routers=1
          target_command_endpoint="tcp://127.0.0.1:5236"
          target_command+=(--command-endpoint "${target_command_endpoint}")
        fi
        ;;
    esac
    if [[ "${impl}" == zlink-node && "${target_impl}" == zlink-framework-node ]]; then
      if [[ "${l0_two_routers:-0}" != 1 ]]; then
        target_command_endpoint="${target_endpoint}"
      fi
    fi

    for pattern in "${patterns[@]}"; do
      bench_skip_cell "${impl}" "${pattern}" && continue
      for payload in "${payloads[@]}"; do
        cell_id="${impl}-${pattern}-${payload}"
        cell_dir="$(bench_cell_dir "${run}" "${impl}" "${pattern}" "${payload}")"
        mkdir -p "${cell_dir}"
        target_log="${cell_dir}/target.log"
        source_log="${cell_dir}/source.log"
        target_stats_file="${cell_dir}/target-stats.json"
        echo "[bench] cell=${cell_id} run=${run} starting target B then source A" >&2

        setsid taskset -c 8-13 "${target_command[@]}" >"${target_log}" 2>&1 &
        b_pid=$!
        wait_for_stats "${target_stats_url}" 0

        source_args=(
          --implementation "${impl}"
          --scenario "${pattern}"
          --payload-size "${payload}"
          --request-window "${WINDOW}"
          --submit-concurrency "${SUBMIT_CONCURRENCY}"
          --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
          --warmup-seconds "${WARMUP_SECONDS}"
          --drain-bound-ms "${DRAIN_BOUND_MS}"
          --request-timeout-ms "${REQUEST_TIMEOUT_MS}"
          --route-ready-ms "${ROUTE_READY_MS}"
          --trigger-url "${trigger_url}"
          --stats-url "${source_stats_url}"
          --target-endpoint "${target_endpoint}"
          --target-stats-url "${target_stats_url}"
          --raw-socket router
          --output "${cell_dir}"
          --report-file report.txt
        )
        if [[ -n "${target_command_endpoint}" ]]; then
          source_args+=(--target-command-endpoint "${target_command_endpoint}")
        fi
        setsid taskset -c 0-5 node client/main.js "${source_args[@]}" >"${source_log}" 2>&1 &
        a_pid=$!
        wait_for_stats "${source_stats_url}" 1
        BINDING_SCHED_FILE="${cell_dir}/scheduling.txt"
        BINDING_SOURCE_PID="$a_pid"
        BINDING_TARGET_PID="$b_pid"
        binding_sched_start

        bench_run_cell "${trigger_url}" "${source_stats_url}" "${target_stats_url}" \
          "${run_id}" "${cell_id}" "${impl}" "${pattern}" "${payload}" \
          "$((WARMUP_SECONDS * 1000))" "${cell_dir}/results.json" "${target_stats_file}"

        cleanup_cell
        wait_for_ports_free "${NODE_PORT_LOW}" "${NODE_PORT_HIGH}"
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
