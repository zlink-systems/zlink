import pathlib, subprocess, sys, os, signal, time, json
p=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply2')
name=sys.argv[1];cmd=sys.argv[2:]
running_floor_gib = 8 if name.startswith('build-') else 12
if name == 'configure':cmd.append('-DCMAKE_EXE_LINKER_FLAGS=-Wl,--compress-debug-sections=zstd')
if name == 'build-focused':cmd.insert(cmd.index('-j1'),'creply_prototype_close_control')
with (p/'logs'/f'{name}.log').open('w') as log, (p/'logs'/f'{name}-memory.jsonl').open('w') as memlog:
 log.write('COMMAND '+repr(cmd)+'\n');log.write(f'Memory policy: start >=12 GiB, running floor {running_floor_gib} GiB\n');log.flush()
 available=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
 if available<12*1048576:
  log.write('Refused to start below 12 GiB MemAvailable: '+str(available)+' KiB\n');(p/'logs'/f'{name}.status.json').write_text(json.dumps({'returncode':75,'time':time.time()}));raise SystemExit(75)
 childenv=os.environ.copy();childenv['LD_LIBRARY_PATH']='/root/.cache/zlink/core/1.23.0/linux-x64/lib:'+childenv.get('LD_LIBRARY_PATH','')
 child=subprocess.Popen(cmd,stdout=log,stderr=log,start_new_session=True,env=childenv)
 def stop(*args):
  if child.poll() is None:os.killpg(child.pid,signal.SIGTERM)
  child.wait()
  if args:raise SystemExit(143)
 signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
 try:
  while child.poll() is None:
   kb=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
   memlog.write(json.dumps({'time':time.time(),'available_kb':kb})+'\n');memlog.flush()
   if kb<running_floor_gib*1048576:stop();raise RuntimeError(f'{running_floor_gib} GiB running memory guard')
   time.sleep(.25)
  print(name,'exit',child.returncode,flush=True)
  sys.exit(child.returncode)
 finally:
  stop()
  (p/'logs'/f'{name}.status.json').write_text(json.dumps({'returncode':child.returncode,'time':time.time()}))
