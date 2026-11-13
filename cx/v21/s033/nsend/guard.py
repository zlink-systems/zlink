import os,pathlib,time,threading,signal,json,atexit
P=pathlib.Path('/proc');HZ=os.sysconf('SC_CLK_TCK')
def scan():
 result={}
 for d in P.iterdir():
  if not d.name.isdigit():continue
  try:
   s=(d/'stat').read_text();end=s.rindex(')');f=s[end+2:].split();result[int(d.name)]={'ppid':int(f[1]),'ticks':int(f[11])+int(f[12]),'start':int(f[19]),'comm':s[s.index('(')+1:end]}
  except (FileNotFoundError,ProcessLookupError,PermissionError):pass
 return result
def relatives(rows):
 own={os.getpid()};p=os.getpid()
 while p in rows and rows[p]['ppid']>0:
  p=rows[p]['ppid'];own.add(p)
 descendants={os.getpid()}
 while True:
  old=len(descendants);descendants.update(p for p,r in rows.items() if r['ppid'] in descendants)
  if len(descendants)==old:break
 return own|descendants,descendants
class Guard:
 def __init__(self,out):
  self.out=pathlib.Path(out);self.stop_event=threading.Event();self.samples=0;self.max_external=0;self.min_memory=10**20;self.detected=None
  self.thread=threading.Thread(target=self.watch,name='measurement-condition-guard',daemon=True);self.thread.start();atexit.register(self.close)
 def watch(self):
  before=scan();bt=time.monotonic();seen={}
  while not self.stop_event.wait(1):
   after=scan();at=time.monotonic();own,desc=relatives(after);hot=[]
   self.samples+=1
   memory=int(next(l.split()[1] for l in (P/'meminfo').read_text().splitlines() if l.startswith('MemAvailable:')));self.min_memory=min(self.min_memory,memory)
   for p,r in after.items():
    b=before.get(p)
    if p in own or r['ppid']==2 or b is None or b['start']!=r['start']:continue
    cores=max(0,r['ticks']-b['ticks'])/HZ/(at-bt);self.max_external=max(self.max_external,cores)
    seen[p]=seen.get(p,0)+1 if cores>.25 else 0
    if cores>.5 or seen[p]>=2:
     try:cwd=os.readlink(P/str(p)/'cwd')
     except OSError:cwd='unavailable'
     hot.append({'pid':p,'ppid':r['ppid'],'comm':r['comm'],'cores':cores,'cwd':cwd})
   if hot or memory<12582912:
    self.detected={'time':time.strftime('%FT%T%z'),'foreign_cpu':hot,'memavailable_kb':memory,'rule':'external >0.5 CPU in one second, or >0.25 CPU for two consecutive seconds; MemAvailable >=12 GiB'}
    (self.out/'contamination.detected.json').write_text(json.dumps(self.detected,indent=2)+'\n');print('MEASUREMENT CONDITION VIOLATION',json.dumps(self.detected),flush=True)
    for p in sorted(desc-{os.getpid()},reverse=True):
     try:os.kill(p,signal.SIGINT)
     except ProcessLookupError:pass
    os.kill(os.getpid(),signal.SIGINT);return
   before=after;bt=at
 def close(self):
  self.stop_event.set();self.thread.join(timeout=2)
  (self.out/'guard-summary.json').write_text(json.dumps({'samples':self.samples,'max_external_cpu':self.max_external,'minimum_memavailable_kb':self.min_memory,'detected':self.detected},indent=2)+'\n')
