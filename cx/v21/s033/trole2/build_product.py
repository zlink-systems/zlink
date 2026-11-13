from pathlib import Path
import subprocess,os
job=Path('/root/jobs/fx/cx/v21/s033/trole2')
root=Path('/root/worktree/zlink-v21-s033/trole')
# Called only through cpp-build-slot.sh. This process compiles and never tests.
with (job/'logs/build-full-final.log').open('w') as log:
    subprocess.run(['cmake','--build',str(root/'build-trole'),'-j3','--','-k','0'],
        cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
subprocess.run(['python3',str(job/'negative_controls.py')],cwd=root,
    env=os.environ|{'ZLINK_HANDOFF_BUILD_SLOT':'1'},check=True)
