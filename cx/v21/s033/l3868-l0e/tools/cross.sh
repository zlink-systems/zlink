#!/usr/bin/env bash
set -euo pipefail
REPO=/root/worktree/zlink-v21-s033/l3868
BUILD_DIR="$REPO/build-l3868"
JOB=/root/jobs/fx/cx/v21/s033/l3868-l0e
source "$REPO/framework/bench/grpc/runner_common.sh"
source "$JOB/tools/cell-controls.sh"
ROOT_OUTPUT="$OUTPUT_DIR"
LEVEL="${ZLINK_CPP_LADDER_LEVEL:?}"
[[ "$LEVEL" == 0 ]] || { echo "main/control measurements disabled by PM 08:30 UTC" >&2; exit 1; }
check_ports_free 5280 5299
a_pid=""; b_pid=""
trap cleanup_cell EXIT
for pattern in send-saturation; do
  for run in $(seq "${RUN_START:-1}" "$RUNS"); do
    if [[ "$LEVEL" == 0 ]]; then combinations=(raw-raw framework-raw framework-framework); else exit 2; fi
    for combination in "${combinations[@]}"; do
      export OUTPUT_DIR="$ROOT_OUTPUT/$combination"
      implementation=zlink-cpp
      endpoint=tcp://127.0.0.1:5294
      target_extra=(); source_extra=()
      target_binary=bench_cpp_framework_server
      if [[ "$combination" == framework-framework ]]; then
        implementation=zlink-framework-cpp
      elif [[ "$LEVEL" == 0 ]]; then
        source_extra=(--command-endpoint "$endpoint" --ladder-request-rid bench-server --ladder-command-rid bench-server)
        if [[ "$combination" == raw-raw ]]; then
          endpoint=tcp://127.0.0.1:5287
          target_binary=bench_cpp_zlink_server
          target_extra=(--command-endpoint tcp://127.0.0.1:5288)
          source_extra=(--command-endpoint tcp://127.0.0.1:5288)
        fi
      else
        source_extra=(--ladder-raw-mesh 1)
        if [[ "$combination" == raw-raw ]]; then
          target_extra=(--ladder-raw-mesh 1)
        fi
      fi
      cell_id="$implementation-$pattern-1024"
      cell_dir="$OUTPUT_DIR/run$run/$cell_id"
      test ! -e "$cell_dir/results.json" || { echo "refuse to overwrite raw: $cell_dir" >&2; exit 1; }
      mkdir -p "$cell_dir"
      [[ $(date -u +%s) -lt 1794564860 ]] || { echo "time cap: no new cross cell" >&2; exit 124; }
      printf '%s L%s %s run%s %s\n' "$(date --iso-8601=seconds)" "$LEVEL" "$pattern" "$run" "$combination"
      python3 "$JOB/tools/provenance.py" "$cell_dir"
      setsid taskset -c "$TARGET_CPUS" "$BUILD_DIR/$target_binary" --endpoint "$endpoint" \
        --stats-port 5295 "${target_extra[@]}" >"$cell_dir/target.log" 2>&1 &
      b_pid=$!
      wait_for_stats http://127.0.0.1:5295 0
      setsid taskset -c "$SOURCE_CPUS" "$BUILD_DIR/bench_cpp_client" \
        --implementation "$implementation" --pattern "$pattern" --payload-size 1024 \
        --endpoint "$endpoint" --trigger-port 5292 --stats-port 5293 --target-stats-port 5295 \
        --output-file "$cell_dir/results.json" --warmup-seconds "$WARMUP_SECONDS" --warmup-segments 10 \
        "${source_extra[@]}" >"$cell_dir/source.log" 2>&1 &
      a_pid=$!
      wait_for_stats http://127.0.0.1:5293 1
      bench_run_cell http://127.0.0.1:5292 http://127.0.0.1:5293 http://127.0.0.1:5295 \
        "$RUN_STAMP-run$run" "$cell_id" "$implementation" "$pattern" 1024 \
        "$((WARMUP_SECONDS*1000))" "$cell_dir/results.json" "$cell_dir/target-stats.json" http://127.0.0.1:5295
      curl -fsS http://127.0.0.1:5293/bench/stats >"$cell_dir/source-stats.json"
      record_cell_controls "$cell_dir/results.json" http://127.0.0.1:5293
      python3 "/root/jobs/fx/cx/v21/s033/l3868-l0e/tools/check-result.py" "$cell_dir/results.json"
      cleanup_cell
      wait_for_ports_free 5280 5299
    done
  done
done

