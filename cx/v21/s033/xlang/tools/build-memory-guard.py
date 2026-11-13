import json,os,pathlib,signal,time
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');W='/root/worktree/zlink-xlang';low=10**12;events=[]
if (P/'build-memory.json').exists():
 old=json.loads((P/'build-memory.json').read_text());low=old['min_mem_kb'];events=old['stopped']
try:
 while not (P/'BUILD_DONE').exists():
  n=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')));low=min(low,n)
  if n<15728640:
   candidates=[]
   for p in pathlib.Path('/proc').iterdir():
    if not p.name.isdigit():continue
    try:
     cwd=os.readlink(p/'cwd')
     if not cwd.startswith(W+'/'):continue
     cmd=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace');comm=(p/'comm').read_text().strip()
     if comm=='cc1plus' or 'GradleDaemon' in cmd or ('csc.dll' in cmd) or ('typescript' in cmd and '/tsc' in cmd):
      rss=int((p/'statm').read_text().split()[1])*4;candidates.append((rss,int(p.name),comm))
    except (OSError,ValueError):pass
   if candidates:
    rss,pid,comm=max(candidates)
    os.kill(pid,signal.SIGTERM);events.append({'time':time.time(),'mem_kb':n,'pid':pid,'comm':comm,'rss_kb':rss});print('stopped own compiler',events[-1],flush=True);time.sleep(2)
  (P/'build-memory.json').write_text(json.dumps({'min_mem_kb':low,'stopped':events,'last':time.time()},indent=2));time.sleep(.2)
finally:
 (P/'build-memory.json').write_text(json.dumps({'min_mem_kb':low,'stopped':events,'end':time.time()},indent=2))
