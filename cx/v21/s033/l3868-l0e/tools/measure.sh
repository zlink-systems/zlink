#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868-l0e
REPO=/root/worktree/zlink-v21-s033/l3868
export ZLINK_CPP_LADDER_LEVEL=0 IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp
mkdir -p "$JOB/raw/aggregates"
for pattern in send-saturation request-serial request-backpressure; do
  for size in 1024 4096; do
    export PATTERNS="$pattern" PAYLOAD_SIZES="$size" OUTPUT_DIR="$JOB/raw/L0/$pattern-$size"
    export RUNS=3 RUN_START=1
    if [[ "$pattern" == send-saturation && "$size" == 4096 ]]; then RUNS=5; fi
    "$JOB/tools/cpp-cell-runner.sh"
    agg="$JOB/raw/aggregates/$pattern-$size.json"
    python3 "$REPO/framework/bench/grpc/tools/bench_aggregate.py" --lang cpp --min-runs 3 --baseline zlink-cpp \
      --runs-glob "$OUTPUT_DIR/run*" --json-out "$agg" >"$JOB/raw/aggregates/$pattern-$size.md"
    if [[ "$RUNS" == 3 ]] && python3 - "$agg" <<'PYG5'
import json,sys
rows=json.load(open(sys.argv[1]))['rows']
raise SystemExit(0 if any(r['g5_status']=='fail' for r in rows) else 1)
PYG5
    then
      cp "$agg" "${agg%.json}-3runs.json"
      printf '\n- %s %s-%s: G5 failed at 3 runs; add runs 4/5, preserve all 3 originals.\n' "$(date -u +%FT%TZ)" "$pattern" "$size" >> "$JOB/progress.md"
      RUN_START=4 RUNS=5 "$JOB/tools/cpp-cell-runner.sh"
      python3 "$REPO/framework/bench/grpc/tools/bench_aggregate.py" --lang cpp --min-runs 3 --baseline zlink-cpp \
        --runs-glob "$OUTPUT_DIR/run*" --json-out "$agg" >"$JOB/raw/aggregates/$pattern-$size-5runs.md"
    fi
    python3 "$JOB/tools/cell-summary.py" "$pattern" "$size"
  done
done
export OUTPUT_DIR="$JOB/raw/cross/L0" RUNS=3 RUN_START=1
"$JOB/tools/cross.sh"
python3 "$JOB/tools/summarize.py"
if python3 - "$JOB/raw/summary.json" <<'PYCROSS'
import json,sys
raise SystemExit(0 if any(s['g5']=='fail' for s in json.load(open(sys.argv[1]))['cross'].values()) else 1)
PYCROSS
then
  cp "$JOB/raw/summary.json" "$JOB/raw/summary-first3-cross.json"
  for agg in "$JOB"/raw/aggregates/cross-*.json; do cp "$agg" "${agg%.json}-3runs.json"; done
  printf '\n- %s cross G5 failed at 3 runs; add fresh runs 4/5 for all three combinations.\n' "$(date -u +%FT%TZ)" >>"$JOB/progress.md"
  RUN_START=4 RUNS=5 "$JOB/tools/cross.sh"
  python3 "$JOB/tools/summarize.py"
fi
while read -r pattern size; do
  [[ -n "$pattern" ]] || continue
  profile="$JOB/raw/profiles/$pattern-$size"
  mkdir -p "$profile"
  if RUNS=1 IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp PATTERNS="$pattern" PAYLOAD_SIZES="$size" OUTPUT_DIR="$profile" \
    "$JOB/tools/profile-runner.sh" >"$profile/driver.log" 2>&1; then
    for data in "$profile"/run*/*/capture/perf.data; do
      perf report --stdio --no-children --sort pid,comm,dso,symbol -i "$data" >"$(dirname "$data")/profile.txt" 2>"$(dirname "$data")/report-error.txt"
      perf report --stdio --children --call-graph graph,0.5,caller -i "$data" >"$(dirname "$data")/stacks.txt" 2>"$(dirname "$data")/stacks-error.txt"
    done
  else
    printf 'profile failed; preserve raw outputs\n' >"$profile/failed.txt"
  fi
done < <(python3 - "$JOB/raw/summary.json" <<'PYLOW'
import json,sys
for c in json.load(open(sys.argv[1]))['matrix']:
 if c.get('ratio',1)<.95:print(c['pattern'],c['payload_size'])
PYLOW
)
