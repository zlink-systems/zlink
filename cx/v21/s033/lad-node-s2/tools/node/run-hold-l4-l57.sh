#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s2
TOOLS=$JOB/tools/node
START=$SECONDS
cells=('send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096')
printf '%s guard_ready; variants=L4,L5_7; CPU source=0-5 target=8-13; C2/C3 off required\n' "$(date -u +%FT%TZ)" >> "$JOB/progress.md"
for pair in "${cells[@]}"; do
  read -r pattern size <<<"$pair"
  "$TOOLS/run-cell.sh" L4 "$pattern" "$size" warm
  "$TOOLS/run-cell.sh" L4 "$pattern" "$size" counted
  "$TOOLS/run-cell.sh" L5_7 "$pattern" "$size" warm
  "$TOOLS/run-cell.sh" L5_7 "$pattern" "$size" counted
done
while read -r pattern size; do
  [[ -n "$pattern" ]] || continue
  for phase in confirm2 confirm3; do
    "$TOOLS/run-cell.sh" L4 "$pattern" "$size" "$phase"
    "$TOOLS/run-cell.sh" L5_7 "$pattern" "$size" "$phase"
  done
done < <(python3 - "$JOB/raw/node" <<'PY'
import json,pathlib,sys
root=pathlib.Path(sys.argv[1])
for pattern,size in [('send-saturation','1024'),('send-saturation','4096'),('request-serial','1024'),('request-serial','4096'),('request-backpressure','1024'),('request-backpressure','4096')]:
    vals=[]
    for variant in ('L4','L5_7'):
        paths=sorted((root/variant/f'{pattern}-{size}'/'counted').glob('attempt*/valid-result.path'))
        if not paths: vals.append(None); continue
        result=pathlib.Path(paths[-1].read_text().strip())
        vals.append(json.loads(result.read_text())['cells'][0]['throughput_per_second'])
    if None in vals: continue
    loss=1-vals[1]/vals[0]
    if .18 <= loss <= .24: print(pattern,size)
PY
)
(( SECONDS < 570 )) || { echo '10-minute hold safety boundary reached' >&2; exit 124; }
