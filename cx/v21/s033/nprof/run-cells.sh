#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/nprof
repo=/root/worktree/zlink-ratio040
run_one() {
  local mode=$1 impl=$2 pattern=$3 stamp=$4 out=$5
  local mem
  mem=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
  if ((mem < 12582912)); then echo "MemAvailable below 12 GiB: ${mem} KiB" >&2; return 3; fi
  local -a envs=("IMPLEMENTATIONS=${impl}" "PATTERNS=${pattern}" "PAYLOAD_SIZES=4096" "RUNS=1" "WARMUP_SECONDS=10" "DURATION_SECONDS=20" "SKIP_BUILD=1" "RUN_STAMP=${stamp}" "OUTPUT_DIR=${out}")
  if [[ $mode == profile ]]; then
    envs+=("CAPTURE_MODE=profile" "NODE_OPTIONS=--perf-basic-prof")
  elif [[ $mode == heap || $mode == cpu ]]; then
    envs+=("CAPTURE_MODE=${mode}" "NODE_OPTIONS=--inspect=127.0.0.1:0 --perf-basic-prof")
  elif [[ $mode == trace ]]; then
    envs+=("CAPTURE_MODE=trace")
  fi
  echo "BEGIN ${mode} ${impl} ${pattern} MemAvailable=${mem}KiB $(date -Is)"
  flock -o /root/jobs/fx/cx/measure.lock env "${envs[@]}" bash "${job}/run_local.sh" >"${out}.stdout" 2>"${out}.stderr"
  echo "END ${mode} ${impl} ${pattern} $(date -Is)"
}
case "${1:?mode baseline|profile}" in
  baseline|profile|heap|cpu|trace) mode=$1 ;;
  *) echo 'bad mode' >&2; exit 2 ;;
esac
shift
run_one "$mode" "$1" "$2" "$3" "$4"
