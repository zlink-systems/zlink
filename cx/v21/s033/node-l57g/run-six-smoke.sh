#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-node
JOB=/root/jobs/fx/cx/v21/s033/node-l57g
TOOLS=/root/jobs/fx/cx/v21/s033/lad-node-s1/tools/node/run_local_framework.sh
PROGRESS="$JOB/progress.md"
for pattern in send-saturation request-serial request-backpressure; do
  for payload in 1024 4096; do
    for phase in warm counted; do
      out="$JOB/raw/final-smoke/$phase/${pattern}-${payload}"
      mkdir -p "$out"
      stamp="node-l57g-${phase}-${pattern}-${payload}-$(date -u +%H%M%S)"
      printf -- '- L5_7 final smoke %s %s phase=%s raw=%s\n' "$pattern" "$payload" "$phase" "$out" >> "$PROGRESS"
      (cd "$ROOT/framework/bench/grpc/node" && \
        ZLINK_LADDER_L5_7_SERVER=1 ZLINK_LADDER_L5_7_CLIENT=1 \
        IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node \
        PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS=1 SKIP_BUILD=1 \
        WARMUP_SECONDS=3 DURATION_SECONDS=3 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 SCHEDULING_RECORD=1 \
        OUTPUT_DIR="$out" RUN_STAMP="$stamp" bash "$TOOLS")
      result="$out/run1/zlink-framework-node-${pattern}-${payload}/results.json"
      schedule="$out/run1/zlink-framework-node-${pattern}-${payload}/scheduling.txt"
      python3 - "$result" "$schedule" <<'PY'
import json, pathlib, sys
result, schedule = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
cell = json.loads(result.read_text())['cells'][0]
text = schedule.read_text()
assert cell['errors'] == 0 and cell['server_errors'] == 0, cell
assert cell['target_stats']['errors'] == 0, cell
assert cell['submitted'] == cell['completed'] == cell['target_stats']['received'], cell
phases = ('source_start', 'target_start', 'mid_active_source', 'mid_active_target')
for phase in phases:
    assert f'{phase}_tid=' in text, f'missing scheduling phase {phase}: {schedule}'
policies = [line for line in text.splitlines() if 'current scheduling policy:' in line]
assert policies and all('current scheduling policy: SCHED_OTHER' in line for line in policies), schedule
for label, expected in (('source_start','0-5'), ('mid_active_source','0-5'), ('target_start','8-13'), ('mid_active_target','8-13')):
    affinities = [line.rsplit('cpus=',1)[1] for line in text.splitlines() if line.startswith(label+'_affinity ') and 'cpus=' in line]
    assert affinities and all(value == expected for value in affinities), (label, affinities, schedule)
assert 'scheduling_invalid=' not in text, schedule
print(f"{cell['pattern']} {cell['payload_size']}: submitted=completed=received={cell['completed']} errors=0")
PY
    done
  done
done
