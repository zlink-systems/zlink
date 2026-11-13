import json,pathlib,subprocess,time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain')
tests=json.loads(subprocess.check_output(['ctest','--test-dir',str(job/'test-build'),'--show-only=json-v1'],text=True))['tests']
results=[]
for test in tests:
 if not (test['name'].startswith('test_cpp_framework_typed_json_compile_fail_') or test['name']=='test_cpp_framework_tooling_contract'):continue
 command=[('-DCOMPILER='+str(job/'cxx-slot.sh')) if a.startswith('-DCOMPILER=') else a for a in test['command']]
 if test['name']=='test_cpp_framework_tooling_contract':command.insert(command.index('-P'),'-DCMAKE_COMMAND='+str(job/'cmake-slot.sh'))
 print('START',test['name'],flush=True)
 start=time.monotonic();done=subprocess.run(command)
 results.append({'name':test['name'],'exit_code':done.returncode,'seconds':time.monotonic()-start,'command':command})
 (job/'compiler-checks.json').write_text(json.dumps(results,indent=2))
 print('END',test['name'],done.returncode,flush=True)
 done.check_returncode()
assert len(results)==13
