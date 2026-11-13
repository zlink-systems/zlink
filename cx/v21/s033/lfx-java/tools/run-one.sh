#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/lfx-java
root=/root/worktree/zlink-v21-s033/lfx-java
step=$1 pattern=$2 size=$3 kind=$4
out="$job/raw/$step/$pattern-$size/$kind/1"
[[ ! -e "$out" ]] || { echo "raw exists: $out" >&2; exit 2; }
mkdir -p "$out"
client_jar="$root/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
server_jar="$root/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar"
client_bin="$root/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client"
server_bin="$root/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
commit=$(git -C "$root" rev-parse HEAD); dirty=clean; [[ -z "$(git -C "$root" status --porcelain)" ]] || dirty=dirty
base=$(git -C "$root" merge-base HEAD origin/main)
sha=$(sha256sum "$client_jar" "$server_jar" "$client_bin" "$server_bin" | awk '{printf "%s=%s;",$2,$1}')
for src in "$root/framework/bench/grpc/java/client/src" "$root/framework/bench/grpc/java/zlink-framework-server/src"; do
  while IFS= read -r f; do [[ "$client_jar" -nt "$f" || "$server_jar" -nt "$f" ]] || { echo "stale artifact: $f" >&2; exit 2; }; done < <(find "$src" -type f -name '*.java')
done
flag=ZLINK_BENCH_LADDER_$(printf %s "$step" | tr '[:lower:]' '[:upper:]'); [[ "$step" != L5_7 ]] || flag=ZLINK_BENCH_LADDER_L5_7
printf -- '- %s %s %s-%s %s build{HEAD=%s dirty=%s merge_base=%s artifacts_sha=%s}; receive(binding/L-step): `ZLinkRawBenchServer.java:94-109` / `ZLinkFrameworkBenchServer.java:151-177,211-237(L2),351-419(L3),272-316(L4)`; reply(binding/L-step): `ZLinkRawBenchServer.java:115-127` / `ZLinkFrameworkBenchServer.java:249-269(L2),400-419(L3),291-337(L4)`; per-message waits: one blocking receive each; allocations: L2 envelope, L3 frame, L4 typed JSON per ladder-steps; metrics: same counters/atomics; copies: no extra driver copies; call-shape.md; raw=%s\n' "$(date -u +%FT%TZ)" "$step" "$pattern" "$size" "$kind" "$commit" "$dirty" "$base" "$sha" "$out" >> "$job/progress.md"
args=(env -u ZLINK_BENCH_LADDER_L0 -u ZLINK_BENCH_LADDER_L1 -u ZLINK_BENCH_LADDER_L2 -u ZLINK_BENCH_LADDER_L3 -u ZLINK_BENCH_LADDER_L4 -u ZLINK_BENCH_LADDER_L5_7)
args+=(SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS=zlink-framework-java PATTERNS="$pattern" PAYLOAD_SIZES="$size" WARMUP_SECONDS=10 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 SUBMIT_CONCURRENCY=8 WINDOW=100 SCHEDULING_RECORD=1 OUTPUT_DIR="$out" RUN_STAMP="lfx-${step}-${pattern}-${size}-${kind}-$(date -u +%Y%m%dT%H%M%SZ)")
args+=("$flag=1" bash "$root/../lfx-java/tools/run_local_binding.sh")
"${args[@]}" > "$out/driver.log" 2>&1 || { tail -60 "$out/driver.log" >&2; return 1; }
result="$out/run1/zlink-framework-java-$pattern-$size/results.json"
sched="$out/run1/zlink-framework-java-$pattern-$size/scheduling.txt"
[[ -f "$result" && -f "$sched" ]] || { echo "missing result/scheduling: $out" >&2; return 1; }
readout=$(python3 - "$result" "$sched" <<'PY'
import json,pathlib,sys
c=json.loads(pathlib.Path(sys.argv[1]).read_text())['cells'][0]
s=pathlib.Path(sys.argv[2]).read_text()
ok=c['errors']==c['server_errors']==c['target_stats']['errors']==0 and c['submitted']==c['completed']==c['target_stats']['received'] and 'scheduling_invalid=' not in s
for phase,expect in (('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')):
 b=next((p for p in s.split('\n\n') if p.startswith(phase+'_utc=')), '')
 pol=[x for x in b.splitlines() if 'current scheduling policy:' in x]
 aff=[x.rsplit('cpus=',1)[1] for x in b.splitlines() if x.startswith(phase+'_task_affinity ') and 'cpus=' in x]
 ok=ok and bool(pol) and all('current scheduling policy: SCHED_OTHER' in x for x in pol) and bool(aff) and all(x==expect for x in aff)
print(('yes' if ok else 'no'),c['throughput_per_second'],c['errors']+c['server_errors']+c['target_stats']['errors'],c['submitted'],c['completed'],c['target_stats']['received'],sep='\t')
PY
)
IFS=$'\t' read -r valid rate errors submitted completed received <<< "$readout"
printf -- '- result %s %s-%s %s valid=%s msg/s=%s errors=%s submitted=%s completed=%s received=%s raw=%s scheduling=%s\n' "$step" "$pattern" "$size" "$kind" "$valid" "$rate" "$errors" "$submitted" "$completed" "$received" "$result" "$sched" >> "$job/progress.md"
printf '%s %s %s %s %s %s %s\n' "$step" "$pattern" "$size" "$kind" "$valid" "$rate" "$result" >> "$job/raw/index.tsv"
[[ "$valid" == yes ]] || { echo "invalid run retained: $result" >&2; exit 1; }
