from pathlib import Path
import subprocess, os, sys, resource, json
job=Path('/root/jobs/fx/cx/v21/s033/sameowner-impl')
root=Path('/root/worktree/zlink-v21-s033/sameowner-impl')
build=root/'build-sameowner'
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
mode=sys.argv[1]
repeat=50 if mode.startswith('repeat') else 1
filters='SameOwner.*:ReceiveRoleHandoff.T05SameNodeOwnerProgress:ReceiveRoleHandoff.T05IndependentGatesDuringSynchronousHandler:ReceiveRoleHandoff.T07AwaitKeepsGateAndYieldReacquiresIt'
env=os.environ.copy()
env['LD_LIBRARY_PATH']='/root/.cache/zlink/core-release/1.24.0/linux-x64/lib'+(':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
results=[]
for name,args in [('mailbox',[str(build/'test_cpp_framework_receive_role_handoff'),'--gtest_filter='+filters,'--gtest_repeat='+str(repeat),'--gtest_output=xml:'+str(job/(mode+'-mailbox.xml'))]),('channel',[str(build/'test_cpp_framework_client_server_runtime'),'--same-owner-regression']),('actor',[str(build/'test_cpp_framework_client_server_runtime'),'--same-owner-gated-control'])]:
    if name=='mailbox' and mode!='fail-before':
        args.append('--gtest_break_on_failure')
    iterations=1 if name=='mailbox' else repeat
    for i in range(iterations):
        runenv=env.copy()
        runenv['ZLINK_CPP_SAME_OWNER_FLOW_LOG']=str(job/f'{mode}-{name}-{i+1:02}.flow')
        log=job/f'{mode}-{name}-{i+1:02}.log'
        with log.open('w') as out:
            try:
                rc=subprocess.run(args,cwd=root,env=runenv,stdout=out,stderr=subprocess.STDOUT,timeout=1800 if mode.startswith('repeat') else 120).returncode
            except subprocess.TimeoutExpired:
                rc=124
        results.append({'case':name,'iteration':i+1,'exit':rc,'log':str(log)})
        print(json.dumps(results[-1]),flush=True)
        (job/f'{mode}-summary.json').write_text(json.dumps(results,indent=2)+'\n')
        if rc and mode!='fail-before':
            sys.exit(1)
sys.exit(0 if mode=='fail-before' or all(r['exit']==0 for r in results) else 1)
