#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124
RESULTS="$JOB/results"
COMPARATOR="$JOB/src-1.24/bindings/c/perf/perf_regression_gate.py"
SINGLE_TRANSPORTS=tcp,tls,ws,wss,inproc,ipc
MULTI_TRANSPORTS=tcp,tls,ws,wss
STAGES=(B1-single B1-multi C-single C-multi B2-single B2-multi)
source_root() { [[ $1 == B1 || $1 == B2 ]] && printf '%s' "$JOB/src-1.23" || printf '%s' "$JOB/src-1.24"; }
prefix_for() { [[ $1 == C* ]] && printf '%s/prefix-1.24' "$JOB" || printf '%s/prefix-1.23' "$JOB"; }
runner_for() { [[ $1 == *-multi ]] && printf run_benchmarks_multi.sh || printf run_benchmarks.sh; }
expected_head() { [[ $1 == C* ]] && printf 0005a4d6c07160bbf661a6f7aea3d8093880514e || printf 79567d379162e67476ba13e18cfe0f69925e63c9; }
expected_core_sha() { [[ $1 == C* ]] && printf 2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64 || printf c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a; }
core_runtime() { find "$(prefix_for "$1")/lib" -maxdepth 1 -name 'libzlink.so.*' -type f | sort | head -1; }
sha256() { sha256sum "$1" | cut -d' ' -f1; }
suite_for() { [[ $1 == *-multi ]] && printf multi || printf single; }
kind_for() { [[ $1 == C* ]] && printf candidate || printf baseline; }
print_stage() {
  local stage=$1 root prefix runner suite transports out
  root=$(source_root "${stage%%-*}"); prefix=$(prefix_for "$stage"); runner=$(runner_for "$stage"); suite=$(suite_for "$stage")
  transports=$SINGLE_TRANSPORTS; [[ $suite == multi ]] && transports=$MULTI_TRANSPORTS
  out="$RESULTS/$stage.txt"
  printf 'STAGE %s\n' "$stage"
  printf '  taskset -c 8-15 env ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=%q PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 bash -c %q _ %q %q %q %q\n' \
    "$prefix" 'cd "$1"; exec bash "$2" --reuse-build --pattern ALL --transports "$3" --msg-sizes 64,256,1024,65536 --runs 3 --output "$4"' "$root" "$root/bindings/c/perf/$runner" "$transports" "$out"
}
if [[ ${1:-} == --dry-run ]]; then
  echo 'Dry run only; no preflight or benchmark is executed.'
  for s in "${STAGES[@]}"; do print_stage "$s"; done
  for pair in 'C-vs-B1 B1 C' 'C-vs-B2 B2 C' 'B2-vs-B1 B1 B2'; do
    read -r name base cand <<<"$pair"
    printf 'COMPARISON %s: python3 %q --baseline-single %q --candidate-single %q --baseline-multi %q --candidate-multi %q\n' \
      "$name" "$COMPARATOR" "$RESULTS/$base-single.txt" "$RESULTS/$cand-single.txt" "$RESULTS/$base-multi.txt" "$RESULTS/$cand-multi.txt"
  done
  exit 0
