#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-s4
ROOT=/root/worktree/zlink-v21-s033/lad-java-l57p
RUNNER=/root/jobs/fx/cx/v21/s033/jl57bp/tools/java/run_local_binding.sh
cd "$ROOT"
commit=$(git rev-parse HEAD)
dirty=clean; [[ -z "$(git status --porcelain)" ]] || dirty=dirty
base=$(git merge-base HEAD origin/main)
client_jar="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
server_jar="$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar"
client_bin="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client"
server_bin="$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
sha=$(sha256sum "$client_jar" "$server_jar" "$client_bin" "$server_bin" | awk '{printf "%s=%s;",$2,$1}')
[[ "$commit" == cffa8009d33bb520fe3b783c41a2184b2d91902d && "$dirty" == clean ]] || { echo 'unexpected source state' >&2; exit 2; }
for src in "$ROOT/framework/bench/grpc/java/client/src" "$ROOT/framework/bench/grpc/java/zlink-framework-server/src"; do
  while IFS= read -r f; do [[ "$client_jar" -nt "$f" || "$server_jar" -nt "$f" ]] || { echo "stale artifact: $f" >&2; exit 2; }; done < <(find "$src" -type f -name '*.java')
done
printf 'hold_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/holds.txt"
trap 'printf "hold_end_utc=%s\n" "$(date -u +%FT%TZ)" >> "$JOB/raw/holds.txt"' EXIT
run_one() {
  local step="$1" pattern="$2" size="$3" kind="$4" idx="$5" out stamp flag
  out="$JOB/raw/$step/$pattern-$size/$kind/$idx"
  [[ ! -e "$out" ]] || { echo "raw exists: $out" >&2; exit 2; }
  mkdir -p "$out"
  stamp="s4-${step}-${pattern}-${size}-${kind}-${idx}-$(date -u +%Y%m%dT%H%M%SZ)"
  case "$step" in L4) flag=ZLINK_BENCH_LADDER_L4;; L5_7) flag=ZLINK_BENCH_LADDER_L5_7;; esac
  printf -- '- %s %s %s-%s %s/%s build{HEAD=%s dirty=%s merge_base=%s artifacts_sha=%s}; receive: binding `ZLinkRawBenchServer.java:94-109` || L4 `ZLinkFrameworkBenchServer.java:272-316` / L5~L7 `:419-435`; reply: binding `ZLinkRawBenchServer.java:115-127` || L4 `ZLinkFrameworkBenchServer.java:291-337` / L5~L7 `BenchTypedEchoHandler.java:13-27`; per-message: binding/L4 one blocking receive, fresh reply allocation, same metrics atomics/copies; L5~L7 adds owner queue/worker wait and Framework allocations, same metrics owner/atomics, no extra copies; call-shape.md; raw=%s\n' "$(date -u +%FT%TZ)" "$step" "$pattern" "$size" "$kind" "$idx" "$commit" "$dirty" "$base" "$sha" "$out" >> "$JOB/progress.md"
  env -u ZLINK_BENCH_LADDER_L0 -u ZLINK_BENCH_LADDER_L1 -u ZLINK_BENCH_LADDER_L2 -u ZLINK_BENCH_LADDER_L3 -u ZLINK_BENCH_LADDER_L4 -u ZLINK_BENCH_LADDER_L5_7 SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS=zlink-framework-java PATTERNS="$pattern" PAYLOAD_SIZES="$size" \
    WARMUP_SECONDS=10 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 SUBMIT_CONCURRENCY=8 WINDOW=100 \
    SCHEDULING_RECORD=1 OUTPUT_DIR="$out" RUN_STAMP="$stamp" "$flag"=1 \
    bash "$RUNNER" > "$out/driver.log" 2>&1 || { tail -80 "$out/driver.log" >&2; echo "runner failed; raw retained at $out" >&2; return 1; }
  local result="$out/run1/zlink-framework-java-$pattern-$size/results.json"
  local sched="$out/run1/zlink-framework-java-$pattern-$size/scheduling.txt"
  [[ -f "$result" && -f "$sched" ]] || { echo "missing raw: $out" >&2; return 1; }
  python3 - "$result" "$sched" "$step" <<'PY'
