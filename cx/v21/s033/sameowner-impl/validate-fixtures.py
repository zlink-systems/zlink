from pathlib import Path
import subprocess, os, resource, shutil, json
job=Path(__file__).parent
root=Path('/root/worktree/zlink-v21-s033/sameowner-impl')
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
resource.setrlimit(resource.RLIMIT_AS,(26843545600,26843545600))
env=os.environ.copy()
env['LD_LIBRARY_PATH']='/root/.cache/zlink/core-release/1.24.0/linux-x64/lib'+(':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
env['ZLINK_CPP_SAME_OWNER_FLOW_LOG']=str(job/'fixtures-targeted.flow')
names=['test_cpp_framework_m6b_runtime']
pattern='^('+'|'.join(names)+')$'
with (job/'fixtures-targeted.log').open('w') as log:
    rc=subprocess.run(['ctest','--test-dir',str(root/'build-sameowner'),'-R',pattern,'--output-on-failure','-j3'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT).returncode
(job/'fixtures-targeted.exit').write_text(str(rc)+'\n')
for name in ('LastTest.log','LastTestsFailed.log','CTestCostData.txt'):
    p=root/'build-sameowner/Testing/Temporary'/name
    if p.exists(): shutil.copy2(p,job/('fixtures-targeted-'+name))
if rc: raise SystemExit(rc)
metadata=json.loads(subprocess.check_output(['ctest','--test-dir',str(root/'build-sameowner'),'--show-only=json-v1'],cwd=root,env=env,text=True))
test=next(t for t in metadata['tests'] if t['name']=='test_cpp_framework_mailbox_owner_dispatch')
timeout=next((p['value'] for p in test['properties'] if p['name']=='TIMEOUT'),None)
results=[]
for index in range(1,51):
    path=job/f'owner-dispatch-repeat-{index:02}.log'
    with path.open('w') as log:
        try:
            rc=subprocess.run(test['command'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout).returncode
        except subprocess.TimeoutExpired: rc=124
    row={'iteration':index,'exit':rc,'log':str(path)}
    results.append(row)
    (job/'owner-dispatch-repeat-summary.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(row),flush=True)
    if rc: raise SystemExit(1)
(job/'owner-dispatch-repeat.exit').write_text('0\n')
subprocess.run(['python3',str(job/'finalize-report.py')],cwd=root,env=env,check=True)
