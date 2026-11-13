#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
TOOLS=$JOB/tools/node
START=$SECONDS
variants=("$@")
CELL_FILTER=${CELL_FILTER:-}
ready=0
for _ in $(seq 1 60); do
  ready=1
  for cpu in 0 8; do for state in 2 3; do
    [[ "$(cat /sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}/disable 2>/dev/null || echo unavailable)" == 1 ]] || ready=0
  done; done
  [[ "$ready" == 1 ]] && break
  sleep 0.25
done
[[ "$ready" == 1 ]] || { echo 'quiet guard did not disable C2/C3 before measurement' >&2; exit 1; }
printf '%s guard_ready C2/C3 disabled, SCHED_OTHER enforcement and 0-5/8-13 isolation active\n' "$(date -u +%FT%TZ)" >> "$JOB/progress.md"
for variant in "${variants[@]}"; do
  for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
    read -r pattern size <<<"$pair"
    if [[ -n "$CELL_FILTER" && ",${CELL_FILTER}," != *",${pattern}-${size},"* ]]; then continue; fi
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" warm
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" counted
  done
done
# The requested quick method adds two more counted runs on both adjacent steps when a
# cell is near its pass boundary. L5~L7 uses its 21% budget; other steps use 7%.
if ((${#variants[@]} == 2)); then
  prev=${variants[0]} next=${variants[1]}
  threshold=0.07
  [[ "$next" == L5_7 ]] && threshold=0.21
  while read -r pattern size; do
    for phase in repeat2 repeat3; do
      "$TOOLS/run-cell.sh" "$prev" "$pattern" "$size" "$phase"
      "$TOOLS/run-cell.sh" "$next" "$pattern" "$size" "$phase"
    done
  done < <(python3 - "$JOB/raw/node" "$prev" "$next" "$threshold" "$CELL_FILTER" <<'PY'
import json,pathlib,sys
root=pathlib.Path(sys.argv[1]); prev,nxt,threshold=sys.argv[2],sys.argv[3],float(sys.argv[4])
cell_filter=set(sys.argv[5].split(',')) if sys.argv[5] else None
for pattern,size in [('send-saturation',1024),('send-saturation',4096),('request-serial',1024),('request-serial',4096),('request-backpressure',1024),('request-backpressure',4096)]:
    if cell_filter and f'{pattern}-{size}' not in cell_filter: continue
    values=[]
    for variant in (prev,nxt):
        paths=sorted((root/variant/f'{pattern}-{size}'/'counted').glob(f'attempt*/run1/zlink-framework-node-{pattern}-{size}/results.json'))
        if not paths: raise SystemExit(f'missing counted result for {variant} {pattern}-{size}')
        values.append(json.loads(paths[-1].read_text())['cells'][0]['throughput_per_second'])
    loss=1-values[1]/values[0]
    low,high=(.19,.23) if nxt=='L5_7' else (.05,.09)
    if low <= loss <= high: print(pattern,size)
PY
)
fi
(( SECONDS - START < 1500 )) || { echo '30-minute hold safety boundary reached' >&2; exit 124; }
