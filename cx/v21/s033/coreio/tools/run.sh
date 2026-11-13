#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/coreio
lang=${1:?}; mode=${2:?}; impl=${3:?}; pattern=${4:?}
export CAPTURE_ASYNC=0
[[ "$lang" != java ]] || export CAPTURE_ASYNC=1
export CAPTURE_MODE=$mode PATTERNS=$pattern PAYLOAD_SIZES=1024 IMPLEMENTATIONS=$impl
export RUNS=1 WARMUP_SECONDS=10 DURATION_SECONDS=30 SKIP_BUILD=1 RUN_STAMP=coreio-$lang-$mode-$impl-$pattern${COREIO_RUN_TAG:+-$COREIO_RUN_TAG}
[[ "$mode" != uprobe ]] || export DURATION_SECONDS=45
[[ "$mode" != perf ]] || export DURATION_SECONDS=60
export RECORD_BPF_FIRST=0
if [[ "$mode" == uprobe && "$pattern" == request-backpressure ]]; then export RECORD_BPF_FIRST=1; fi
if [[ ( "$mode" == strace || "$mode" == uprobe ) && "$RECORD_BPF_FIRST" == 0 ]]; then
 combined=$job/out/coreio-$lang-perf-$impl-$pattern/run1/$impl-$pattern-1024/capture-$mode/meta.json
 if [[ -s "$combined" ]]; then
  if python3 - "$combined" <<'CHECK'
import json,sys
m=json.load(open(sys.argv[1]));assert all(x in (0,130,-2) for x in m['exit_codes']);print('existing active-window capture',sys.argv[1])
CHECK
  then exit 0; fi
 fi
fi
export OUTPUT_DIR=$job/out/$RUN_STAMP
memavail_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
((memavail_kb >= 12582912)) || exit 3
mkdir -p "$OUTPUT_DIR"
exec bash "$job/runner/$lang-run_local.sh" >"$OUTPUT_DIR/runner.log" 2>&1
