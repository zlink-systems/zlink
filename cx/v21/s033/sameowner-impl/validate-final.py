from pathlib import Path
import subprocess, os, sys, resource, shutil
job=Path('/root/jobs/fx/cx/v21/s033/sameowner-impl')
root=Path('/root/worktree/zlink-v21-s033/sameowner-impl')
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
resource.setrlimit(resource.RLIMIT_AS,(26843545600,26843545600))
env=os.environ.copy()
env['LD_LIBRARY_PATH']='/root/.cache/zlink/core-release/1.24.0/linux-x64/lib'+(':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
with (job/'repeat-final-driver.log').open('w') as log:
    result=subprocess.run(['python3',str(job/'run-focused.py'),'repeat-final'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
(job/'repeat-final.exit').write_text(str(result.returncode)+'\n')
if result.returncode:
    sys.exit(1)
env['ZLINK_CPP_SAME_OWNER_FLOW_LOG']=str(job/'full-suite.flow')
with (job/'full-suite.log').open('w') as log:
    result=subprocess.run(['ctest','--test-dir',str(root/'build-sameowner'),'-L','^(unit|contract)$','--output-on-failure','-j3'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
(job/'full-suite.exit').write_text(str(result.returncode)+'\n')
for name in ['LastTest.log','LastTestsFailed.log','CTestCostData.txt']:
    source=root/'build-sameowner/Testing/Temporary'/name
    if source.exists():
        shutil.copy2(source,job/('full-suite-'+name))
if result.returncode:
    sys.exit(1)
subprocess.run(['python3',str(job/'finalize-report.py')],cwd=root,env=env,check=True)
sys.exit(0)
