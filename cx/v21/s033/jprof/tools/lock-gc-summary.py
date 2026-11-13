import json, subprocess
from pathlib import Path
root=Path('/root/jobs/fx/cx/v21/s033/jprof')
for pat in ['send-saturation','request-backpressure','request-serial']:
 print('\n##',pat)
 for impl in ['zlink-framework-java','zlink-java']:
  msgs=json.load((root/'out'/f'jprof-{impl}-{pat}-profile'/'run1'/f'{impl}-{pat}-1024'/'results.json').open())['cells'][0]['completed']
  print('\n',impl,'msgs',msgs)
  for role in ['source','target']:
   jfr=root/'profiles'/f'jprof-{impl}-{pat}-profile-{role}.jfr'
   events=json.loads(subprocess.check_output(['jfr','print','--json','--events','jdk.JavaMonitorEnter,jdk.GarbageCollection,jdk.YoungGarbageCollection,jdk.OldGarbageCollection',str(jfr)]))['recording']['events']
   locks={}; gcs=0
   for e in events:
    v=e['values']
    if 'JavaMonitorEnter' in e['type']:
     cls=v.get('monitorClass',{}).get('name','?').replace('/','.')
     frames=v.get('stackTrace',{}).get('frames',[])
     f=frames[0]['method'] if frames else {}
     typ=f.get('type',{}).get('name','?').replace('/','.')
     key=f'{cls} @ {typ}.{f.get("name","?")}'
     locks[key]=locks.get(key,0)+1
    if 'GarbageCollection' in e['type'] or 'YoungGarbageCollection' in e['type'] or 'OldGarbageCollection' in e['type']:
     gcs+=1
   print(' ',role,'GC',gcs,'per 1M',round(gcs*1e6/max(1,msgs),3),'monitor enter starts',sum(locks.values()),'per 1M',round(sum(locks.values())*1e6/max(1,msgs),2))
   for k,n in sorted(locks.items(),key=lambda x:-x[1])[:8]: print('   ',n,k)
