import subprocess, pathlib, time, os, signal
out=pathlib.Path('/root/jobs/fx/cx/v21/s033/cadvice')
def available():
 return int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
assert available()>=12582912
p=subprocess.Popen(['bash',str(out/'run_local.sh')],start_new_session=True)
with (out/'memory.tsv').open('w') as f:
 while p.poll() is None:
  m=available(); f.write(f'{time.time()}\t{m}\n'); f.flush()
  if m<12582912:
   os.killpg(p.pid,signal.SIGTERM); p.wait(); raise RuntimeError('MemAvailable floor reached')
  time.sleep(1)
raise SystemExit(p.returncode)
