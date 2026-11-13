#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124b
RESULTS="$JOB/results"
ROOT="$JOB/src-1.23"
PREFIX=/root/jobs/fx/cx/v21/s033/core124/prefix-1.23
RUNTIME="$PREFIX/lib/libzlink.so.1.23.0"
EXPECTED_CORE_SHA=c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a
EXPECTED_HEAD=79567d379162e67476ba13e18cfe0f69925e63c9
SINGLE_TRANSPORTS=tcp,tls,ws,wss,inproc,ipc
MULTI_TRANSPORTS=tcp,tls,ws,wss

preflight() {
  local stage="$1" wait_started=$SECONDS blocked_logged=0
  while :; do
    local status
    status="$(python3 - "$JOB/preflight.jsonl" "$stage" <<'PY'
import json,os,pathlib,re,subprocess,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
load=float(pathlib.Path('/proc/loadavg').read_text().split()[0])
rx=re.compile(r'(^|[ /])(cmake|ninja|gmake|make|gcc(?:-\d+)?|g\+\+(?:-\d+)?|cc1plus?|ctest|pytest|perf_[\w]+|comp_src_[\w]+|run_benchmarks(?:_multi)?\.sh|run_comparison\.py)(?:$|\s)',re.I)
anc=set(); pid=os.getpid()
while pid>1:
 anc.add(pid)
 try:
  s=pathlib.Path(f'/proc/{pid}/stat').read_text(); pid=int(s[s.rfind(')')+2:].split()[1])
 except (FileNotFoundError,ValueError,IndexError): break
ps=subprocess.run(['ps','-eo','pid=,ppid=,stat=,pcpu=,comm=,args='],text=True,capture_output=True,check=True); heavy=[]
for line in ps.stdout.splitlines():
 z=line.split(None,5)
 if len(z)<6: continue
 p=int(z[0]); st=z[2]; cpu=float(z[3]); cmd=z[4]+' '+z[5]
 if p in anc or 'Z' in st or 'codex' in cmd.lower(): continue
 if rx.search(cmd) or cpu>=10: heavy.append({'pid':p,'cpu_percent':cpu,'cmdline':cmd[:300]})
r={'stage':sys.argv[2],'captured_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'MemAvailable_bytes':mem,'load_1m':load,'heavy_processes':heavy,'allowed':mem>=12*1024**3 and load<=1.0 and not heavy}
with open(sys.argv[1],'a') as f: f.write(json.dumps(r)+'\n')
print(json.dumps(r))
PY
    )"
    echo "$status"
    if python3 -c 'import json,sys; raise SystemExit(0 if json.loads(sys.argv[1])["allowed"] else 1)' "$status"; then return 0; fi
    if (( blocked_logged == 0 && SECONDS - wait_started >= 600 )); then
      { printf 'Blocked preflight after 10 minutes: %s at %s\n' "$stage" "$(date -u +%FT%T.%NZ)"; printf '%s\n' "$status"; ps -eo pid=,ppid=,stat=,pcpu=,comm=,args=; } >> "$JOB/blocked.txt"
      blocked_logged=1
    fi
    sleep 30
  done
}

for stage in B2-single B2-multi; do
  for suffix in txt log meta.json; do [[ ! -e "$RESULTS/$stage.$suffix" ]] || { echo "Refusing to overwrite $RESULTS/$stage.$suffix" >&2; exit 2; }; done
  preflight "$stage"
  [[ "$(git -C "$ROOT" rev-parse HEAD)" == "$EXPECTED_HEAD" ]]
  [[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=all)" ]]
  [[ -f "$RUNTIME" && "$(sha256sum "$RUNTIME" | cut -d' ' -f1)" == "$EXPECTED_CORE_SHA" ]]
  version=1.23.0
  build="$ROOT/bindings/c/build-release-$version"
  [[ -d "$build" ]]
  suite=single; runner=run_benchmarks.sh; transports=$SINGLE_TRANSPORTS
  if [[ $stage == *-multi ]]; then suite=multi; runner=run_benchmarks_multi.sh; transports=$MULTI_TRANSPORTS; fi
  for exe in "$build"/perf/*; do
    [[ -f "$exe" && -x "$exe" ]] || continue
    [[ $(basename "$exe") == perf_stream_client ]] && continue
    ldd "$exe" | grep -F "=> $PREFIX/lib/" >/dev/null || { echo "Wrong Core linkage: $exe" >&2; exit 4; }
  done
  report="$RESULTS/$stage.txt"; log="$RESULTS/$stage.log"
  started=$(date -u +%FT%T.%NZ); load_before=$(cat /proc/loadavg); core_before=$(sha256sum "$RUNTIME" | cut -d' ' -f1)
  echo "START $stage $started"
  set +e
  taskset -c 8-15 env ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$PREFIX" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
    bash -c 'cd "$1"; exec bash "$2" --reuse-build --pattern ALL --transports "$3" --msg-sizes 64,256,1024,65536 --runs 3 --output "$4"' \
    _ "$ROOT" "$ROOT/bindings/c/perf/$runner" "$transports" "$report" > "$log" 2>&1
  rc=$?
  set -e
  ended=$(date -u +%FT%T.%NZ); load_after=$(cat /proc/loadavg); core_after=$(sha256sum "$RUNTIME" | cut -d' ' -f1)
  python3 - "$RESULTS/$stage.meta.json" "$stage" "$started" "$ended" "$load_before" "$load_after" "$core_before" "$core_after" "$rc" <<'PY'
import json,pathlib,sys
pathlib.Path(sys.argv[1]).write_text(json.dumps(dict(stage=sys.argv[2],start_utc=sys.argv[3],end_utc=sys.argv[4],loadavg_before=sys.argv[5].split(),loadavg_after=sys.argv[6].split(),core_sha256_before=sys.argv[7],core_sha256_after=sys.argv[8],exit_code=int(sys.argv[9])),indent=2)+'\n')
PY
  echo "END $stage $ended rc=$rc"
  (( rc == 0 )) || { tail -40 "$log" >&2; exit "$rc"; }
  [[ $core_before == "$core_after" ]]
done
