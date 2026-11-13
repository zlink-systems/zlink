#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="/root/worktree/zlink-v21-s033/i3871/framework/bench/grpc/java"
source "${BENCH}/../runner_common.sh"
impl="$1"; payload="$2"; runno="$3"; out="$4"
bench_init java "grpc-java,zlink-java,zlink-framework-java" "request-serial" "1024,4096"
cd "${BENCH}"
select_java_home; export PATH="${JAVA_HOME}/bin:${PATH}"
GRPC_BIN="${BENCH}/grpc-server/build/install/bench-grpc-server/bin/bench-grpc-server"
RAW_BIN="${BENCH}/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
FW_BIN="${BENCH}/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
SOURCE_BIN="${BENCH}/client/build/install/bench-client/bin/bench-client"
case "$impl" in
 zlink-java) trigger_url=http://127.0.0.1:5245; source_stats_url=http://127.0.0.1:5246; target_endpoint=tcp://127.0.0.1:5247; target_command_endpoint=tcp://127.0.0.1:5248; target_stats_url=http://127.0.0.1:5249; target_command=("${RAW_BIN}" --endpoint "$target_endpoint" --command-endpoint "$target_command_endpoint" --metrics-url "$target_stats_url");;
 zlink-framework-java) trigger_url=http://127.0.0.1:5252; source_stats_url=http://127.0.0.1:5253; target_endpoint=tcp://127.0.0.1:5254; target_command_endpoint=; target_stats_url=http://127.0.0.1:5255; target_command=("${FW_BIN}" --endpoint "$target_endpoint" --metrics-url "$target_stats_url");;
 *) echo "bad implementation: $impl" >&2; exit 2;;
esac
check_ports_free 5240 5259
cell="${impl}-request-serial-${payload}"
cell_dir="${out}/round${runno}/${cell}"
mkdir -p "$cell_dir"
a_pid=""; b_pid=""; trap cleanup_cell EXIT
if [[ "${FLOW_TRACE:-0}" == 1 && "$impl" == zlink-framework-java ]]; then target_command=(env BENCH_FLOW_TRACE=1 "${target_command[@]}"); fi
setsid taskset -c 8-15 "${target_command[@]}" >"$cell_dir/target.log" 2>&1 & b_pid=$!
wait_for_stats "$target_stats_url" 0
source_args=(--implementation "$impl" --scenario request-serial --payload-size "$payload" --request-window "$WINDOW" --send-concurrency "$SEND_CONCURRENCY" --latency-sample-limit "$LATENCY_SAMPLE_LIMIT" --warmup-seconds "$WARMUP_SECONDS" --drain-bound-ms "$DRAIN_BOUND_MS" --request-timeout-ms "$REQUEST_TIMEOUT_MS" --route-ready-ms "$ROUTE_READY_MS" --trigger-url "$trigger_url" --stats-url "$source_stats_url" --target-endpoint "$target_endpoint" --target-stats-url "$target_stats_url" --run-id "i3871-${runno}" --cell-id "$cell" --raw-socket router --output "$cell_dir" --report-file report.txt)
[[ -z "$target_command_endpoint" ]] || source_args+=(--target-command-endpoint "$target_command_endpoint")
if [[ "${FLOW_TRACE:-0}" == 1 && "$impl" == zlink-framework-java ]]; then setsid taskset -c 0-7 env BENCH_FLOW_TRACE=1 "${SOURCE_BIN}" "${source_args[@]}" >"$cell_dir/source.log" 2>&1 & else setsid taskset -c 0-7 "${SOURCE_BIN}" "${source_args[@]}" >"$cell_dir/source.log" 2>&1 & fi; a_pid=$!
wait_for_stats "$source_stats_url" 1
trigger_phase "$trigger_url" "i3871-${runno}" "$cell" request-serial "$payload" warmup "$((WARMUP_SECONDS*1000))" >/dev/null
wait_for_idle "$source_stats_url"
settle_and_capture "$source_stats_url" "$target_stats_url" /dev/null allPhaseMessages
[[ "$SETTLE_BOUND_HIT" != true ]] || { echo 'warmup settle bound hit' >&2; exit 1; }
python3 "$HERE/snapshot.py" "$cell_dir/capture" before "$a_pid" "$b_pid"
trigger_phase "$trigger_url" "i3871-${runno}" "$cell" request-serial "$payload" active "$((DURATION_SECONDS*1000))" >/dev/null
python3 "$HERE/snapshot.py" "$cell_dir/capture" after "$a_pid" "$b_pid"
wait_for_idle "$source_stats_url"
[[ -s "$cell_dir/results.json" ]] || { echo "missing result $cell_dir/results.json" >&2; exit 1; }
settle_and_capture "$source_stats_url" "$target_stats_url" "$cell_dir/target-stats.json" received
merge_target_stats "$cell_dir/results.json" "$cell_dir/target-stats.json" "$SETTLE_MS" "$SETTLE_BOUND_HIT"
verify_request_counts "$cell_dir/results.json"
printf '%s\n' "$cell_dir/results.json"
