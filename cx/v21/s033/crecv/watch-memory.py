from pathlib import Path
import json,os,signal,time
J=Path(__file__).parent
state={'minimum_kib':10**12,'interrupts':[],'samples':0}
roots=[str(J/'build'),'/root/worktree/zlink-3867r/framework/languages/cpp/build/linux-ninja-release']
stopping=False
def stop(*args):
 global stopping
 stopping=True
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
try:
 while not stopping and not (J/'monitor.stop').exists():
  mem=int(next(l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
  state['minimum_kib']=min(state['minimum_kib'],mem);state['samples']+=1
  if mem<14*1024*1024:
   candidates=[]
   for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():continue
    try:
     if (proc/'comm').read_text().strip()!='ninja':continue
     cwd=str((proc/'cwd').resolve())
     if cwd in roots:candidates.append((roots.index(cwd),int(proc.name)))
    except (OSError,ProcessLookupError):pass
   if candidates:
    _,pid=min(candidates);os.kill(pid,signal.SIGINT);state['interrupts'].append({'time':time.time(),'pid':pid,'memavailable_kib':mem})
    (J/'memory.json').write_text(json.dumps(state,indent=2));time.sleep(1)
  if state['samples']%20==0:(J/'memory.json').write_text(json.dumps(state,indent=2))
  time.sleep(.25)
finally:(J/'memory.json').write_text(json.dumps(state,indent=2))
