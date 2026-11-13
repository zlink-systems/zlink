#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/jprof
REPO=/root/worktree/zlink-ratio040
IMPL=${1:?implementation}
PATTERN=${2:?pattern}
MODE=${3:?profile|plain}
STAMP=jprof-${IMPL}-${PATTERN}-${MODE}
OUT=$JOB/out/$STAMP
CELL=$OUT/run1/${IMPL}-${PATTERN}-1024
mkdir -p "$OUT" "$CELL" "$JOB/profiles"
export RUN_STAMP=$STAMP OUTPUT_DIR=$OUT RUNS=1 WARMUP_SECONDS=2 DURATION_SECONDS=20 SKIP_BUILD=1
export IMPLEMENTATIONS=$IMPL PATTERNS=$PATTERN PAYLOAD_SIZES=1024
RUNNER=$REPO/framework/bench/grpc/java/run_local.sh
if [[ "$IMPL" == zlink-framework-java ]]; then SRC_URL=http://127.0.0.1:5253; TGT_URL=http://127.0.0.1:5255; SRC_MATCH='systems.zlink.bench.withgrpc.client.BenchClient'; TGT_MATCH='ZLinkFrameworkBenchServer'; else SRC_URL=http://127.0.0.1:5246; TGT_URL=http://127.0.0.1:5249; SRC_MATCH='systems.zlink.bench.withgrpc.client.BenchClient'; TGT_MATCH='ZLinkRawBenchServer'; fi
runner_pid=
source_pid=
target_pid=
recording=0
strace_source_pid=
strace_target_pid=
sampler_source_pid=
sampler_target_pid=
perf_source_pid=
perf_target_pid=
phase() { curl -sS --max-time 1 "$SRC_URL/bench/stats" 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin).get("phase", ""))' 2>/dev/null || true; }
findpid() { ps -C java -o pid=,args= 2>/dev/null | awk -v p="$1" 'index($0,p)>0 {print $1; exit}' || true; }
thread_snapshot() {
  local role=$1 pid=$2 dest=$3
  python3 - "$pid" "$dest" <<'PY'
import glob,os,sys,time
pid,path=sys.argv[1],sys.argv[2]
clk=os.sysconf(os.sysconf_names['SC_CLK_TCK'])
with open(path,'w') as o:
 o.write(f'# captured_ns={time.time_ns()} pid={pid} hz={clk}\n')
 for f in glob.glob(f'/proc/{pid}/task/*/stat'):
  try:
   s=open(f).read(); q=s.rfind(')'); name=s[s.index('(')+1:q]; a=s[q+2:].split(); tid=os.path.basename(os.path.dirname(f)); ticks=int(a[11])+int(a[12]);
   status=open(f'/proc/{pid}/task/{tid}/status').read().splitlines()
   vals={x.split(':',1)[0]:int(x.split(':',1)[1].strip().split()[0]) for x in status if x.startswith(('voluntary_ctxt_switches:', 'nonvoluntary_ctxt_switches:'))}
   o.write(f'{tid}\t{ticks}\t{vals.get("voluntary_ctxt_switches",0)}\t{vals.get("nonvoluntary_ctxt_switches",0)}\t{name}\n')
  except (OSError,ValueError,IndexError): pass
PY
}
start_jfr() {
 local role=$1 pid=$2
 local file=$JOB/profiles/${STAMP}-${role}.jfr
 jcmd "$pid" JFR.start name=active settings="$JOB/tools/java-profile.jfc" filename="$file" maxsize=512m >"$CELL/jfr-start-$role.log" 2>&1
}
stop_jfr() {
 local role=$1 pid=$2
 jcmd "$pid" JFR.stop name=active >"$CELL/jfr-stop-$role.log" 2>&1 || true
}
start_strace() { local role=$1 pid=$2; setsid strace -c -f -p "$pid" -o "$CELL/strace-$role.txt" >"$CELL/strace-$role.log" 2>&1 & if [[ "$role" == source ]]; then strace_source_pid=$!; else strace_target_pid=$!; fi; }
start_perf() { local role=$1 pid=$2; setsid perf stat -p "$pid" -e task-clock,context-switches,cpu-migrations -x, -o "$CELL/perf-$role.csv" >"$CELL/perf-$role.stdout" 2>"$CELL/perf-$role.stderr" & if [[ "$role" == source ]]; then perf_source_pid=$!; else perf_target_pid=$!; fi; }
stop_group() { local p=${1:-}; [[ -n "$p" ]] || return 0; kill -INT -- "-$p" 2>/dev/null || kill -INT "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; }
cleanup() { stop_group "$sampler_source_pid"; stop_group "$sampler_target_pid"; stop_group "$strace_source_pid"; stop_group "$strace_target_pid"; stop_group "$perf_source_pid"; stop_group "$perf_target_pid"; [[ -n "$runner_pid" ]] && kill -0 "$runner_pid" 2>/dev/null && { kill -TERM "$runner_pid" 2>/dev/null || true; wait "$runner_pid" || true; }; }
trap cleanup EXIT
cd "$REPO/framework/bench/grpc"
bash "$RUNNER" >"$CELL/runner.log" 2>&1 & runner_pid=$!
seen=0
while kill -0 "$runner_pid" 2>/dev/null; do
 source_pid=${source_pid:-$(findpid "$SRC_MATCH")}; target_pid=${target_pid:-$(findpid "$TGT_MATCH")}
 p=$(phase)
 if [[ "$p" == active && $seen == 0 ]]; then
  seen=1
  printf 'start_ns=%s\nsource_pid=%s\ntarget_pid=%s\n' "$(date +%s%N)" "$source_pid" "$target_pid" >"$CELL/profile-window.txt"
  thread_snapshot source "$source_pid" "$CELL/threads-start-source.tsv"
  thread_snapshot target "$target_pid" "$CELL/threads-start-target.tsv"
  if [[ "$MODE" == counts ]]; then start_strace source "$source_pid"; start_strace target "$target_pid"; fi
  if [[ "$MODE" == perf ]]; then start_perf source "$source_pid"; start_perf target "$target_pid"; fi
  if [[ "$MODE" == profile ]]; then
   python3 "$JOB/tools/sample-threads.py" "$source_pid" "$CELL/thread-cpu-source.tsv" & sampler_source_pid=$!
   python3 "$JOB/tools/sample-threads.py" "$target_pid" "$CELL/thread-cpu-target.tsv" & sampler_target_pid=$!
   start_jfr source "$source_pid"; start_jfr target "$target_pid"
  fi
 elif [[ "$p" == idle && $seen == 1 ]]; then
  thread_snapshot source "$source_pid" "$CELL/threads-end-source.tsv"
  thread_snapshot target "$target_pid" "$CELL/threads-end-target.tsv"
  if [[ "$MODE" == profile ]]; then
   stop_group "$sampler_source_pid"; sampler_source_pid=; stop_group "$sampler_target_pid"; sampler_target_pid=
   stop_jfr source "$source_pid"; stop_jfr target "$target_pid"
  fi
  if [[ "$MODE" == counts ]]; then stop_group "$strace_source_pid"; strace_source_pid=; stop_group "$strace_target_pid"; strace_target_pid=; fi
  if [[ "$MODE" == perf ]]; then stop_group "$perf_source_pid"; perf_source_pid=; stop_group "$perf_target_pid"; perf_target_pid=; fi
  printf 'end_ns=%s\n' "$(date +%s%N)" >>"$CELL/profile-window.txt"
  break
 elif [[ "$p" == failed ]]; then echo 'FAILED' >"$CELL/profile-window.txt"; break
 fi
 sleep .02
done
wait "$runner_pid"; runner_pid=; trap - EXIT
[[ $seen == 1 ]] || echo 'ACTIVE_PHASE_NOT_OBSERVED' >"$CELL/profile-window.txt"
echo "$STAMP complete"
