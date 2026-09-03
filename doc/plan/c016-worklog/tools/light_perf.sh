#!/usr/bin/env bash
# 1024B-only quick comparison: ROUTER_ROUTER, SENDSEND, REQREP. Prints candidate/baseline ratios.
# usage: light_perf.sh [tag]
export ZLINK_WORK=$HOME/project/zlink-work
TAG=${1:-light}; W=$ZLINK_WORK/c016/light-results/$TAG; mkdir -p $W
BASE=$HOME/project/zlink-perf-core-0.15.1; CAND=${CANDIDATE_ROOT:-$HOME/project/zlink}
SINGLE="ROUTER_ROUTER:tcp ROUTER_ROUTER:inproc DEALER_ROUTER_REQREP:tcp ROUTER_ROUTER_REQREP:tcp"
MULTI="DEALER_ROUTER_SENDSEND:tcp ROUTER_ROUTER_SENDSEND:tcp DEALER_ROUTER_REQREP:tcp ROUTER_ROUTER_REQREP:tcp"
run() { # suite side root pattern transport
  local runner=$3/bindings/c/perf/run_benchmarks.sh; [[ $1 == multi ]] && runner=$3/bindings/c/perf/run_benchmarks_multi.sh
  ( cd $3 && timeout 600 $runner --pattern $4 --transports $5 --msg-sizes 1024 --runs ${LIGHT_RUNS:-1} --results-tag $TAG-$2-$4-$5 --results-dir $W/$2 ) > $W/$2-$1-$4-$5.log 2>&1
  grep -E '^RESULT,' $W/$2-$1-$4-$5.log | sed "s/^/$1,/" >> $W/$2.csv
}
: > $W/b.csv; : > $W/c.csv
for suite in single multi; do
  list=$SINGLE; [[ $suite == multi ]] && list=$MULTI
  for cell in $list; do p=${cell%%:*}; t=${cell#*:}; run $suite b $BASE $p $t; run $suite c $CAND $p $t; done
done
python3 - "$W" <<'PY'
import sys,csv,collections
W=sys.argv[1]
def load(f):
    d={}
    for row in csv.reader(open(f)):
        if len(row)<7: continue
        # suite,RESULT,current,PAT,tr,size,metric,val
        d[(row[0],row[3],row[4],row[5],row[6])]=float(row[7])
    return d
b=load(f'{W}/b.csv'); c=load(f'{W}/c.csv')
print('| suite | pattern/transport | size | thr | lat | p95 | p99 |'); print('|---|---|---|---|---|---|---|')
keys=sorted({(k[0],k[1],k[2],k[3]) for k in b})
for s,p,t,z in keys:
    r=lambda m: (f"{c[(s,p,t,z,m)]/b[(s,p,t,z,m)]:.3f}" if (s,p,t,z,m) in c and (s,p,t,z,m) in b and b[(s,p,t,z,m)] else '-')
    print(f"| {s} | {p}/{t} | {z} | {r('throughput')} | {r('latency')} | {r('latency_p95')} | {r('latency_p99')} |")
PY
