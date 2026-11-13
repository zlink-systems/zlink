#!/usr/bin/env python3
"""External /proc observer; does not alter the benchmark unless --placement is used."""
import argparse,collections,hashlib,json,os,pathlib,subprocess,time
from read_state import Reader
from thread_pause import pause
p=argparse.ArgumentParser();p.add_argument('--runs',type=int,default=20);p.add_argument('--label',default='natural');p.add_argument('--placement',choices=['none','separate','siblings','onecpu'],default='none');p.add_argument('--sample-cap');p.add_argument('--duration',default='5');p.add_argument('--profile',default='balanced');p.add_argument('--queue-cycle',action='store_true');p.add_argument('--aslr-off',action='store_true');p.add_argument('--pad',type=int);a=p.parse_args()
j=pathlib.Path(__file__).parent;binary=j/'bin/perf_router_router'
env=os.environ.copy();env.update(PERF_SINGLE_DURATION_SECONDS=a.duration,PERF_PART_COUNT='2',PERF_CTX_AUTO_HWM_PROFILE=a.profile,PERF_SINGLE_SNDTIMEO_MS='200',PERF_SINGLE_RCVTIMEO_MS='200',PERF_PRINT_AUTO_HWM_DETAIL='1')
if a.sample_cap is not None:env['PERF_SINGLE_LATENCY_SAMPLE_CAP']=a.sample_cap
if a.pad is not None:env['DIAG_STACK_PAD']='p'*a.pad

def snap(pid,t0):
 out={}
 try: tasks=list(pathlib.Path(f'/proc/{pid}/task').iterdir())
 except FileNotFoundError:return out
 for d in tasks:
  try:
   stat=(d/'stat').read_text();q=stat[stat.rfind(')')+2:].split();status=dict(x.split(':',1) for x in (d/'status').read_text().splitlines() if ':' in x)
   out[d.name]={'comm':(d/'comm').read_text().strip(),'cpu':int(q[36]),'utime_ticks':int(q[11]),'stime_ticks':int(q[12]),'start_ticks':int(q[19]),'schedstat':list(map(int,(d/'schedstat').read_text().split())),'voluntary':int(status['voluntary_ctxt_switches']),'involuntary':int(status['nonvoluntary_ctxt_switches']),'affinity':status['Cpus_allowed_list'].strip(),'wchan':(d/'wchan').read_text().strip()}
  except (FileNotFoundError,ProcessLookupError):pass
 return out

