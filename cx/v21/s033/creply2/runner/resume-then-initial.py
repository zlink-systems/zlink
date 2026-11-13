from pathlib import Path
import os,time
p=Path('/root/jobs/fx/cx/v21/s033/creply2')
while not (p/'resume-entered.json').exists():time.sleep(.2)
os.execvp('flock',['flock','-o','/root/jobs/fx/cx/measure.lock','python3',str(p/'runner/initial.py')])
