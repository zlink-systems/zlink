#!/usr/bin/env python3
import json, pathlib, subprocess, time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/r4190m')
aa=pathlib.Path('/root/jobs/fx/cx/v21/s033/f4162')
def conditions(path):
    parts=[subprocess.check_output(['date','-u','+%FT%TZ'],text=True).strip(),subprocess.check_output(['uptime'],text=True).strip(),subprocess.check_output(['free','-b'],text=True)]
    for cpu in range(16):
        base=pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq')
        parts += [f'cpu{cpu}.governor={ (base/"scaling_governor").read_text().strip() }',f'cpu{cpu}.scaling_cur_freq_khz={ (base/"scaling_cur_freq").read_text().strip() }']
    path.write_text('\n'.join(parts)+'\n')
raw=job/'raw'
conditions(raw/'aa-clean-conditions-before.txt')
with (raw/'aa-clean-driver.log').open('w') as log:
    subprocess.run(['python3','observe.py','--runs','20','--label','aa-clean','--placement','separate'],cwd=aa,stdout=log,stderr=subprocess.STDOUT,check=True)
conditions(raw/'aa-clean-conditions-after.txt')
rows=[json.loads(line) for line in (aa/'summary.jsonl').read_text().splitlines() if json.loads(line).get('label')=='aa-clean']
if len(rows)!=20: raise SystemExit(f'expected 20 aa-clean rows, found {len(rows)}')
stage=job/'tools'
(stage/'summary.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
(stage/'raw').mkdir(exist_ok=True)
subprocess.run(['cp',str(aa/'analyze-aa.py'),str(stage/'analyze-aa.py')],check=True)
with (raw/'analyze-aa.log').open('w') as log:
    subprocess.run(['python3',str(stage/'analyze-aa.py')],cwd=stage,stdout=log,stderr=subprocess.STDOUT,check=True)
print((stage/'raw/aa-analysis.json').read_text())
