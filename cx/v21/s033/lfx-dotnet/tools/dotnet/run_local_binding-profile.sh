#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="/root/worktree/zlink-v21-s033/lfx-dotnet/framework/bench/grpc/dotnet"
TOOLS_DIR="/root/jobs/fx/cx/v21/s033/lfx-dotnet/tools/dotnet"
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
    for task in /proc/"$pid"/task/*; do
      [[ -r "$task/status" ]] || continue
      tid="${task##*/}"
      affinity="$(awk '/^Cpus_allowed_list:/ {print $2}' "$task/status" 2>/dev/null || true)"
      policy="$(chrt -p "$tid" 2>&1 || true)"
      if [[ ! -d /proc/$tid ]]; then
        printf '%s_thread_exited_during_snapshot tid=%s\n' "$label" "$tid" >> "$BINDING_SCHED_FILE"
        continue
      fi
      printf '%s_tid=%s\n' "$label" "$tid" >> "$BINDING_SCHED_FILE"
      printf '%s_affinity tid=%s cpus=%s\n' "$label" "$tid" "${affinity:-unavailable}" >> "$BINDING_SCHED_FILE"
      if [[ "$label" == source_start || "$label" == mid_active_source ]]; then expected=6-7; else expected=14-15; fi
      binding_cpu_list_allowed "$affinity" "$expected" || printf 'affinity_invalid=%s:%s:%s\n' "$label" "$tid" "${affinity:-unavailable}" >> "$BINDING_SCHED_FILE"
      printf '%s\n' "$policy" >> "$BINDING_SCHED_FILE"
      if ! grep -q 'current scheduling policy: SCHED_OTHER' <<<"$policy"; then printf 'scheduling_invalid=%s:%s\n' "$label" "$tid" >> "$BINDING_SCHED_FILE"; fi
    done
    for child in $(pgrep -P "$pid" 2>/dev/null || true); do record_sched_process "$child"; done
  }
  { printf '\n%s_utc=%s\n%s_pid=%s\n' "$label" "$(date -u +%FT%TZ)" "$label" "$root_pid"; } >> "$BINDING_SCHED_FILE"
  record_sched_process "$root_pid"
}
binding_cpu_list_allowed() {
  local actual="$1" expected="$2" low high part first last
  IFS='-' read -r low high <<<"$expected"
  IFS=',' read -r -a parts <<<"$actual"
  ((${#parts[@]} > 0)) || return 1
  for part in "${parts[@]}"; do
    if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then first=${BASH_REMATCH[1]}; last=${BASH_REMATCH[2]}
    elif [[ "$part" =~ ^[0-9]+$ ]]; then first="$part"; last="$part"
    else return 1
    fi
    (( first >= low && last <= high && first <= last )) || return 1
  done
}
binding_process_split_ok() {
  local pid="$1" expected="$2" tid actual child
  [[ -r /proc/"$pid"/status ]] || return 0
  for task in /proc/"$pid"/task/*; do
    [[ -d "$task" ]] || continue
    tid="${task##*/}"
    actual="$(awk '/^Cpus_allowed_list:/ {print $2}' "$task/status" 2>/dev/null || true)"
    binding_cpu_list_allowed "$actual" "$expected" || return 1
  done
  for child in $(pgrep -P "$pid" 2>/dev/null || true); do binding_process_split_ok "$child" "$expected" || return 1; done
}
binding_wait_for_cpu_split() {
  local ready=0
  for _ in $(seq 1 120); do
    if binding_process_split_ok "$BINDING_SOURCE_PID" '6-7' && binding_process_split_ok "$BINDING_TARGET_PID" '14-15'; then ready=1; break; fi
    sleep 0.25
  done
  if [[ "$ready" != 1 ]]; then
    for role in source target; do
      if [[ "$role" == source ]]; then pid="$BINDING_SOURCE_PID"; else pid="$BINDING_TARGET_PID"; fi
      {
        printf 'affinity_wait_failure role=%s root_pid=%s\n' "$role" "$pid"
        dump_process_tree() {
          local process="$1" child
          [[ -d /proc/$process ]] || return 0
          printf 'process pid=%s cmd=' "$process"; tr '\0' ' ' </proc/$process/cmdline; printf '\n'
          for task in /proc/$process/task/*; do
            [[ -d "$task" ]] || continue
            printf 'thread tid=%s ' "${task##*/}"
            awk '/^Cpus_allowed_list:/ {printf "cpus=%s\n", $2}' "$task/status"
          done
          for child in $(pgrep -P "$process" 2>/dev/null || true); do dump_process_tree "$child"; done
        }
        dump_process_tree "$pid"
      } >>"$BINDING_SCHED_FILE"
    done
    echo 'quiet guard did not narrow source/target CPU affinity to 6-7 / 14-15 before warmup' >&2
    return 1
  fi
}
binding_sched_start() {
  local cpu state val
  : > "$BINDING_SCHED_FILE"
  binding_wait_for_cpu_split
  printf 'run_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$BINDING_SCHED_FILE"
  for cpu in 0 8; do for state in 2 3; do
    val="$(cat "/sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}/disable" 2>/dev/null || echo unavailable)"
    printf 'cpu%s.state%s.disable_at_run_start=%s\n' "$cpu" "$state" "$val" >> "$BINDING_SCHED_FILE"
    : # profile-only qualitative run; no throughput verdict
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
