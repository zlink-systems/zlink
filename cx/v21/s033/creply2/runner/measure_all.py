from pathlib import Path
import subprocess,json,time
P=Path('/root/jobs/fx/cx/v21/s033/creply2')
(P/'measurement-entered.json').write_text(json.dumps({'time':time.time()}))
for pattern,size in [('request-backpressure',1024),('request-backpressure',4096),('request-serial',1024)]:
 for index in range(1,6):
  # Rotate order to avoid assigning one variant every first run in a triplet.
  variants=('main','direct','binding') if index%2 else ('binding','direct','main')
  for variant in variants:
   case=f'{variant}-unpaced-{pattern}-{size}-{index}'
   cmd=['python3',str(P/'runner/run.py'),case,'--variant',variant,'--pattern',pattern,'--payload',str(size),'--warmup','10','--active','5']
   print('RUN',case,flush=True);subprocess.run(cmd,check=True)
rate=json.loads((P/'rate.json').read_text())['rate']
for index in (1,2):
 for variant in ('main','direct','binding'):
  cmd=['python3',str(P/'runner/run.py'),f'{variant}-perf-{index}','--variant',variant,'--rate',str(rate),'--collect','perf','--active','40']
  print('RUN',cmd,flush=True);subprocess.run(cmd,check=True)
for variant in ('main','direct'):
 cmd=['python3',str(P/'runner/run.py'),f'{variant}-mutex','--variant',variant,'--rate',str(rate),'--active','30','--mutex-stat']
 print('RUN',cmd,flush=True);subprocess.run(cmd,check=True)
for f in ('align_ss.py','summarize.py','table.py'):
 subprocess.run(['python3',str(P/'tools'/f)],check=True)
(P/'measurement-completed.json').write_text(json.dumps({'time':time.time()}))