for n in range(1,a.runs+1):
 stem=j/'raw'/f'{a.label}-{n:02d}';assert not stem.with_suffix('.log').exists()
 t0=time.monotonic();out=stem.with_suffix('.log').open('w');proc=subprocess.Popen((['setarch','x86_64','-R'] if a.aslr_off else [])+['taskset','-c','8-15',str(binary),'c','inproc','1024'],stdout=out,stderr=subprocess.STDOUT,env=env)
 meta={'argv':proc.args,'env':{k:v for k,v in env.items() if k.startswith('PERF_')},'pid':proc.pid,'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'placement':a.placement,'aslr_off':a.aslr_off,'pad_length':a.pad,'interventions':[],'roles':{},'affinity_actions':[]}
 samples=[];forced=False;reader=None
 while proc.poll() is None:
  ts=time.monotonic()-t0;s=snap(proc.pid,t0)
  # Core background threads have ZLINK names. Two ordinary children are
  # constructed in receiver-then-sender order in perf_single_one_way.hpp.
  app=sorted((int(t) for t,v in s.items() if int(t)!=proc.pid and v['comm']=='perf_router_rou'),key=int)
  # Linux comm is 15 bytes: accept the full truncation produced by exec.
  if len(app)!=2:
   app=sorted(int(t) for t,v in s.items() if int(t)!=proc.pid and not v['comm'].startswith('ZLINK') and 'reaper' not in v['comm'].lower() and '/IO/' not in v['comm'] and v['comm']==binary.name[:15])
  if len(app)==2:
   meta['roles']={'receiver':app[0],'sender':app[1]}
   if not forced and a.placement!='none':
    pair={'separate':(8,10),'siblings':(8,9),'onecpu':(8,8)}[a.placement]
    for role,cpu in zip(('receiver','sender'),pair):
     tid=meta['roles'][role];os.sched_setaffinity(tid,{cpu});meta['affinity_actions'].append({'t':ts,'role':role,'tid':tid,'cpu':cpu})
    forced=True
  memory=None
  try:
   if reader is None:reader=Reader(proc.pid,int(a.sample_cap) if a.sample_cap is not None else 1000000)
   memory=reader.sample()
   if a.queue_cycle and len(meta['roles'])==2:
    index=len(meta['interventions'])
    if index<2 and ts>=(1.0 if index==0 else 3.0):
     role='receiver' if index==0 else 'sender';event={'role':role,'t_before':time.monotonic()-t0,'before':reader.sample()}
     event.update(pause(meta['roles'][role],.020,reader.sample));event['t_after']=time.monotonic()-t0;event['after']=reader.sample();meta['interventions'].append(event)
  except (OSError,StopIteration,ProcessLookupError):pass
  freq={}
  for cpu in range(8,16):
   f=pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq')
   try:freq[cpu]=int(f.read_text())
   except (OSError,ValueError):pass
  cpu_stat={line.split()[0]:list(map(int,line.split()[1:])) for line in pathlib.Path('/proc/stat').read_text().splitlines() if line.split() and line.split()[0] in {f'cpu{i}' for i in range(8,16)}}
  samples.append({'t':ts,'threads':s,'memory':memory,'frequency_khz':freq,'system_cpu_ticks':cpu_stat});time.sleep(.02 if ts<.2 else .1)
 rc=proc.wait();out.close()
 if reader:reader.close()
 meta['returncode']=rc;meta['elapsed']=time.monotonic()-t0
 stem.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n');stem.with_suffix('.proc.jsonl').write_text(''.join(json.dumps(s)+'\n' for s in samples))
 vals={}
 for line in stem.with_suffix('.log').read_text().splitlines():
  if line.startswith('RESULT,'):
   q=line.split(',');vals[q[-2]]=float(q[-1])
 # TID union, retaining threads absent at either end; snapshots are raw.
 window=[x for x in samples if .2<=x['t']<=4.8 and all(str(t) in x['threads'] for t in meta['roles'].values())]
 before=window[0] if window else samples[0];after=window[-1] if window else samples[-1]
 first=before['threads'];last=after['threads']
 stem.with_suffix('.before.json').write_text(json.dumps(before,indent=2)+'\n')
 stem.with_suffix('.after.json').write_text(json.dumps(after,indent=2)+'\n')
 delta={t:{'comm':(last.get(t) or first.get(t))['comm'],'runtime_ns':max(0,last.get(t,{}).get('schedstat',[0])[0]-first.get(t,{}).get('schedstat',[0])[0]),'voluntary':max(0,last.get(t,{}).get('voluntary',0)-first.get(t,{}).get('voluntary',0)),'involuntary':max(0,last.get(t,{}).get('involuntary',0)-first.get(t,{}).get('involuntary',0))} for t in first.keys()|last.keys()}
 hist={role:dict(collections.Counter(s['threads'][str(tid)]['cpu'] for s in samples if str(tid) in s['threads'])) for role,tid in meta['roles'].items()}
 row={'label':a.label,'run':n,'values':vals,'roles':meta['roles'],'cpu_histogram':hist,'deltas_tid_union':delta,'window_before_s':before['t'],'window_after_s':after['t'],'only_before_tids':list(first.keys()-last.keys()),'only_after_tids':list(last.keys()-first.keys()),'rc':rc,'raw':str(stem)}
 with (j/'summary.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
 print(json.dumps({k:row[k] for k in ('label','run','values','roles','cpu_histogram','rc')}),flush=True)
 if rc:raise SystemExit(rc)
 if a.placement!='none' and not forced:raise SystemExit('No role threads recognized; placement NOT forced')
