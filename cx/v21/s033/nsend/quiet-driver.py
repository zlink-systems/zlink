import pathlib,os,time,subprocess,sys,json
J=pathlib.Path(__file__).parent
sys.path.insert(0,str(J));from guard import scan,relatives,HZ
before=scan();bt=time.monotonic();quiet=0
while quiet<5:
 time.sleep(1);after=scan();at=time.monotonic();own,_=relatives(after);hot=[]
 for pid,r in after.items():
  b=before.get(pid)
  if pid in own or r['ppid']==2 or b is None or b['start']!=r['start']:continue
  c=max(0,r['ticks']-b['ticks'])/HZ/(at-bt)
  if c>.25:hot.append({'pid':pid,'comm':r['comm'],'cores':c})
 m=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
 quiet=quiet+1 if not hot and m>=12582912 else 0
 print('QUIET',quiet,'MemAvailableKiB',m,'foreign',hot,flush=True);before=after;bt=at
os.execv(sys.executable,[sys.executable,str(J/'resume-prototype-runs4.py')])
