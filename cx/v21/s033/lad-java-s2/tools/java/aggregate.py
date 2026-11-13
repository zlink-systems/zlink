#!/usr/bin/env python3
import datetime, json
from pathlib import Path
LANG='java'
IMPL='zlink-java'
JOB=Path('/root/jobs/fx/cx/v21/s033/lad-java-s1')
RAW=JOB/'raw'/LANG
DATE='2026-11-13b'
DAILY=Path('/root/jobs/fx/cx/v21/s033/binding-daily')/f'{LANG}-{DATE}.json'
cells={}
for pattern in ('send-saturation','request-serial','request-backpressure'):
 for size in (1024,4096):
  key=f'{pattern}-{size}'
  raw=Path((RAW/key/'counted'/'valid-result.path').read_text().strip())
  sched=raw.with_name('scheduling.txt')
  r=json.loads(raw.read_text())['cells'][0]; txt=sched.read_text()
  assert r['errors']==r['server_errors']==r['target_stats']['errors']==0
  assert r['submitted']==r['completed']==r['target_stats']['received']
  assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in txt for cpu in (0,8) for state in (2,3))
  assert 'scheduling_invalid=' not in txt
  for label,expected in (('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')):
   vals=[line.rsplit('cpus=',1)[1] for line in txt.splitlines() if line.startswith(label+'_affinity ') and 'cpus=' in line]
   assert vals and all(value==expected for value in vals), f'missing or wrong affinity {label}: {sched}'
  cells[key]={'msg_s':r['throughput_per_second'],'raw':str(raw),'scheduling':str(sched)}
value={'language':LANG,'utc_date':'2026-11-13','conditions':'C2·C3 off, SCHED_OTHER, CPU split 0-5/8-13 + housekeeping 6-7,14-15, warm run per cell, guard v8','measured_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'cells':cells}
# explicit requested reference filename carries the suffix
DAILY.parent.mkdir(parents=True,exist_ok=True)
with DAILY.open('x') as f: json.dump(value,f,ensure_ascii=False,indent=2); f.write('\n')
report=JOB/'binding-results.md'
with report.open('w') as f:
 f.write('# Java binding 측정\n\n| 셀 | msg/s | raw 결과 | affinity·스케줄 기록 |\n|---|---:|---|---|\n')
 for key,item in cells.items():
  f.write(f"| {key} | {item['msg_s']} | [{item['raw']}]({item['raw']}) | [{item['scheduling']}]({item['scheduling']}) |\n")
 f.write('\n조건: C2·C3 off, SCHED_OTHER, source 0–5 / target 8–13, guard v8.\n')
print(DAILY)
