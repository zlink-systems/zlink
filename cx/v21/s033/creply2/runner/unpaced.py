from pathlib import Path
import subprocess,json,statistics
P=Path('/root/jobs/fx/cx/v21/s033/creply2')
for pattern,size in [('request-backpressure',1024),('request-backpressure',4096),('request-serial',1024)]:
 for index in range(1,6):
  for variant in ('main','direct','binding'):
   case=f'{variant}-unpaced-{pattern}-{size}-{index}'
   cmd=['python3',str(P/'runner/run.py'),case,'--variant',variant,'--pattern',pattern,'--payload',str(size),'--warmup','10','--active','5']
   print('RUN',case,flush=True);subprocess.run(cmd,check=True)
subprocess.run(['python3',str(P/'tools/table.py')],check=True)
