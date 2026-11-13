from pathlib import Path
import subprocess,os,sys
if os.environ.get('ZLINK_HANDOFF_TEST_SLOT')!='1':
    os.execve('/root/jobs/fx/cx/test-slot.sh', ['test-slot.sh','12-19','python3',__file__],
              os.environ|{'ZLINK_HANDOFF_TEST_SLOT':'1'})
job=Path('/root/jobs/fx/cx/v21/s033/trole2')
root=Path('/root/worktree/zlink-v21-s033/trole')
for name,command in [
 ('handoff-final-50',[str(root/'build-trole/test_cpp_framework_receive_role_handoff'),'--gtest_repeat=50','--gtest_break_on_failure']),
 ('negative-run',['python3',str(job/'run_negatives.py')]),
 ('full-suite',['ctest','--test-dir',str(root/'build-trole'),'-L','^(unit|contract)$','--output-on-failure','-j3','--output-junit',str(job/'logs/full-suite.xml')])]:
    print('START',name,flush=True)
    with (job/'logs'/f'{name}.log').open('w') as log:
        subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('END',name,flush=True)
