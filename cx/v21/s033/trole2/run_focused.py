from pathlib import Path
import subprocess, json, re, resource, sys
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
job=Path('/root/jobs/fx/cx/v21/s033/trole2');build=Path('/root/worktree/zlink-v21-s033/trole/build-trole')
i=1
while (job/'logs'/f'focused-run-{i:02}').exists(): i+=1
out=job/'logs'/f'focused-run-{i:02}';out.mkdir()
results=[]
for target in ['test_cpp_framework_receive_role_handoff','test_cpp_framework_handler_entry_permit']:
    path=out/(target+'.log')
    with path.open('w') as log:
        rc=subprocess.call([str(build/target),'--gtest_repeat=50','--gtest_break_on_failure'],cwd=build,stdout=log,stderr=subprocess.STDOUT,timeout=1500)
    content=path.read_text()
    counts={}
    for name in re.findall(r'^\[       OK \] ([\w.]+)',content,re.M): counts[name]=counts.get(name,0)+1
    results.append({'target':target,'exit':rc,'passes':counts})
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(target,rc,counts,flush=True)
    if rc: sys.exit(1)
for target in ['test_cpp_framework_offload_executor','test_cpp_framework_mailbox_owner_dispatch','test_cpp_framework_receive_role_completion','test_cpp_framework_yield_admission','test_cpp_framework_operation_registry']:
    passed=0
    path=out/(target+'.log')
    with path.open('w') as log:
        for iteration in range(1,51):
            log.write(f'iteration={iteration}\n');log.flush()
            try:
                rc=subprocess.call([str(build/target)],cwd=build,stdout=log,stderr=subprocess.STDOUT,timeout=30)
            except subprocess.TimeoutExpired: rc=124
            if rc: break
            passed+=1
    results.append({'target':target,'exit':rc,'passes':passed})
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(target,rc,passed,flush=True)
    if rc: sys.exit(1)
path=out/'T05-known-failure.log'
with path.open('w') as log:
    rc=subprocess.call([str(build/'test_cpp_framework_receive_role_handoff'),'--gtest_also_run_disabled_tests','--gtest_filter=ReceiveRoleHandoff.DISABLED_T05KnownFailureSameNodeOwnerProgress','--gtest_repeat=50'],cwd=build,stdout=log,stderr=subprocess.STDOUT,timeout=1500)
content=path.read_text()
runs=len(re.findall(r'^\[ RUN      \]',content,re.M));passed=len(re.findall(r'^\[       OK \]',content,re.M))
results.append({'target':'T05-known-failure','exit':rc,'runs':runs,'passes':passed,'expected_failures':runs-passed})
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print('T05-known-failure',rc,runs,passed,flush=True)
assert rc==1 and runs==50 and passed==0
