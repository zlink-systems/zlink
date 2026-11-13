import collections,datetime,json,pathlib,subprocess
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');JFR='/usr/lib/jvm/java-25-openjdk-amd64/bin/jfr';COUNTER=P/'tools/eventpipe/bin/Release/net8.0/counter.dll'
def load(p):return json.loads(p.read_text())
def stamp(s):return datetime.datetime.fromisoformat(s.replace('Z','+00:00')).timestamp()
rows=[]
for p in sorted((P/'runs/alloc').glob('*/meta.json')):
 m=load(p)
 if not m.get('ok'):continue
 lang=m['job']['lang'];d=p.parent;a=load(d/'before.json');b=load(d/'after.json');n=b['stats']['source']['completed']-a['stats']['source']['completed'];out={'job':m['job'],'messages':n,'roles':{}}
 if lang=='java':
  for side in ['source','target']:
   dest=d/(side+'.jfr-alloc.json')
   if not dest.exists():
    with dest.open('w') as h:subprocess.run([JFR,'print','--events','jdk.ObjectAllocationInNewTLAB,jdk.ObjectAllocationOutsideTLAB','--json',str(d/(side+'.jfr'))],stdout=h,check=True)
   counts=collections.Counter()
   for ev in load(dest)['recording']['events']:
    v=ev['values'];t=stamp(v['startTime'])
    if not a['realtime']<=t<=b['realtime']:continue
    if ev['type']=='jdk.ObjectAllocationInNewTLAB':counts['tlab_bytes']+=v['tlabSize'];counts['refills']+=1
    else:counts['outside_bytes']+=v['allocationSize'];counts['outside_objects']+=1
   out['roles'][side]={**counts,'tlab_capacity_bytes_per_msg':(counts['tlab_bytes']+counts['outside_bytes'])/n,'tlab_events_per_msg':(counts['refills']+counts['outside_objects'])/n}
 if lang=='dotnet':
  for side in ['source','target']:
   dest=d/(side+'.allocation-ticks.json')
   if not dest.exists():subprocess.run(['dotnet',str(COUNTER),'read',str(d/(side+'.nettrace')),str(dest)],check=True)
   counts=collections.Counter()
   for v in load(dest):
    t=stamp(v['timestamp'])
    if not a['realtime']<=t<=b['realtime']:continue
    counts['ticks']+=1;counts['tick_bytes']+=v['bytes']
    if v['objectSize']>0:counts['valid_ticks']+=1;counts['estimated_objects']+=v['bytes']/v['objectSize']
   out['roles'][side]={**counts,'tick_bytes_per_msg':counts['tick_bytes']/n,'count_per_msg_estimate':counts['estimated_objects']/n if counts['valid_ticks'] else None}
 if out['roles']:
  (d/'allocation-summary.json').write_text(json.dumps(out,indent=2));rows.append(out);print('allocation decoded',d.name,flush=True)
(P/'allocation-summary.json').write_text(json.dumps(rows,indent=2))
