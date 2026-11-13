#!/bin/bash
set -euo pipefail
bash /root/jobs/fx/cx/v21/s033/i3560b/run-wakeperf.sh
python3 - <<'PY'
import pathlib,json,subprocess,os
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560b')
rate=json.loads((J/'raw-cpu/run1/zlink-framework-cpp-request-backpressure-1024/results.json').read_text())['cells'][0]['throughput_per_second']
(J/'matched-rate.json').write_text(json.dumps({'framework_cpu_round_rps':rate,'binding_target_rate':round(rate)},indent=2)+'\n')
for mode in ('light','cpu'):
 env=dict(os.environ,PROBE_MODE=mode,I3560B_CHILD='1',RUN_LABEL='-matched',IMPLEMENTATIONS='zlink-cpp',TARGET_RATE=str(round(rate)))
 subprocess.run(['python3',str(J/'measure.py')],env=env,check=True)
PY