fi
[[ $# -eq 0 ]] || { echo "Usage: $0 [--dry-run]" >&2; exit 2; }
for stage in "${STAGES[@]}"; do
  for suffix in txt log meta.json; do [[ ! -e "$RESULTS/$stage.$suffix" ]] || { echo "Refusing to overwrite $RESULTS/$stage.$suffix" >&2; exit 2; }; done
done
for f in gate-C-vs-B1.txt gate-C-vs-B2.txt gate-B2-vs-B1.txt; do [[ ! -e "$JOB/$f" ]] || { echo "Refusing to overwrite $JOB/$f" >&2; exit 2; }; done
mkdir -p "$RESULTS"
preflight() {
  local stage="$1" deadline=$((SECONDS + 1800))
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
  s=pathlib.Path(f'/proc/{pid}/stat').read_text();pid=int(s[s.rfind(')')+2:].split()[1])
 except (FileNotFoundError,ValueError,IndexError):break
ps=subprocess.run(['ps','-eo','pid=,ppid=,stat=,pcpu=,comm=,args='],text=True,capture_output=True,check=True); heavy=[]
for line in ps.stdout.splitlines():
 z=line.split(None,5)
 if len(z)<6:continue
 p=int(z[0]); st=z[2]; cpu=float(z[3]); cmd=z[4]+' '+z[5]
 if p in anc or 'Z' in st or 'codex' in cmd.lower():continue
 if rx.search(cmd) or cpu>=10:heavy.append({'pid':p,'cpu_percent':cpu,'cmdline':cmd[:300]})
r={'stage':sys.argv[2],'captured_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'MemAvailable_bytes':mem,'load_1m':load,'heavy_processes':heavy,'allowed':mem>=12*1024**3 and load<=1.0 and not heavy}
with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r))
PY
)"
    echo "$status"
    if python3 -c 'import json,sys;raise SystemExit(0 if json.loads(sys.argv[1])["allowed"] else 1)' "$status"; then return 0; fi
    if (( SECONDS >= deadline )); then echo "Preflight did not pass within 30 minutes: $stage" >&2; return 3; fi
    sleep 30
  done
}
for stage in "${STAGES[@]}"; do
  preflight "$stage"
  kind=$(kind_for "$stage"); root=$(source_root "${stage%%-*}"); prefix=$(prefix_for "$stage"); runner=$(runner_for "$stage"); suite=$(suite_for "$stage")
  transports=$SINGLE_TRANSPORTS; [[ $suite == multi ]] && transports=$MULTI_TRANSPORTS
  version=1.23.0
  build="$root/bindings/c/build-release-$version"
  [[ "$(git -C "$root" rev-parse HEAD)" == "$(expected_head "$stage")" ]] || { echo "Unexpected source revision: $root" >&2; exit 4; }
  [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || { echo "Prepared source tree is modified: $root" >&2; exit 4; }
  [[ -d "$build" ]] || { echo "Missing harness build directory: $build" >&2; exit 4; }
  core="$(core_runtime "$stage")"
  [[ -f "$core" ]] || { echo "Missing Core runtime for $stage" >&2; exit 4; }
  [[ "$(sha256 "$core")" == "$(expected_core_sha "$stage")" ]] || { echo "Core SHA mismatch: $core" >&2; exit 4; }
  for exe in "$build"/perf/*; do
    [[ -f "$exe" && -x "$exe" ]] || continue
    if [[ $(basename "$exe") == perf_stream_client ]]; then continue; fi
    ldd "$exe" | grep -F "=> $prefix/lib/" >/dev/null || { echo "Wrong Core linkage: $exe" >&2; exit 4; }
  done
  report="$RESULTS/$stage.txt"; log="$RESULTS/$stage.log"
  started=$(date -u +%FT%T.%NZ); load_before=$(cat /proc/loadavg); core_before=$(sha256 "$core")
  echo "START $stage $started"
  set +e
  taskset -c 8-15 env ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$prefix" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
    bash -c 'cd "$1"; exec bash "$2" --reuse-build --pattern ALL --transports "$3" --msg-sizes 64,256,1024,65536 --runs 3 --output "$4"' \
    _ "$root" "$root/bindings/c/perf/$runner" "$transports" "$report" > "$log" 2>&1
  rc=$?
  set -e
  ended=$(date -u +%FT%T.%NZ); load_after=$(cat /proc/loadavg); core_after=$(sha256 "$core")
  python3 - "$RESULTS/$stage.meta.json" "$stage" "$started" "$ended" "$load_before" "$load_after" "$core_before" "$core_after" "$rc" <<'PY'
import json,pathlib,sys
pathlib.Path(sys.argv[1]).write_text(json.dumps(dict(stage=sys.argv[2],start_utc=sys.argv[3],end_utc=sys.argv[4],loadavg_before=sys.argv[5].split(),loadavg_after=sys.argv[6].split(),core_sha256_before=sys.argv[7],core_sha256_after=sys.argv[8],exit_code=int(sys.argv[9])),indent=2)+'\n')
PY
  echo "END $stage $ended rc=$rc"
  (( rc == 0 )) || { tail -40 "$log" >&2; exit "$rc"; }
  [[ $core_before == "$core_after" ]] || { echo "Core package changed during $stage" >&2; exit 4; }
done
for pair in 'C-vs-B1 B1 C' 'C-vs-B2 B2 C' 'B2-vs-B1 B1 B2'; do
  read -r name base cand <<<"$pair"
  set +e
  python3 "$COMPARATOR" --baseline-single "$RESULTS/$base-single.txt" --candidate-single "$RESULTS/$cand-single.txt" \
    --baseline-multi "$RESULTS/$base-multi.txt" --candidate-multi "$RESULTS/$cand-multi.txt" > "$JOB/gate-$name.txt" 2>&1
  rc=$?
  set -e
  cat "$JOB/gate-$name.txt"
  (( rc <= 1 )) || exit "$rc"
done
