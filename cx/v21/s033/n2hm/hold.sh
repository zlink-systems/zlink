#!/usr/bin/env bash
# Node L2 A/B: A = exp/4350-node-fixes (lfx-node), B = + n2hdr header native write. Run inside flock -x.
set -uo pipefail
J=/root/jobs/fx/cx/v21/s033/n2hm
echo "hold start $(date -u +%FT%TZ) A=$(git -C /root/worktree/zlink-v21-s033/lfx-node rev-parse --short HEAD) B=$(git -C /root/worktree/zlink-v21-s033/n2hdr-m rev-parse --short HEAD)" >> $J/results.txt
for cell in "request-backpressure 1024" "request-backpressure 4096" "request-serial 1024" "request-serial 4096" "send-saturation 1024" "send-saturation 4096"; do
  read -r p s <<< "$cell"
  for side in A B; do
    for v in L1 L2; do
      bash $J/$side/tools/node/run-cell.sh $v $p $s counted
      f=$(ls -t $J/$side/raw/node/$v/$p-$s/counted/attempt*/valid-result.path 2>/dev/null | head -1)
      r=INVALID; [ -n "$f" ] && r=$(python3 -I -c "import json,sys;print(json.load(open(open(sys.argv[1]).read().strip()))['cells'][0]['throughput_per_second'])" $f)
      echo "$(date -u +%T) $side $v $p-$s $r" >> $J/results.txt
      sleep 3
    done
  done
done
echo "hold end $(date -u +%FT%TZ)" >> $J/results.txt
