import pathlib,time,os,signal,datetime
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdesign')
own_cwds={'/root/jobs/fx/cx/v21/s033/cdesign/build'}
paused=set()
with (job/'governor.log').open('a',buffering=1) as log:
 def send(pid,sig):
  try:
   os.kill(pid,sig)
   log.write(f'{datetime.datetime.now().isoformat()} {pid} {sig.name}\n')
  except ProcessLookupError: pass
 try:
  while not (job/'memory-stop').exists():
   mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
   compilers=[]
   for p in pathlib.Path('/proc').glob('[0-9]*'):
    try:
     if (p/'comm').read_text().strip()!='cc1plus' or os.readlink(p/'cwd') not in own_cwds: continue
     rss=int(next(x.split()[1] for x in (p/'status').read_text().splitlines() if x.startswith('VmRSS:')))
     compilers.append((int(p.name),rss))
    except (FileNotFoundError,ProcessLookupError,PermissionError,StopIteration): pass
   alive={pid for pid,_ in compilers};paused.intersection_update(alive)
   if mem<18874368 and len(compilers)>1:
    # Let the largest compiler finish and release its memory first.
    chosen=max(compilers,key=lambda x:x[1])[0]
    for pid,_ in compilers:
     if pid!=chosen and pid not in paused: send(pid,signal.SIGSTOP);paused.add(pid)
    if chosen in paused: send(chosen,signal.SIGCONT);paused.remove(chosen)
   elif mem>=20971520 or not (alive-paused):
    for pid in list(paused):send(pid,signal.SIGCONT)
    paused.clear()
   time.sleep(.5)
 finally:
  for pid in paused:send(pid,signal.SIGCONT)
