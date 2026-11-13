#!/usr/bin/env bash
set -euo pipefail
WT=/root/worktree/zlink-v21-s033/lad-node
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0f
BASE="$OUTPUT_DIR"
RUNNER="$JOB/tools/node-run-local.sh"
run_cell() {
  local group="$1" pattern="$2" payload="$3" runs="$4"
  local out="$BASE/$group/${pattern}-${payload}"
  "$JOB/tools/progress-before-measurement.sh" "final-${group}-${pattern}-${payload}"
  env IMPLEMENTATIONS=zlink-node,zlink-framework-node \
    PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS="$runs" \
    RUN_STAMP="4350l0f-final-${group}-${pattern}-${payload}" \
    OUTPUT_DIR="$out" SKIP_BUILD=1 ZLINK_LADDER_L0_SERVER=1 \
    bash "$RUNNER"
}
for pattern in request-serial request-backpressure send-saturation; do
  for payload in 1024 4096; do
    run_cell three "$pattern" "$payload" 3
  done
done
python3 - "$BASE" <<'PY'
import glob, json, statistics, sys
base=sys.argv[1]
for pattern in ('request-serial','request-backpressure','send-saturation'):
  for payload in (1024,4096):
    rates=[]
    for impl in ('zlink-node','zlink-framework-node'):
      files=sorted(glob.glob(f'{base}/three/{pattern}-{payload}/run*/{impl}-{pattern}-{payload}/results.json'))
      vals=[]
      for path in files:
        cell=json.load(open(path))['cells'][0]
        vals.append(cell['throughput_per_second'])
      median=statistics.median(vals)
      spread=max(abs(v-median) for v in vals)/median*100
      rates.append((impl,spread))
    if any(spread > 10 for _,spread in rates):
      print(f'G5 spread >10%: extending {pattern}-{payload} to five runs; {rates}', flush=True)
      open(f'{base}/extend-{pattern}-{payload}.flag','w').close()
PY
for flag in "$BASE"/extend-*.flag; do
  [[ -e "$flag" ]] || continue
  name="${flag##*/extend-}"
  name="${name%.flag}"
  pattern="${name%-*}"
  payload="${name##*-}"
  run_cell extra2 "$pattern" "$payload" 2
done
python3 - "$BASE" > "$BASE/final-summary.tsv" <<'PY'
import glob, json, statistics, sys
base=sys.argv[1]
print('pattern\tpayload\timplementation\truns\tmedian_msg_s\tspread_percent\traw_results')
for pattern in ('request-serial','request-backpressure','send-saturation'):
  for payload in (1024,4096):
    for impl in ('zlink-node','zlink-framework-node'):
      files=[]
      for group in ('three','extra2'):
        files += sorted(glob.glob(f'{base}/{group}/{pattern}-{payload}/run*/{impl}-{pattern}-{payload}/results.json'))
      vals=[]
      for path in files:
        cell=json.load(open(path))['cells'][0]
        vals.append(cell['throughput_per_second'])
      median=statistics.median(vals)
      spread=max(abs(v-median) for v in vals)/median*100
      print(f'{pattern}\t{payload}\t{impl}\t{len(vals)}\t{median:.1f}\t{spread:.1f}\t'+';'.join(files))
PY
