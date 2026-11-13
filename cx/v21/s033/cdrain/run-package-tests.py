import json,pathlib,subprocess,time,sys
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain')
tests=json.loads(subprocess.check_output(['ctest','--test-dir',str(job/'build'),'--show-only=json-v1'],text=True))['tests']
selected=set(sys.argv[1:]) if len(sys.argv)>1 else {'test_cpp_framework_install_consumer','test_cpp_stream_connector_install_consumer'}
results=[]
for test in tests:
 if test['name'] not in selected:continue
 command=test['command'];command.insert(command.index('-P'),'-DCMAKE_COMMAND='+str(job/'cmake-slot.sh'))
 print('START',test['name'],flush=True)
 start=time.monotonic();done=subprocess.run(command)
 results.append({'name':test['name'],'command':command,'seconds':time.monotonic()-start,'exit_code':done.returncode})
 (job/('package-tests-'+('-'.join(sorted(selected)))+'.json')).write_text(json.dumps(results,indent=2))
 print('END',test['name'],done.returncode,flush=True)
 done.check_returncode()
assert len(results)==len(selected)
