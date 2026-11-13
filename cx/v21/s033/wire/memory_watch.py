import pathlib,os,signal,time,json
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/wire');paused=set();minimum=10**12
try:
 while not (job/'memory-watch-stop').exists():
  available=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
  minimum=min(minimum,available)
  if available<17*1024*1024:
   for p in pathlib.Path('/proc').iterdir():
    if not p.name.isdigit():continue
    try:
     if (p/'comm').read_text().strip()!='cc1plus':continue
     if '/root/worktree/zlink-3867w/framework/bench/grpc/cpp/build' not in os.readlink(p/'cwd'):continue
     pid=int(p.name)
     if pid not in paused:
      os.kill(pid,signal.SIGSTOP);paused.add(pid)
      print('paused bench compiler',pid,'available_kb',available,flush=True)
    except (FileNotFoundError,ProcessLookupError,PermissionError):pass
  if available>20*1024*1024:
   for pid in list(paused):
    try:os.kill(pid,signal.SIGCONT)
    except ProcessLookupError:pass
    paused.remove(pid);print('resumed bench compiler',pid,flush=True)
  (job/'memory-min.json').write_text(json.dumps({'minimum_kb':minimum,'last_kb':available,'paused':sorted(paused)}))
  time.sleep(1)
finally:
 for pid in paused:
  try:os.kill(pid,signal.SIGCONT)
  except ProcessLookupError:pass
