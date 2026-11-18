#!/usr/bin/env bash
set -uo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <initial-delay-seconds> <interval-seconds> <output-directory>" >&2
  exit 2
fi

initial_delay=$1
interval=$2
output_dir=$3
if ! [[ "$initial_delay" =~ ^[0-9]+$ && "$interval" =~ ^[0-9]+$ ]] || (( interval < 1 )); then
  echo "Delay and interval must be non-negative integers; interval must be positive." >&2
  exit 2
fi

mkdir -p "$output_dir"
baseline_pids=" $(jps -lv 2>/dev/null | awk '$1 ~ /^[0-9]+$/ { printf "%s ", $1 }')"
sleep "$initial_delay"

while :; do
  timestamp=$(date -u +%Y%m%dT%H%M%SZ)
  process_list=$(jps -lv 2>&1 || true)
  while IFS= read -r process_line; do
    [[ "$process_line" =~ ^([0-9]+)[[:space:]]+([^[:space:]]+) ]] || continue
    pid=${BASH_REMATCH[1]}
    main_class=${BASH_REMATCH[2]}
    [[ "$baseline_pids" == *" ${pid} "* ]] && continue
    case "$main_class" in
      org.gradle.launcher.daemon.bootstrap.GradleDaemon|worker.org.gradle.process.internal.worker.GradleWorkerMain|org.gradle.process.internal.worker.GradleWorkerMain)
        safe_class=${main_class//./_}
        dump_file="${output_dir}/${timestamp}-pid-${pid}-${safe_class}.txt"
        {
          echo "UTC timestamp: ${timestamp}"
          echo "PID: ${pid}"
          echo "Process: ${process_line}"
          echo
          jstack -l "$pid"
        } >"$dump_file" 2>&1 || echo "jstack exited with status $?" >>"$dump_file"
        echo "Wrote JVM thread dump: ${dump_file}"
        ;;
    esac
  done <<<"$process_list"
  sleep "$interval"
done
