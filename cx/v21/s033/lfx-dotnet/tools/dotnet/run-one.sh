#!/usr/bin/env bash
set -euo pipefail
pattern="$1"; payload="$2"; out="$3"; stamp="$4"; warmup="$5"; level="$6"
root=/root/worktree/zlink-v21-s033/lfx-dotnet/framework/bench/grpc/dotnet
impl=zlink-dotnet
target=zlink-dotnet
unset ZLINK_DOTNET_LADDER_LEVEL ZLINK_DOTNET_LADDER_CLIENT ZLINK_DOTNET_RAW_TARGET
if [[ "$level" != binding ]]; then
  export ZLINK_DOTNET_LADDER_LEVEL="$level"
  target=zlink-framework-dotnet
  if [[ "$level" == L5_7 ]]; then
    impl=zlink-framework-dotnet
    unset ZLINK_DOTNET_LADDER_CLIENT ZLINK_DOTNET_RAW_TARGET
  else
    export ZLINK_DOTNET_LADDER_CLIENT=raw ZLINK_DOTNET_RAW_TARGET=framework
  fi
fi
export SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS="$impl" TARGET_IMPLEMENTATION="$target"
export PATTERNS="$pattern" PAYLOAD_SIZES="$payload" WARMUP_SECONDS="$warmup" DURATION_SECONDS=5
export SOURCE_CPUS=0-5 TARGET_CPUS=8-13 OUTPUT_DIR="$out" RUN_STAMP="$stamp"
export SUBMIT_CONCURRENCY=8 WINDOW=100 SCHEDULING_RECORD=1
bash /root/jobs/fx/cx/v21/s033/lfx-dotnet/tools/dotnet/run_local_binding.sh >"$out/driver.log" 2>&1
result="$out/run1/${impl}-${pattern}-${payload}/results.json"
sched="$out/run1/${impl}-${pattern}-${payload}/scheduling.txt"
[[ -f "$result" && -f "$sched" ]] || { echo "missing result/scheduling $result $sched" >&2; exit 1; }
python3 - "$result" "$sched" "$out/run1/${impl}-${pattern}-${payload}" <<'PY'
import json,pathlib,sys
r=json.loads(pathlib.Path(sys.argv[1]).read_text())['cells'][0]
s=pathlib.Path(sys.argv[2]).read_text()
assert all(r.get(k,0)==0 for k in ('errors','server_errors')) and r['target_stats']['errors']==0
assert r['submitted']==r['completed']==r['target_stats']['received'], (r['submitted'],r['completed'],r['target_stats']['received'])
assert 'scheduling_invalid=' not in s, s
assert 'affinity_invalid=' not in s, s
for phase,expected in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
    policies=[line for line in s.splitlines() if line.startswith(phase+'_tid=')]
    affinities=[line.rsplit('cpus=',1)[1] for line in s.splitlines() if line.startswith(phase+'_affinity ') and 'cpus=' in line]
    assert policies and len(affinities)==len(policies), (phase,len(policies),len(affinities))
    low,high=map(int,expected.split('-'))
    for value in affinities:
        cpus=[]
        for part in value.split(','):
            ends=part.split('-')
            assert len(ends) in (1,2), (phase,value)
            first=int(ends[0]); last=int(ends[-1])
            assert low<=first<=last<=high, (phase,value,expected)
            cpus.extend(range(first,last+1))
        assert cpus, (phase,value)
    policy_ix=[i for i,line in enumerate(s.splitlines()) if line.startswith(phase+'_tid=')]
    lines=s.splitlines()
    for i in policy_ix:
        chunk='\n'.join(lines[i:i+3])
        assert 'current scheduling policy: SCHED_OTHER' in chunk, (phase,chunk)
print(r['throughput_per_second'])
PY
