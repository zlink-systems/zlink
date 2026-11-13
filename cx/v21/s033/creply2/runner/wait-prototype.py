from pathlib import Path
import os,time
p=Path('/root/jobs/fx/cx/v21/s033/creply2')
while not (p/'prototype-built.json').exists():time.sleep(.2)
os.execvp('python3',['python3',str(p/'runner/post-prototype.py')])
