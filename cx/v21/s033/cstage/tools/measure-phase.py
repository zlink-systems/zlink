from pathlib import Path
import subprocess
j=Path('/root/jobs/fx/cx/v21/s033/cstage')
subprocess.run(['python3',str(j/'tools/measure.py'),'--variants','staged','--runs','1','--warmup','1','--active','1','--tag','smoke'],check=True)
subprocess.run(['python3',str(j/'tools/measure-all.py')],check=True)
