#!/usr/bin/env python3
import datetime, json
from pathlib import Path
LANG='dotnet'
IMPL='zlink-dotnet'
JOB=Path('/root/jobs/fx/cx/v21/s033/binding-rebase')
RAW=JOB/'raw'/LANG
DATE=datetime.datetime.now(datetime.timezone.utc).date().isoformat()
DAILY=Path('/root/jobs/fx/cx/v21/s033/binding-daily')/f'{LANG}-{DATE}.json'
old={}
if DAILY.exists(): raise SystemExit(f'refusing to overwrite existing daily reference: {DAILY}')
cells={}
for pattern in ('send-saturation','request-serial','request-backpressure'):
 for size in (1024,4096):
  key=f'{pattern}-{size}'
  raw=RAW/key/'counted'/'run1'/f'{IMPL}-{pattern}-{size}'/'results.json'
  sched=raw.with_name('scheduling.txt')
  r=json.loads(raw.read_text())['cells'][0]; txt=sched.read_text()
  assert r['errors']==r['server_errors']==r['target_stats']['errors']==0
  assert r['submitted']==r['completed']==r['target_stats']['received']
  assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in txt for cpu in (0,8) for state in (2,3))
  assert 'scheduling_invalid=' not in txt
  cells[key]={'msg_s':r['throughput_per_second'],'raw':str(raw)}
value={'language':LANG,'utc_date':DATE,'conditions':'C2·C3 off, SCHED_OTHER, CPU split 0-5/8-13 + housekeeping 6-7,14-15, warm run per cell, guard v7','measured_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'cells':cells}
DAILY.parent.mkdir(parents=True,exist_ok=True)
with DAILY.open('x') as f: json.dump(value,f,ensure_ascii=False,indent=2); f.write('\n')
report=JOB/'report.md'
with report.open('a') as f:
 for key,item in cells.items():
  old_value=old.get(key)
  old_text='—' if old_value is None else str(old_value)
  f.write(f"  | {LANG} | {key} | {item['msg_s']} | {old_text} | [raw]({item['raw']}) |\n")
 f.write('\n')
print(DAILY)
