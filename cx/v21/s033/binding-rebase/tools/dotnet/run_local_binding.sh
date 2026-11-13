#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet"
TOOLS_DIR="/root/jobs/fx/cx/v21/s033/binding-rebase/tools/dotnet"
# shellcheck source=../runner_common.sh
source "${TOOLS_DIR}/runner_common.sh"
bench_init dotnet "grpc-dotnet,zlink-dotnet,zlink-framework-dotnet" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"

CONFIGURATION=Release
BENCH_TARGET_ROOT="${BENCH_TARGET_ROOT:-${ROOT_DIR}}"
target_process_prefix=()
source_process_prefix=()
if [[ -n "${TARGET_CPUS:-}" ]]; then
  target_process_prefix=(taskset -c "${TARGET_CPUS}")
fi
if [[ -n "${SOURCE_CPUS:-}" ]]; then
  source_process_prefix=(taskset -c "${SOURCE_CPUS}")
fi

if [[ "${SKIP_BUILD}" != 1 ]]; then
  "${ROOT_DIR}/build.sh"
fi

check_ports_free 5200 5219
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
    affinity="$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/"$pid"/status 2>/dev/null || true)"
    printf '%s_affinity pid=%s cpus=%s\n' "$label" "$pid" "${affinity:-unavailable}" >> "$BINDING_SCHED_FILE"
    for task in /proc/"$pid"/task/*; do
      [[ -d "$task" ]] || continue
      tid="${task##*/}"
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
    target_impl="${TARGET_IMPLEMENTATION:-${impl}}"
    case "${impl}" in
      grpc-dotnet)
        trigger_url="http://127.0.0.1:5200"
        source_stats_url="http://127.0.0.1:5201"
        ;;
      zlink-dotnet)
        trigger_url="http://127.0.0.1:5205"
        source_stats_url="http://127.0.0.1:5206"
        ;;
      zlink-framework-dotnet)
        trigger_url="http://127.0.0.1:5212"
        source_stats_url="http://127.0.0.1:5213"
        ;;
    esac
    case "${target_impl}" in
      grpc-dotnet)
        target_endpoint="http://127.0.0.1:5202"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5203"
        target_project="${BENCH_TARGET_ROOT}/GrpcServer/WithGrpcBench.GrpcServer.csproj"
        target_args=(--url "${target_endpoint}" --metrics-url "${target_stats_url}")
        ;;
      zlink-dotnet)
        target_endpoint="tcp://127.0.0.1:5207"
        target_command_endpoint="tcp://127.0.0.1:5208"
        target_stats_url="http://127.0.0.1:5209"
        target_project="${BENCH_TARGET_ROOT}/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj"
        target_args=(--endpoint "${target_endpoint}" --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
        ;;
      zlink-framework-dotnet)
        target_endpoint="tcp://127.0.0.1:5214"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5215"
        target_project="${BENCH_TARGET_ROOT}/ZLinkServer/WithGrpcBench.ZLinkServer.csproj"
        target_args=(--endpoint "${target_endpoint}" --metrics-url "${target_stats_url}")
        ;;
    esac
    if [[ "${impl}" == zlink-dotnet && "${target_impl}" == zlink-framework-dotnet ]]; then
      target_command_endpoint="${target_endpoint}"
    fi

    for pattern in "${patterns[@]}"; do
      bench_skip_cell "${impl}" "${pattern}" && continue
      for payload in "${payloads[@]}"; do
        if [[ "${target_impl}" == "${impl}" ]]; then
          cell_id="${impl}-${pattern}-${payload}"
        else
          cell_id="cross-${impl}-to-${target_impl}-${pattern}-${payload}"
        fi
        cell_dir="$(bench_cell_dir "${run}" "${impl}" "${pattern}" "${payload}")"
        mkdir -p "${cell_dir}"
        target_log="${cell_dir}/target.log"
        source_log="${cell_dir}/source.log"
        target_stats_file="${cell_dir}/target-stats.json"
        echo "[bench] cell=${cell_id} run=${run}: start B then A" >&2

        setsid "${target_process_prefix[@]}" dotnet run --no-build -c "${CONFIGURATION}" --project "${target_project}" -- "${target_args[@]}" >"${target_log}" 2>&1 &
        b_pid=$!
        wait_for_stats "${target_stats_url}" 0

        source_args=(
          --implementation "${impl}" --scenario "${pattern}" --payload-size "${payload}"
          --request-window "${WINDOW}" --submit-concurrency "${SUBMIT_CONCURRENCY}"
          --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
          --warmup-seconds "${WARMUP_SECONDS}" --drain-bound-ms "${DRAIN_BOUND_MS}"
          --trigger-url "${trigger_url}" --stats-url "${source_stats_url}"
          --target-endpoint "${target_endpoint}" --target-stats-url "${target_stats_url}"
          --raw-socket router
          --output "${cell_dir}" --report-file report.txt
          --configuration "${CONFIGURATION}" --timeout-seconds "${TIMEOUT_SECONDS}"
        )
        if [[ -n "${target_command_endpoint}" ]]; then
          source_args+=(--target-command-endpoint "${target_command_endpoint}")
        fi
        setsid "${source_process_prefix[@]}" dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/Client/WithGrpcBench.Client.csproj" -- "${source_args[@]}" >"${source_log}" 2>&1 &
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
        wait_for_ports_free 5200 5219
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
