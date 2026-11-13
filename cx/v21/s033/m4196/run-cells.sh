#!/usr/bin/env bash
set -euo pipefail
BASE=/root/worktree/zlink-v21-s033/m4196-base
PR=/root/worktree/d4196-pr
JOB=/root/jobs/fx/cx/v21/s033/m4196
OUTROOT="${OUTROOT:-$JOB/raw/final}"
source "$BASE/framework/bench/grpc/runner_common.sh"
DRAIN_BOUND_MS=30000
WINDOW=100
SEND_CONCURRENCY=8
TIMEOUT_SECONDS=300
LATENCY_SAMPLE_LIMIT=200000
cleanup() {
  set +e
  for pid in "${source_pid:-}" "${target_pid:-}"; do
    [[ -n "$pid" ]] || continue
    kill -TERM -- "-$pid" >/dev/null 2>&1 || kill -TERM "$pid" >/dev/null 2>&1 || true
  done
  sleep .3
  for pid in "${source_pid:-}" "${target_pid:-}"; do
    [[ -n "$pid" ]] || continue
    wait "$pid" >/dev/null 2>&1 || true
  done
  source_pid= target_pid=
}
trap cleanup EXIT
snapshot() {
  local role=$1 pid=$2 path=$3
  python3 - "$role" "$pid" "$path" <<'PY'
import json,pathlib,sys,time
role,pid,path=sys.argv[1:]; root=pathlib.Path(f'/proc/{pid}/task'); threads={}
for t in root.iterdir():
    try:
        status={x.split(':',1)[0]:x.split(':',1)[1].strip() for x in (t/'status').read_text().splitlines() if ':' in x}
        threads[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'voluntary':int(status['voluntary_ctxt_switches']),'involuntary':int(status['nonvoluntary_ctxt_switches'])}
    except (FileNotFoundError,ProcessLookupError,KeyError): pass
obj={'role':role,'pid':int(pid),'monotonic':time.monotonic(),'threads':threads}
pathlib.Path(path).write_text(json.dumps(obj,indent=2)+'\n')
PY
}
cell() {
  local variant=$1 root=$2 round=$3 impl=$4 pattern=$5 payload=$6
  local port trigger source_url endpoint cmd_endpoint target_url target_project cell_dir run_id
  case "$impl" in
    zlink-dotnet) port=5205; trigger=http://127.0.0.1:5205; source_url=http://127.0.0.1:5206; endpoint=tcp://127.0.0.1:5207; cmd_endpoint=tcp://127.0.0.1:5208; target_url=http://127.0.0.1:5209; target_project="$root/framework/bench/grpc/dotnet/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj" ;;
    zlink-framework-dotnet) port=5212; trigger=http://127.0.0.1:5212; source_url=http://127.0.0.1:5213; endpoint=tcp://127.0.0.1:5214; cmd_endpoint=; target_url=http://127.0.0.1:5215; target_project="$root/framework/bench/grpc/dotnet/ZLinkServer/WithGrpcBench.ZLinkServer.csproj" ;;
  esac
  cell_dir="$OUTROOT/round${round}/${variant}/${impl}-${pattern}-${payload}"
  mkdir -p "$cell_dir"
  run_id="m4196-${round}-${variant}"
  if [[ "$impl" == zlink-dotnet ]]; then
    setsid taskset -c 8-15 dotnet "$root/framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll" --endpoint "$endpoint" --command-endpoint "$cmd_endpoint" --metrics-url "$target_url" >"$cell_dir/target.log" 2>&1 &
  else
    setsid taskset -c 8-15 dotnet "$root/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll" --endpoint "$endpoint" --metrics-url "$target_url" >"$cell_dir/target.log" 2>&1 &
  fi
  target_pid=$!
  wait_for_stats "$target_url" 0
  taskset -pc "$target_pid" >"$cell_dir/target-affinity.txt"
  rg -q "affinity list: 8-15$" "$cell_dir/target-affinity.txt" || { echo "target affinity mismatch: $cell_dir/target-affinity.txt" >&2; return 1; }
  tr "\0" " " <"/proc/$target_pid/cmdline" >"$cell_dir/target-cmdline.txt"
  local source_project="$root/framework/bench/grpc/dotnet/Client/WithGrpcBench.Client.csproj"
  local source_args=(--implementation "$impl" --scenario "$pattern" --payload-size "$payload" --request-window 100 --send-concurrency 8 --latency-sample-limit 200000 --warmup-seconds 30 --drain-bound-ms 30000 --trigger-url "$trigger" --stats-url "$source_url" --target-endpoint "$endpoint" --target-stats-url "$target_url" --raw-socket router --output "$cell_dir" --report-file "$cell_dir/report.txt" --configuration Release --timeout-seconds 300)
  [[ -z "$cmd_endpoint" ]] || source_args+=(--target-command-endpoint "$cmd_endpoint")
  setsid taskset -c 0-7 dotnet "$root/framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll" "${source_args[@]}" >"$cell_dir/source.log" 2>&1 &
  source_pid=$!
  wait_for_stats "$source_url" 1
  taskset -pc "$source_pid" >"$cell_dir/source-affinity.txt"
  rg -q "affinity list: 0-7$" "$cell_dir/source-affinity.txt" || { echo "source affinity mismatch: $cell_dir/source-affinity.txt" >&2; return 1; }
  tr "\0" " " <"/proc/$source_pid/cmdline" >"$cell_dir/source-cmdline.txt"
  trigger_phase "$trigger" "$run_id" "${impl}-${pattern}-${payload}" "$pattern" "$payload" warmup 30000 >/dev/null
  wait_for_idle "$source_url"
  settle_and_capture "$source_url" "$target_url" /dev/null allPhaseMessages 30000
  [[ "$SETTLE_BOUND_HIT" == false ]] || { echo "warmup settle bound hit $cell_dir" >&2; return 1; }
  trigger_phase "$trigger" "$run_id" "${impl}-${pattern}-${payload}" "$pattern" "$payload" active 5000 >/dev/null
  local deadline=$((SECONDS+10)) phase
  while ((SECONDS<deadline)); do
    phase=$(curl --silent --show-error --fail "$source_url/bench/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("phase",""))')
    [[ "$phase" == active ]] && break
    sleep .02
  done
  [[ "$phase" == active ]] || { echo "active phase not observed $cell_dir phase=$phase" >&2; return 1; }
  snapshot source "$source_pid" "$cell_dir/source-before.json"
  snapshot target "$target_pid" "$cell_dir/target-before.json"
  curl --silent --show-error --fail "$source_url/bench/stats" > "$cell_dir/source-stats-before.json"
  local active_deadline=$((SECONDS+8))
  while ((SECONDS<active_deadline)); do
    phase=$(curl --silent --show-error --fail "$source_url/bench/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("phase",""))')
    [[ "$phase" == active ]] || break
    sleep .05
  done
  snapshot source "$source_pid" "$cell_dir/source-after.json"
  snapshot target "$target_pid" "$cell_dir/target-after.json"
  curl --silent --show-error --fail "$source_url/bench/stats" > "$cell_dir/source-stats-after.json"
  wait_for_idle "$source_url"
  [[ -s "$cell_dir/results.json" ]] || { echo "missing result $cell_dir" >&2; return 1; }
  settle_and_capture "$source_url" "$target_url" "$cell_dir/target-stats.json" received 30000
  [[ "$SETTLE_BOUND_HIT" == false ]] || { echo "active settle bound hit: $cell_dir" >&2; return 1; }
  merge_target_stats "$cell_dir/results.json" "$cell_dir/target-stats.json" "$SETTLE_MS" false
  verify_request_counts "$cell_dir/results.json"
  python3 - "$cell_dir" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]); result=json.loads((p/'results.json').read_text())
cell=result['cells'][0]
(p/'result-summary.json').write_text(json.dumps({'throughput_per_second':cell['throughput_per_second'],'completed':cell.get('completed_messages',cell.get('completed')),'result':cell},indent=2)+'\n')
PY
  cleanup
  wait_for_ports_free "$port" "$((port+4))"
  echo "finished $cell_dir"
}
for round in 1 2 3; do
  if ((round%2==1)); then order=(base pr); else order=(pr base); fi
  for variant in "${order[@]}"; do
    root=$BASE; [[ "$variant" != pr ]] || root=$PR
    cell "$variant" "$root" "$round" zlink-dotnet send-saturation 1024
    cell "$variant" "$root" "$round" zlink-dotnet send-saturation 4096
    cell "$variant" "$root" "$round" zlink-dotnet request-backpressure 1024
    cell "$variant" "$root" "$round" zlink-framework-dotnet send-saturation 1024
  done
done
