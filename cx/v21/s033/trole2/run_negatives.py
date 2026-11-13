from pathlib import Path
import subprocess,json,re,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=Path('/root/jobs/fx/cx/v21/s033/trole2/negative')
build=Path('/root/worktree/zlink-v21-s033/trole/build-trole')
results=[]
for case in json.loads((out/'manifest.json').read_text()):
    name=case['id'];pattern=case['pattern'];path=out/(name+'-test.log')
    with path.open('w') as log:
        command=[case['binary']]+(['--gtest_filter='+pattern] if pattern else [])
        try: rc=subprocess.call(command,cwd=build,stdout=log,stderr=subprocess.STDOUT,timeout=30)
        except subprocess.TimeoutExpired: rc=124
    content=path.read_text()
    if pattern:
        detected='Failure' in content and '[  FAILED  ]' in content
    elif name=='T04':
        detected='Assertion' in content and 'receiving' in content
    else:
        count=re.search(r'worker_notifications_for_129_records=(\d+)',content)
        detected=bool(count and int(count.group(1))!=1)
    results.append({'id':name,'exit':rc,'rejected':rc!=0 and detected,
                    'source':case['source'],'mutation':case['mutation']})
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(name,rc,'detected='+str(detected),flush=True)
    if rc==0 or not detected: raise RuntimeError((name,'mutation survived or unrelated failure'))
