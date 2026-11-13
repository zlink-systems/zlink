#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868
matrix="$1"; pattern="$2"; size="$3"
AGG=/root/worktree/zlink-v21-s033/agg-main/framework/bench/grpc/tools/bench_aggregate.py
mkdir -p "$JOB/raw/cell-aggregates"
json="$JOB/raw/cell-aggregates/$pattern-$size.json"
python3 "$AGG" --lang cpp --min-runs 3 --baseline zlink-cpp --runs-glob "$matrix/run*" --json-out "$json" >"$JOB/raw/cell-aggregates/$pattern-$size.md"
rc=0
python3 - "$json" <<'PY' || rc=$?
import json,sys
x=json.load(open(sys.argv[1]));rows={r['implementation']:r for r in x['rows']}
a=rows['zlink-framework-cpp'];b=rows['zlink-cpp'];assert a['runs']==b['runs']==3
ratio=a['median']['throughput_per_second']/b['median']['throughput_per_second']
print('L0/binding ratio',ratio)
raise SystemExit(0 if ratio<.95 else 1)
PY
[[ "$rc" != 1 ]] || exit 0
[[ "$rc" == 0 ]] || exit "$rc"
commit="$(git -C /root/worktree/zlink-v21-s033/l3868 rev-parse --short HEAD)"
profile="$JOB/raw/profiles-owner/$pattern-$commit"
[[ ! -e "$profile/done" ]] || exit 0
mkdir -p "$profile"
if RUNS=1 IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp PATTERNS="$pattern" PAYLOAD_SIZES="$size" OUTPUT_DIR="$profile" \
  "$JOB/tools/profile-runner.sh" >"$profile/driver.log" 2>&1; then
  for data in "$profile"/run*/*/capture/perf.data; do
    perf report --stdio --no-children --sort pid,comm,dso,symbol -i "$data" >"$(dirname "$data")/profile.txt" 2>"$(dirname "$data")/report-error.txt" || true
  done
  touch "$profile/done"
else
  printf 'diagnostic profile failed; retain logs, continue official cells\n' >"$profile/failed.txt"
fi