import json,pathlib,sys
r=json.loads(pathlib.Path(sys.argv[1]).read_text())['cells'][0]
s=pathlib.Path(sys.argv[2]).read_text()
print(json.dumps(r,sort_keys=True))
print('scheduling_invalid=' + str('scheduling_invalid=' in s))
print('affinity_evidence=' + str(all(x in s for x in ('source_start_task_affinity','mid_active_source_task_affinity','target_start_task_affinity','mid_active_target_task_affinity'))))
print('sched_policy_counts=' + str(s.count('current scheduling policy: SCHED_OTHER')))
print('affinity_values=' + ','.join(sorted(set(line.split('cpus=')[-1] for line in s.splitlines() if '_task_affinity ' in line))))
PY
  python3 - "$result" "$sched" <<'PY'
import json,pathlib,sys
c=json.loads(pathlib.Path(sys.argv[1]).read_text())['cells'][0]; s=pathlib.Path(sys.argv[2]).read_text()
valid=(c['errors']==0 and c['server_errors']==0 and c['target_stats']['errors']==0 and c['submitted']==c['completed']==c['target_stats']['received'] and 'scheduling_invalid=' not in s)
phases=(('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13'))
for phase,expect in phases:
  block=next((p for p in s.split('\n\n') if p.startswith(phase+'_utc=')), '')
  pol=[x for x in block.splitlines() if 'current scheduling policy:' in x]
  aff=[x.rsplit('cpus=',1)[1] for x in block.splitlines() if x.startswith(phase+'_task_affinity ') and 'cpus=' in x]
  valid=valid and bool(pol) and all('current scheduling policy: SCHED_OTHER' in x for x in pol) and bool(aff) and all(x==expect for x in aff)
print('VALID' if valid else 'INVALID')
PY
  local valid rate submitted completed received errors
  valid=$(python3 - "$result" "$sched" <<'PY'
import json,pathlib,sys
c=json.loads(pathlib.Path(sys.argv[1]).read_text())['cells'][0]; s=pathlib.Path(sys.argv[2]).read_text()
ok=c['errors']==c['server_errors']==c['target_stats']['errors']==0 and c['submitted']==c['completed']==c['target_stats']['received'] and 'scheduling_invalid=' not in s
for phase,expect in (('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')):
 b=next((p for p in s.split('\n\n') if p.startswith(phase+'_utc=')), '')
 pol=[x for x in b.splitlines() if 'current scheduling policy:' in x]; aff=[x.rsplit('cpus=',1)[1] for x in b.splitlines() if x.startswith(phase+'_task_affinity ') and 'cpus=' in x]
 ok=ok and bool(pol) and all('current scheduling policy: SCHED_OTHER' in x for x in pol) and bool(aff) and all(x==expect for x in aff)
print('yes' if ok else 'no')
PY
)
  rate=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["throughput_per_second"])' "$result")
  submitted=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["submitted"])' "$result")
  completed=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["completed"])' "$result")
  received=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["target_stats"]["received"])' "$result")
  errors=$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1]))["cells"][0];print(c["errors"]+c["server_errors"]+c["target_stats"]["errors"])' "$result")
  printf -- '- result %s %s-%s %s/%s valid=%s msg/s=%s errors=%s submitted=%s completed=%s received=%s raw=%s scheduling=%s\n' "$step" "$pattern" "$size" "$kind" "$idx" "$valid" "$rate" "$errors" "$submitted" "$completed" "$received" "$result" "$sched" >> "$JOB/progress.md"
  printf '%s %s %s %s %s %s %s %s\n' "$step" "$pattern" "$size" "$kind" "$idx" "$valid" "$rate" "$result" >> "$JOB/raw/index.tsv"
}
for pattern in send-saturation request-serial request-backpressure; do
  for size in 1024 4096; do
    run_one L4 "$pattern" "$size" warm 1
    run_one L4 "$pattern" "$size" counted 1
    run_one L5_7 "$pattern" "$size" warm 1
    run_one L5_7 "$pattern" "$size" counted 1
  done
done
