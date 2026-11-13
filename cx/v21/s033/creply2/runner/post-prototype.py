from pathlib import Path
import subprocess,json
P=Path('/root/jobs/fx/cx/v21/s033/creply2')
assert (P/'prototype-built.json').exists()
subprocess.run(['/root/jobs/fx/cx/measure-shared.sh','python3',str(P/'tools/test_close.py')],check=True)
subprocess.run(['flock','-o','/root/jobs/fx/cx/measure.lock','python3',str(P/'runner/prototype-batch.py')],check=True)
