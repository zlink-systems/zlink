#!/usr/bin/env bash
set -euo pipefail

HERE="/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java"
TOOLS_DIR="/root/jobs/fx/cx/v21/s033/binding-rebase/tools/java"
# shellcheck source=../runner_common.sh
source "${TOOLS_DIR}/runner_common.sh"
bench_init java "grpc-java,zlink-java,zlink-framework-java" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"
cd "${HERE}"

select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

if [[ "${SKIP_BUILD}" != 1 ]]; then
  "${HERE}/build.sh"
fi

GRPC_BIN="${HERE}/grpc-server/build/install/bench-grpc-server/bin/bench-grpc-server"
RAW_BIN="${HERE}/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
FW_BIN="${HERE}/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
SOURCE_BIN="${HERE}/client/build/install/bench-client/bin/bench-client"
for binary in "${GRPC_BIN}" "${RAW_BIN}" "${FW_BIN}" "${SOURCE_BIN}"; do
  [[ -x "${binary}" ]] || { echo "missing ${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

check_ports_free 5240 5259
mkdir -p "${OUTPUT_DIR}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

binding_sched_snapshot() {
  local label="$1" pid="$2" tid policy
  { printf '\n%s_utc=%s\n%s_pid=%s\n' "$label" "$(date -u +%FT%TZ)" "$label" "$pid"
    for task in /proc/"$pid"/task/*; do
      [[ -d "$task" ]] || continue
      tid="${task##*/}"
      printf '%s_tid=%s\n' "$label" "$tid"
      policy="$(chrt -p "$tid" 2>&1 || true)"
      printf '%s\n' "$policy"
      if ! grep -q 'current scheduling policy: SCHED_OTHER' <<<"$policy"; then printf 'scheduling_invalid=%s:%s\n' "$label" "$tid"; fi
    done
  } >> "$BINDING_SCHED_FILE"
}
binding_sched_start() {
  local cpu state val
  : > "$BINDING_SCHED_FILE"
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
    case "${impl}" in
      grpc-java)
        trigger_url="http://127.0.0.1:5240"
        source_stats_url="http://127.0.0.1:5241"
        target_endpoint="127.0.0.1:5242"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5243"
        target_command=("${GRPC_BIN}" --port 5242 --metrics-url "${target_stats_url}")
        ;;
      zlink-java)
        trigger_url="http://127.0.0.1:5245"
        source_stats_url="http://127.0.0.1:5246"
        target_endpoint="tcp://127.0.0.1:5247"
        target_command_endpoint="tcp://127.0.0.1:5248"
        target_stats_url="http://127.0.0.1:5249"
        if [[ "${ZLINK_BENCH_CROSS_FRAMEWORK_SERVER:-}" == 1 ]]; then
          target_endpoint="tcp://127.0.0.1:5254"
          target_command_endpoint=""
          target_stats_url="http://127.0.0.1:5255"
          target_command=("${FW_BIN}" --endpoint "${target_endpoint}" --metrics-url "${target_stats_url}")
        else
          target_command=("${RAW_BIN}" --endpoint "${target_endpoint}" \
            --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
        fi
        ;;
      zlink-framework-java)
        trigger_url="http://127.0.0.1:5252"
        source_stats_url="http://127.0.0.1:5253"
        target_endpoint="tcp://127.0.0.1:5254"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5255"
        target_command=("${FW_BIN}" --endpoint "${target_endpoint}" --metrics-url "${target_stats_url}")
        ;;
    esac

    for pattern in "${patterns[@]}"; do
      bench_skip_cell "${impl}" "${pattern}" && continue
      for payload in "${payloads[@]}"; do
        cell_id="${impl}-${pattern}-${payload}"
        cell_dir="$(bench_cell_dir "${run}" "${impl}" "${pattern}" "${payload}")"
        mkdir -p "${cell_dir}"
        target_log="${cell_dir}/target.log"
        source_log="${cell_dir}/source.log"
        target_stats_file="${cell_dir}/target-stats.json"
        echo "[bench] cell=${cell_id} run=${run}: start B then A" >&2

        setsid "${target_command[@]}" >"${target_log}" 2>&1 &
        b_pid=$!
        wait_for_stats "${target_stats_url}" 0

        source_args=(
          --implementation "${impl}" --scenario "${pattern}" --payload-size "${payload}"
          --request-window "${WINDOW}" --submit-concurrency "${SUBMIT_CONCURRENCY}"
          --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
          --warmup-seconds "${WARMUP_SECONDS}" --drain-bound-ms "${DRAIN_BOUND_MS}"
          --request-timeout-ms "${REQUEST_TIMEOUT_MS}" --route-ready-ms "${ROUTE_READY_MS}"
          --trigger-url "${trigger_url}" --stats-url "${source_stats_url}"
          --target-endpoint "${target_endpoint}" --target-stats-url "${target_stats_url}"
          --run-id "${run_id}" --cell-id "${cell_id}" --raw-socket router
          --output "${cell_dir}" --report-file report.txt
        )
        if [[ -n "${target_command_endpoint}" ]]; then
          source_args+=(--target-command-endpoint "${target_command_endpoint}")
        fi
        setsid "${SOURCE_BIN}" "${source_args[@]}" >"${source_log}" 2>&1 &
        a_pid=$!
        BINDING_SCHED_FILE="${cell_dir}/scheduling.txt"
        BINDING_SOURCE_PID="$a_pid"
        BINDING_TARGET_PID="$b_pid"
        wait_for_stats "${source_stats_url}" 1
        binding_sched_start

        bench_run_cell "${trigger_url}" "${source_stats_url}" "${target_stats_url}" \
          "${run_id}" "${cell_id}" "${impl}" "${pattern}" "${payload}" \
          "$((WARMUP_SECONDS * 1000))" "${cell_dir}/results.json" "${target_stats_file}"

        cleanup_cell
        wait_for_ports_free 5240 5259
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
