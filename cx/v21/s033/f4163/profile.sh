#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163
mkdir -p "$JOB/profiles"
for kind in C M; do
  if [[ "$kind" == C ]]; then src="$JOB/src"; else src="$JOB/src-mid"; fi
  bin="$src/bindings/c/build-release-1.23.0/perf/perf_router_router"
  prefix="$JOB/prefix-probe-$kind"
  for n in 1 2; do
    out="$JOB/profiles/$kind-$n"
    [[ ! -e "$out.data" ]]
    flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 bash -c '
      set -e
      python3 "$1/stage-preflight.py" "$1"
      export LD_LIBRARY_PATH="$2/lib"
      ldd "$3"
      exec env PERF_SINGLE_DURATION_SECONDS=5 PERF_PART_COUNT=2 PERF_CTX_AUTO_HWM_PROFILE=balanced PERF_SINGLE_SNDTIMEO_MS=200 PERF_SINGLE_RCVTIMEO_MS=200 PERF_PRINT_AUTO_HWM_DETAIL=0 perf stat -o "$4.stat" -e cycles,instructions,branches,branch-misses,cache-misses -- perf record -q -F 199 -e cycles:u --call-graph dwarf,8192 -o "$4.data" -- "$3" c inproc 1024
    ' _ "$JOB" "$prefix" "$bin" "$out" > "$out.log" 2>&1
    perf report -i "$out.data" --stdio --no-children --sort=pid,symbol --percent-limit=0.5 --no-call-graph -t ';' > "$out.report-fixed" 2> "$out.report-fixed-errors"
    tail -5 "$out.log"
  done
done
