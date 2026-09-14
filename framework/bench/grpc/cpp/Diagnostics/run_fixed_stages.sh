#!/usr/bin/env bash
# Test-only diagnostics. Prepare and selftest are separate from measurements.
set -euo pipefail
diagnostic_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cpp_dir="$(cd "${diagnostic_dir}/.." && pwd)"
source "${cpp_dir}/common/runner_lifecycle.sh"
DRAIN_BOUND_MS=30000
COMMAND_SETTLE_MS=200
build_dir="${BUILD_DIR:-${cpp_dir}/build}"
stages=(core codec)
mode=prepare
output="${cpp_dir}/../log/cpp/diagnostic-$(date +%Y%m%d_%H%M%S)"
duration=5
warmup=2
runs=1
while (($#)); do
  case "$1" in
    --stages) read -r -a stages <<<"$2"; shift 2 ;;
    --prepare) mode=prepare; shift ;;
    --selftest) mode=selftest; shift ;;
    --measure) mode=measure; shift ;;
    --output) output="$2"; shift 2 ;;
    --duration) duration="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --runs) runs="$2"; shift 2 ;;
    *) echo "Unsupported diagnostic argument: $1" >&2; exit 2 ;;
  esac
done
[[ "$runs" == 1 ]] || { echo 'This diagnostic uses one run per cell.' >&2; exit 2; }
[[ "$duration" =~ ^[1-9][0-9]*$ && "$warmup" =~ ^[1-9][0-9]*$ ]] || exit 2
targets=()
for stage in "${stages[@]}"; do
  case "$stage" in
    core|codec) ;;
    envelope|wire|mailbox)
      echo "Diagnostic stage '$stage' is inactive; only core and codec are approved." >&2
      exit 2 ;;
    *) echo "Unknown diagnostic stage: $stage" >&2; exit 2 ;;
  esac
  targets+=("bench_cpp_diag_${stage}")
done
if [[ "$mode" == prepare ]]; then
  cmake -S "$cpp_dir" -B "$build_dir"
  cmake --build "$build_dir" --target "${targets[@]}" -j2
  for stage in "${stages[@]}"; do
    "$build_dir/bench_cpp_diag_${stage}" --role info
    "$build_dir/bench_cpp_diag_${stage}" --role fidelity
  done
  exit
