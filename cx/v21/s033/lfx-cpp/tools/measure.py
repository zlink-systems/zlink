#!/usr/bin/env python3
"""Execute a saved plan inside one exclusive hold; aggregate outside it."""
import datetime,json,os,subprocess,time
from pathlib import Path
JOB=Path('/root/jobs/fx/cx/v21/s033/lfx-cpp')
RAW=JOB/'raw'

def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def write(path,value):
 path.parent.mkdir(parents=True,exist_ok=True)
 path.write_text(json.dumps(value,indent=2)+'\n')
def result(variant,pattern,size,run,profile=False):
 impl='zlink-cpp' if variant=='binding' else 'zlink-framework-cpp'
 return RAW/('profiles' if profile else 'matrix')/variant/f'{pattern}-{size}'/f'run{run}'/f'{impl}-{pattern}-{size}'/'results.json'
def run_variant(variant,pattern,size,run,profile=False):
 deadline=int(os.environ['MEASURE_DEADLINE_EPOCH'])
 if time.time()>=deadline: raise TimeoutError('three-hour task cap reached')
 impl='zlink-cpp' if variant=='binding' else 'zlink-framework-cpp'
 base=result(variant,pattern,size,run,profile).parents[2]
 env=dict(os.environ,ZLINK_CPP_LADDER_LEVEL='1' if variant=='L1' else '0',
  IMPLEMENTATIONS=impl,PATTERNS=pattern,PAYLOAD_SIZES=str(size),OUTPUT_DIR=str(base),
  RUN_START=str(run),RUNS=str(run),RUN_STAMP=f'm1-{variant}-{pattern}-{size}')
 base.mkdir(parents=True,exist_ok=True)
 runner='profile-runner.sh' if profile else 'cpp-cell-runner.sh'
 log=base/f'run{run}-driver.log'
 with log.open('x') as output:
  subprocess.run([str(JOB/'tools'/runner)],env=env,stdout=output,stderr=subprocess.STDOUT,
   check=True,timeout=max(1,deadline-time.time()))
 return result(variant,pattern,size,run,profile)

def save_daily(paths):
 date=datetime.datetime.now(datetime.timezone.utc).date().isoformat()
 path=Path('/root/jobs/fx/cx/v21/s033/binding-daily')/f'cpp-{date}.json'
 if path.exists(): return
 cells={}
 for raw in paths:
  c=json.loads(raw.read_text())['cells'][0]
  cells[f'{c["pattern"]}-{c["payload_size"]}']={'msg_s':c['throughput_per_second'],'raw':str(raw)}
 assert len(cells)==6
 value={'language':'cpp','date_utc':date,'measured_utc':utc(),'cells':cells,
  'warmup_seconds':3,'active_seconds':5,'runs':1,
  'build_provenance':str(RAW/'build-provenance.json')}
 path.parent.mkdir(parents=True,exist_ok=True)
 with path.open('x') as f: json.dump(value,f,indent=2); f.write('\n')
 write(RAW/'binding-daily-reference.json',value)

def main():
 phase=os.environ['M1_PHASE']; plan=json.loads((RAW/f'{phase}-plan.json').read_text())
 completed=[]; bindings=[]
 if phase=='initial':
  daily=Path('/root/jobs/fx/cx/v21/s033/binding-daily')/f'cpp-{datetime.datetime.now(datetime.timezone.utc).date().isoformat()}.json'
  if daily.exists():
   write(RAW/'binding-daily-reference.json',json.loads(daily.read_text()))
   plan=[p for p in plan if p['variant']!='binding']
  elif not any(p['variant']=='binding' for p in plan):
   plan=[dict(variant='binding',pattern=p,size=s,run=1) for p in ('send-saturation','request-serial','request-backpressure') for s in (1024,4096)]+plan
 write(RAW/f'{phase}-start.json',{'utc':utc(),'pid':os.getpid(),'plan':plan})
 try:
  for item in plan:
   variant,pattern,size,run=item['variant'],item['pattern'],item['size'],item['run']
   path=run_variant(variant,pattern,size,run,phase=='profile')
   completed.append(str(path))
   if phase=='initial' and variant=='binding':
    bindings.append(path)
    if len(bindings)==6: save_daily(bindings)
 finally:
  write(RAW/f'{phase}-end.json',{'utc':utc(),'completed':completed})
if __name__=='__main__': main()
