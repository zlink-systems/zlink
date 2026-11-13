from pathlib import Path
import subprocess,json
P=Path('/root/jobs/fx/cx/v21/s033/creply')
subprocess.run(['python3',str(P/'runner/unpaced.py')],check=True)
rate=json.loads((P/'rate.json').read_text())['rate']
for i in (1,2):
 subprocess.run(['python3',str(P/'runner/run.py'),f'direct-perf-{i}','--variant','direct','--rate',str(rate),'--collect','perf','--active','40'],check=True)
for f in ('summarize.py','align_ss.py','table.py'):
 subprocess.run(['python3',str(P/'tools'/f)],check=True)
