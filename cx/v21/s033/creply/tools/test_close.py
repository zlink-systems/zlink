from pathlib import Path
import subprocess,signal,os,time,json
P=Path('/root/jobs/fx/cx/v21/s033/creply');rows={}
for mode in ('main','direct'):
 with (P/f'close-{mode}.log').open('w') as log:
  child=subprocess.Popen([str(P/f'bin-{mode}/creply_close_probe'),mode],stdout=log,stderr=log,start_new_session=True)
  begin=time.monotonic()
  while child.poll() is None:
   if time.monotonic()-begin>12:
    os.killpg(child.pid,signal.SIGTERM);child.wait();raise RuntimeError('close fixture timed out: '+mode)
   time.sleep(.05)
  rows[mode]={'exit':child.returncode,'log':(P/f'close-{mode}.log').read_text()}
  (P/'close-validation.json').write_text(json.dumps(rows,indent=2));assert child.returncode==0,rows[mode]
print(json.dumps(rows,indent=2))
