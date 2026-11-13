from pathlib import Path
import subprocess,os,sys
if os.environ.get('ZLINK_HANDOFF_TEST_SLOT')!='1':
    os.execve('/root/jobs/fx/cx/test-slot.sh', ['test-slot.sh','12-19','python3',__file__],
              os.environ|{'ZLINK_HANDOFF_TEST_SLOT':'1'})
job=Path('/root/jobs/fx/cx/v21/s033/trole2')
build=Path('/root/worktree/zlink-v21-s033/trole/build-trole')
metrics=job/'logs/receive-role-counters.tsv'
assert not metrics.exists()
for target in ['receive_role_handoff','handler_entry_permit','mailbox_owner_dispatch','receive_role_completion']:
    with (job/'logs'/('measurement-off-'+target+'.log')).open('w') as log:
        subprocess.run([str(build/('test_cpp_framework_'+target))],cwd=build,
            stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
with (job/'logs/measurement-on-mailbox.log').open('w') as log:
    subprocess.run([str(build/'test_cpp_framework_mailbox_owner_dispatch')],cwd=build,
        env=os.environ|{'ZLINK_PERF_RECEIVE_ROLE_LOG':str(metrics)},
        stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
