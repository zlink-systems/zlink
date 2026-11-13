#!/usr/bin/env bash
set -euo pipefail

HERE="/root/worktree/zlink-v21-s033/aa-noise/framework/bench/grpc/java"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
AA_TOOLS="${AA_TOOLS:-$(cd "${HERE}/../../../.." && pwd)/tools}"
source "${AA_TOOLS}/aa-measure-lib.sh"
bench_init java "zlink-java" \
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
        target_command=("${RAW_BIN}" --endpoint "${target_endpoint}" \
          --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
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

        target_process=("${target_command[@]}")
        if [[ -n "${TARGET_CPUS:-}" ]]; then target_process=(taskset -c "${TARGET_CPUS}" "${target_process[@]}"); fi
        setsid "${target_process[@]}" >"${target_log}" 2>&1 &
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
        source_process=("${SOURCE_BIN}" "${source_args[@]}")
        if [[ -n "${SOURCE_CPUS:-}" ]]; then source_process=(taskset -c "${SOURCE_CPUS}" "${source_process[@]}"); fi
        setsid "${source_process[@]}" >"${source_log}" 2>&1 &
        a_pid=$!
        record_sched_snapshot() {
          local label="$1" pid="$2" env_file="$3" tid
          { printf "\n%s_sched_pid=%s\n" "$label" "$pid";
            for task in /proc/"$pid"/task/*; do
              [[ -d "$task" ]] || continue; tid="${task##*/}";
              printf "%s_tid=%s\n" "$label" "$tid" >> "$env_file"
              policy="$(chrt -p "$tid" 2>&1 || true)"
              printf '%s\n' "$policy" >> "$env_file"
              if ! grep -q 'SCHED_OTHER' <<<"$policy"; then printf 'scheduling_invalid=%s:%s\n' "$label" "$tid" >> "$env_file"; fi
            done;
          } >> "$env_file"
        }
        sched_file="$AA_ENV_DIR/${AA_CELL}-${AA_SIDE}-run${AA_INDEX}.txt"
        mkdir -p "$(dirname "$sched_file")"
        printf "run_start_utc=%s\nsource_pid=%s\ntarget_pid=%s\n" "$(date -u +%FT%TZ)" "$a_pid" "$b_pid" > "$sched_file"
        record_sched_snapshot source_start "$a_pid" "$sched_file"
        record_sched_snapshot target_start "$b_pid" "$sched_file"
        if grep -q '^scheduling_invalid=' "$sched_file"; then
          printf 'run_invalid_reason=start scheduling policy is not SCHED_OTHER\n' >> "$sched_file"
          printf 'invalid_start_loadavg=' >> "$sched_file"; cat /proc/loadavg >> "$sched_file"
          printf 'invalid_start_top_processes_begin\n' >> "$sched_file"
          ps -eo pid,cls,%cpu,stat,args --sort=-%cpu | head -45 >> "$sched_file"
          printf 'invalid_start_top_processes_end\n' >> "$sched_file"
          for cpu in 0 8; do
            for state in 2 3; do
              state_dir="/sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}"
              printf 'cpu%s.state%s.disable_at_invalid_start=%s\n' "$cpu" "$state" "$(cat "$state_dir/disable" 2>/dev/null || echo unavailable)" >> "$sched_file"
            done
          done
          echo "[bench] invalid scheduling policy at startup; skipped active phase: $sched_file" >&2
          cleanup_cell
          exit 0
        fi
        wait_for_stats "${source_stats_url}" 1

        export BENCH_SOURCE_PID="$a_pid" BENCH_TARGET_PID="$b_pid"
        aa_bench_run_cell "${trigger_url}" "${source_stats_url}" "${target_stats_url}" \
          "${run_id}" "${cell_id}" "${impl}" "${pattern}" "${payload}" \
          "$((WARMUP_SECONDS * 1000))" "${cell_dir}/results.json" "${target_stats_file}"

        cleanup_cell
        wait_for_ports_free 5240 5259
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
