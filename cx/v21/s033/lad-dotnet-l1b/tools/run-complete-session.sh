#!/usr/bin/env bash
set -euo pipefail
if [[ "${L1B_LOCKED:-0}" != 1 ]]; then
  exec flock -x /root/jobs/fx/cx/measure.lock env L1B_LOCKED=1 "$0" "$@"
fi
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
TOOLS="$JOB/tools"
RUNNER="$ROOT/framework/bench/grpc/dotnet/run_local.sh"
SERVER_DIR="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0"
RAW_DIR="$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0"
CLIENT_DIR="$ROOT/framework/bench/grpc/dotnet/Client/bin/Release/net8.0"
L0_DIR="$JOB/raw/builds/l0-server-net8.0"
L1_DIR="$JOB/raw/builds/l1-server-net8.0"
mkdir -p "$JOB/raw/session" "$JOB/raw/lock-windows"

setgov(){ for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
record(){
  local path="$1"
  {
    date -u +%FT%TZ
    uptime
    free -b
    for cpu in $(seq 0 15); do
      printf 'cpu%s.governor=' "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
      printf 'cpu%s.scaling_cur_freq_khz=' "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"
    done
    ps -eo pid,psr,pcpu,stat,etime,comm,args --sort=-pcpu | sed -n "1,80p"
  } > "$path"
}
trap 'setgov powersave' EXIT
setgov performance
record "$JOB/raw/lock-windows/session-before.txt"

HEAD=$(git -C "$ROOT" rev-parse HEAD)
DIRTY=$(test -z "$(git -C "$ROOT" status --porcelain)" && echo no || echo yes)
BASE=$(git -C "$ROOT" merge-base HEAD origin/main)
BINDING_DLL="$RAW_DIR/WithGrpcBench.ZLinkRawServer.dll"
CLIENT_DLL="$CLIENT_DIR/WithGrpcBench.Client.dll"
SERVER_DLL="$SERVER_DIR/WithGrpcBench.ZLinkServer.dll"
FRAMEWORK_DLL="$SERVER_DIR/Zlink.Framework.dll"
test "$HEAD" = b6f58168a6e3d2499138ea36f9958d8a7fd7a3ef
test "$DIRTY" = no
for pair in \
  "$BINDING_DLL:$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs" \
  "$CLIENT_DLL:$ROOT/framework/bench/grpc/dotnet/Client/Program.cs" \
  "$SERVER_DLL:$ROOT/framework/bench/grpc/dotnet/ZLinkServer/Program.cs" \
  "$FRAMEWORK_DLL:$ROOT/framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs"; do
  dll=${pair%%:*}; src=${pair#*:}; test -f "$dll"; test -f "$src"; test "$dll" -nt "$src"
done

hashset(){
  local active="$1" target_hash framework_hash
  target_hash=$(sha256sum "$active" | cut -c1-64)
  framework_hash=$(sha256sum "$FRAMEWORK_DLL" | cut -c1-64)
  printf 'variant=%s compile=%s HEAD=%s dirty=%s merge-base=%s Client=%s:%s Binding=%s:%s target=%s:%s LadderServer=%s:%s Framework=%s:%s' \
    "$VARIANT" "$LEVEL" "$HEAD" "$DIRTY" "$BASE" "$CLIENT_DLL" "$(sha256sum "$CLIENT_DLL" | cut -c1-64)" \
    "$BINDING_DLL" "$(sha256sum "$BINDING_DLL" | cut -c1-64)" \
    "$active" "$target_hash" "$SERVER_DLL" "$(sha256sum "$SERVER_DLL" | cut -c1-64)" \
    "$FRAMEWORK_DLL" "$framework_hash"
}

activate(){
  case "$1" in
    binding) : ;;
    L0) cp -a "$L0_DIR/." "$SERVER_DIR/" ;;
    L1) cp -a "$L1_DIR/." "$SERVER_DIR/" ;;
    *) echo "unknown target $1" >&2; exit 2 ;;
  esac
}