fi
mkdir -p "$output"
target_pid=""
source_pid=""
target_url=""
source_url=""
cleanup() {
  for role in source target; do
    local pid_var="${role}_pid" url_var="${role}_url"
    if [[ -n "${!pid_var}" ]]; then
      curl --silent --show-error --fail --max-time 5 -H 'content-type: application/json' \
        --data '{"phase":"close"}' "${!url_var}/bench/start" >/dev/null 2>&1 \
        || kill -KILL "${!pid_var}" 2>/dev/null || true
      wait "${!pid_var}" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT INT TERM
for stage in "${stages[@]}"; do
  driver="$build_dir/bench_cpp_diag_${stage}"
  "$driver" --role info >"$output/${stage}.info.json"
  "$driver" --role fidelity >"$output/${stage}.fidelity.log"
  for pattern in request-serial request-backpressure send-saturation; do
    cell="$output/$stage/$pattern"
    mkdir -p "$cell"
    # An ephemeral loopback endpoint avoids the production runner port range.
    read -r port target_stats_port source_stats_port < <(python3 -c 'import socket; ss=[socket.socket() for _ in range(3)]; [s.bind(("127.0.0.1",0)) for s in ss]; print(*(s.getsockname()[1] for s in ss)); [s.close() for s in ss]')
    endpoint="tcp://127.0.0.1:$port"
    target_url="http://127.0.0.1:$target_stats_port"
    source_url="http://127.0.0.1:$source_stats_port"
    target_args=(--role target --pattern "$pattern" --endpoint "$endpoint" --stats-port "$target_stats_port" --output "$cell/target.json")
    expected_count=1
    case "$pattern" in request-backpressure) expected_count=100 ;; send-saturation) expected_count=8 ;; esac
    "$driver" "${target_args[@]}" >"$cell/target.log" 2>&1 &
    target_pid=$!
    ready_deadline=$((SECONDS + 10))
    until rg -q '^READY$' "$cell/target.log"; do
      kill -0 "$target_pid" 2>/dev/null || { cat "$cell/target.log" >&2; exit 1; }
      ((SECONDS < ready_deadline)) || { echo 'Target readiness timeout' >&2; exit 1; }
      sleep 0.05
    done
    source_role=source
    [[ "$mode" != selftest ]] || source_role=selftest
    "$driver" --role "$source_role" --pattern "$pattern" --endpoint "$endpoint" \
      --warmup "$warmup" --duration "$duration" --target-pid "$target_pid" \
      --target-stats-port "$target_stats_port" --stats-port "$source_stats_port" \
      --output "$cell/results.json" >"$cell/source.log" 2>&1 &
    source_pid=$!
    ready_deadline=$((SECONDS + warmup + duration + 30))
    until rg -q '^IDLE$' "$cell/source.log"; do
      kill -0 "$source_pid" 2>/dev/null || { cat "$cell/source.log" >&2; exit 1; }
      ((SECONDS < ready_deadline)) || { echo 'Source phase did not complete' >&2; exit 1; }
      sleep 0.05
    done
    # One shared settle owner. The source still owns its native socket here.
    settle_and_capture "$source_url" "$target_url" "$cell/target-stats.json" any
    python3 - "$cell/results.json" "$cell/target-stats.json" "$mode" "$expected_count" "$SETTLE_MS" "$SETTLE_BOUND_HIT" <<'PY'
import json,sys
source=json.load(open(sys.argv[1])); target=json.load(open(sys.argv[2]))['snapshot']
valid=(source['errors']==target['errors']==0 and not source['drainBoundHit'] and sys.argv[6]=='false'
       and source['submitted']==source['completed']==target['activeMessages']
       and source['warmupSubmitted']==source['warmupCompleted']
       and target['anyPhaseMessages']==source['warmupSubmitted']+source['submitted'])
if sys.argv[3]=='selftest':
    assert valid and source['submitted']==source['warmupSubmitted']==int(sys.argv[4])
else:
    summary={'stage':source['stage'],'pattern':source['pattern'],'valid':valid,
             'serverReceivedAtClose':source['serverReceivedAtClose'],
             'serverReceivedPostDrain':target['activeMessages'],'drainMs':int(sys.argv[5]),
             'serverReceivedPerSecond':source['serverReceivedAtClose']/source['durationSeconds'],
             'bandwidthMbPerSecond':source['serverReceivedAtClose']/source['durationSeconds']*4096/1e6,
             'sourceCpuPercent':source['sourceCpuPercent'],'sourceMemoryMb':source['sourceMemoryMb'],
             'targetCpuPercent':source['targetCpuPercent'],'targetMemoryMb':source['targetMemoryMb'],
             'durationSeconds':source['durationSeconds'],'warmupSeconds':source['warmupSeconds']}
    print(json.dumps(summary))
    with open(sys.argv[1].removesuffix('results.json')+'summary.json','w') as output:
        json.dump(summary,output,indent=2)
if not valid: raise SystemExit('Diagnostic cell invalid; throughput comparison rejected')
PY
    # Close the receiver gracefully, then release the retained source context.
    curl --silent --show-error --fail --max-time 5 -H 'content-type: application/json' \
      --data '{"phase":"close"}' "$target_url/bench/start" >/dev/null
    wait "$target_pid"
    target_pid=""
    curl --silent --show-error --fail --max-time 5 -H 'content-type: application/json' \
      --data '{"phase":"close"}' "$source_url/bench/start" >/dev/null
    wait "$source_pid"
    source_pid=""
    [[ "$mode" != selftest ]] || echo "Selftest passed: $stage/$pattern"
  done
done
