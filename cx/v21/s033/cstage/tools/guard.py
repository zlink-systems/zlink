import subprocess,sys,time,signal,json,pathlib,os
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cstage')
def mem(): return int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
assert mem()>=12582912
p=subprocess.Popen(sys.argv[1:],start_new_session=True)
def stop(*a):
 os.killpg(p.pid,signal.SIGTERM)
 p.wait()
 raise SystemExit(143)
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
with (job/'memory.jsonl').open('a') as f:
 while p.poll() is None:
  m=mem();f.write(json.dumps({'time':time.time(),'MemAvailable_kB':m,'pid':p.pid})+'\n');f.flush()
  if m<13631488: stop()
  time.sleep(1)
sys.exit(p.returncode)
