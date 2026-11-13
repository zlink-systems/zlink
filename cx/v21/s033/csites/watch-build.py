import os,pathlib as P,signal,time
root='/root/worktree/zlink-3867h/framework/languages/cpp/build/linux-ninja-release'
while True:
 pids=[]
 for p in P.Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   if (p/'comm').read_text().strip()=='ninja' and os.readlink(p/'cwd')==root:pids.append(int(p.name))
  except (OSError,ProcessLookupError):pass
 if not pids:print('build finished',flush=True);break
 available=int(next(s.split()[1] for s in P.Path('/proc/meminfo').read_text().splitlines() if s.startswith('MemAvailable:')))
 if available<13*1024*1024:
  for pid in pids:os.kill(pid,signal.SIGINT)
  print('preempted before 12 GiB floor, MemAvailable KiB',available,flush=True);raise SystemExit(1)
 time.sleep(3)
