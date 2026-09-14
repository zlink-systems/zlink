#!/usr/bin/env bash
set -euo pipefail
diagnostic_output="$1"
diagnostic_driver="$2"
diagnostic_kind="${3:-managed}"
diagnostic_stages="${4:-envelope wire}"
[[ "$diagnostic_kind" == managed || "$diagnostic_kind" == native ]]
unset BENCH_DIAGNOSTIC_TARGET_DLL BENCH_DIAGNOSTIC_SOURCE_DLL BENCH_DIAGNOSTIC_STAGE
unset BENCH_DIAGNOSTIC_NO_DEADLINE BENCH_DIAGNOSTIC_MINIMAL_HEADER_READ
unset BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER
mkdir -p "$diagnostic_output"
for diagnostic_stage in $diagnostic_stages; do
  [[ "$diagnostic_stage" == core || "$diagnostic_stage" == core-direct || "$diagnostic_stage" == envelope || "$diagnostic_stage" == wire ]]
  diagnostic_cell="${diagnostic_output}/${diagnostic_stage}"
  diagnostic_env=(DOTNET_PerfMapEnabled=3)
  if [[ "$diagnostic_stage" != core ]]; then
    diagnostic_bench_stage="$diagnostic_stage"
    if [[ "$diagnostic_stage" == core-direct ]]; then diagnostic_bench_stage=core; fi
    diagnostic_env+=(BENCH_DIAGNOSTIC_STAGE="$diagnostic_bench_stage" BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER=1
      BENCH_DIAGNOSTIC_SOURCE_DLL="$diagnostic_driver" BENCH_DIAGNOSTIC_TARGET_DLL="$diagnostic_driver")
  fi
  env "${diagnostic_env[@]}" \
    bash framework/bench/grpc/dotnet/run_local.sh --skip-build --scenario request-backpressure \
      --implementation zlink-dotnet --duration-seconds 10 --warmup-seconds 3 --output "$diagnostic_cell" \
      >"${diagnostic_cell}.log" 2>&1 &
  diagnostic_launcher_pid=$!
  diagnostic_target_pid=""
  for diagnostic_probe in {1..200}; do
    if [[ "$diagnostic_stage" == core ]]; then
      diagnostic_target_pid=$(pgrep -f '(^|/| )WithGrpcBench.ZLinkRawServer(\.dll)? --endpoint tcp://127.0.0.1:5207' || true)
    else
      diagnostic_target_pid=$(pgrep -f "^dotnet $diagnostic_driver --role target " || true)
    fi
    if [[ "$diagnostic_target_pid" =~ ^[0-9]+$ ]] \
      && rg -q '"phase":"active"' "${diagnostic_cell}.log"; then break; fi
    sleep 0.1
  done
  [[ "$diagnostic_target_pid" =~ ^[0-9]+$ ]]
  rg -q '"phase":"active"' "${diagnostic_cell}.log"
  sudo -n /usr/lib/linux-tools-6.8.0-139/perf stat --per-thread -p "$diagnostic_target_pid" \
    -e task-clock,context-switches,cpu-migrations -x , -o /dev/stderr --timeout 9000 \
    2>"${diagnostic_cell}.perf.csv" &
  diagnostic_perf_pid=$!
  if [[ "$diagnostic_kind" == native ]]; then
    sudo -n /usr/lib/linux-tools-6.8.0-139/perf record -q -F 199 -p "$diagnostic_target_pid" \
      --call-graph dwarf,8192 -o - -- sleep 9 \
      >"${diagnostic_cell}.perf.data" 2>"${diagnostic_cell}.native.log"
  else
    /home/hep7/.dotnet/tools/dotnet-trace collect -p "$diagnostic_target_pid" \
      --profile dotnet-sampled-thread-time --format Speedscope --duration 00:00:09 \
      -o "${diagnostic_cell}.nettrace" >"${diagnostic_cell}.trace.log" 2>&1
  fi
  wait "$diagnostic_perf_pid"
  wait "$diagnostic_launcher_pid"
  if [[ "$diagnostic_kind" == native ]]; then
    cp "/tmp/perf-${diagnostic_target_pid}.map" "${diagnostic_cell}.map"
  fi
  jq -e 'all(.cells[]; .errors == 0 and .server_errors == 0 and .abandoned == 0
    and .drain_bound_hit == false and .target_stats.errors == 0
    and .submitted == .server_received_at_close and .completed == .submitted)' \
    "$diagnostic_cell/zlink-dotnet-request-backpressure-4096/results.json" >/dev/null
done