run_one(){
  local variant="$1" run="$2" pattern="$3" size="$4" extra="$5"
  local active out cell dir_start logs stamp hashes target_impl raw_target
  activate "$variant"
  if [[ "$variant" == binding ]]; then
    active="$BINDING_DLL"; target_impl=zlink-dotnet; raw_target=""
  else
    active="$SERVER_DLL"; target_impl=zlink-framework-dotnet; raw_target=framework
  fi
  if [[ "$extra" == 0 ]]; then
    out="$JOB/raw/session/$variant/run-$run"
  else
    out="$JOB/raw/session/$variant/extra-$pattern-$size/run-$run"
  fi
  mkdir -p "$out/run1/zlink-dotnet-$pattern-$size"
  cell="$out/run1/zlink-dotnet-$pattern-$size"
  stamp=$(date -u +%FT%TZ)
  printf '%s\n' "$stamp" > "$cell/run-start.txt"
  if [[ "$pattern" == request-serial ]]; then
    ps -eo pid,psr,pcpu,stat,etime,comm,args --sort=-pcpu > "$JOB/raw/lock-windows/${variant}-${pattern}-${size}-run${run}-before.ps.txt"
  fi
  VARIANT="$variant"
  LEVEL="$variant"
  [[ "$variant" == binding ]] && LEVEL=raw
  hashes=$(hashset "$active")
  printf -- '- %s %s %s-%s run=%s start=%s %s\n' "$variant" "$HEAD" "$pattern" "$size" "$run" "$stamp" "$hashes" >> "$JOB/progress.md"
  printf -- '  parity: receive binding=ZLinkRawServer/Program.cs:64-76,125-137 vs L0=ZLinkServer/Program.cs:108-118 vs L1=ZLinkServer/Program.cs:159-172; reply binding=ZLinkRawServer/Program.cs:103-108 vs L0=ZLinkServer/Program.cs:116-123 vs L1=ZLinkServer/Program.cs:169-174. Each request: one blocking Recv (100ms timeout), parse once, same metrics lock (BenchServerMetrics.cs:50-70), new protobuf reply body encode + new header + 2-part submit; no request-body reuse/extra copy; no per-message atomic in binding/L0. Each send: one blocking Recv, parse once, same metrics lock, no reply/copy/atomic. L1 adds AcquireAsync state-lane turn + lease allocation before Recv and awaited ReleaseForHandlerStartAsync before handler (framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:208-223,520-534), one lease Interlocked.Exchange (framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:1210-1211), no message copy. Raw source-to-ladder envelope header is longer by the Framework marker; target route id is bench-server.\n' >> "$JOB/progress.md"
  export PATH="$TOOLS:$PATH" IMPLEMENTATIONS=zlink-dotnet PATTERNS="$pattern" PAYLOAD_SIZES="$size"
  export TARGET_IMPLEMENTATION="$target_impl" ZLINK_DOTNET_LADDER_CLIENT=raw
  if [[ -n "$raw_target" ]]; then export ZLINK_DOTNET_RAW_TARGET="$raw_target"; else unset ZLINK_DOTNET_RAW_TARGET || true; fi
  export SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 WARMUP_SECONDS=30 DURATION_SECONDS=5 RUNS=1
  export OUTPUT_DIR="$out" RUN_STAMP="4351-l1b-$variant-$pattern-$size-$run"
  logs="$JOB/raw/session/${variant}-${pattern}-${size}-run${run}-driver.log"
  "$RUNNER" > "$logs" 2>&1
  python3 - "$cell/results.json" "$cell/run-start.txt" "$variant" "$pattern" "$size" "$run" <<'PY'
import json, sys
p, startp, variant, pattern, size, run = sys.argv[1:]
d = json.load(open(p))['cells'][0]
valid = (d['errors'] == 0 and d['server_errors'] == 0 and
         d['submitted'] == d['completed'] == d['target_stats']['received'])
if not valid:
    raise SystemExit(f"broken cell: {p}")
print(f"- result {variant} {pattern}-{size} run={run} start={open(startp).read().strip()} rate={d['throughput_per_second']:.1f} errors=0 submitted=completed=received={d['completed']} raw={p}")
PY
  python3 - "$cell/results.json" "$variant" "$pattern" "$size" "$run" "$cell/results.json" <<'PY' >> "$JOB/progress.md"
import json, sys
p, variant, pattern, size, run, raw = sys.argv[1:]
d=json.load(open(p))['cells'][0]
print(f"- result {variant} {pattern}-{size} run={run} rate={d['throughput_per_second']:.1f} errors={d['errors']} submitted=completed=received={d['completed']} raw={raw}")
PY
  if [[ "$pattern" == request-serial ]]; then
    ps -eo pid,psr,pcpu,stat,etime,comm,args --sort=-pcpu > "$JOB/raw/lock-windows/${variant}-${pattern}-${size}-run${run}-after.ps.txt"
  fi
}

variants=(binding L0 L1)
patterns=(send-saturation request-serial request-backpressure)
sizes=(1024 4096)
for run in 1 2 3; do
  for pattern in "${patterns[@]}"; do
    for size in "${sizes[@]}"; do
      for variant in "${variants[@]}"; do
        run_one "$variant" "$run" "$pattern" "$size" 0
      done
    done
  done
done

# Always bring request-serial cells to five observations. Non-serial cells only get two extras if their G5 spread fails.
python3 - "$JOB/raw/session" > "$JOB/raw/session/extras.tsv" <<'PY'
import glob, json, statistics, sys
root=sys.argv[1]
variants=('binding','L0','L1')
cells=[]
for size in (1024,4096):
    for v in variants:
        cells.append((v,'request-serial',size,True))
for pattern in ('send-saturation','request-backpressure'):
    for size in (1024,4096):
        failed=False
        for v in variants:
            ps=[f'{root}/{v}/run-{i}/run1/zlink-dotnet-{pattern}-{size}/results.json' for i in (1,2,3)]
            rates=[json.load(open(p))['cells'][0]['throughput_per_second'] for p in ps]
            med=statistics.median(rates)
            failed |= max(abs(x-med) for x in rates)/med > .10
        if failed:
            for v in variants:
                cells.append((v,pattern,size,False))
for v,p,s,_ in cells:
    print(v,p,s)
PY
while read -r variant pattern size; do
  [[ -z "$variant" ]] && continue
  for run in 4 5; do run_one "$variant" "$run" "$pattern" "$size" 1; done
done < "$JOB/raw/session/extras.tsv"

record "$JOB/raw/lock-windows/session-after.txt"
