import pathlib,subprocess,json,time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain')
repo=pathlib.Path('/root/worktree/zlink-1639c')
relative=pathlib.Path('framework/languages/cpp/tests/Zlink.Framework.PackageTests/install_consumer.cmake')
review=job/'fixture-review'
fixture=review/relative
fixture.parent.mkdir(parents=True,exist_ok=True)
fixture.write_bytes((repo/relative).read_bytes())
subprocess.run(['patch','-p1','--directory',str(review),'--input',str(job/'package-fixture-proposal.patch')],check=True)
tests=json.loads(subprocess.check_output(['ctest','--test-dir',str(job/'build'),'--show-only=json-v1'],text=True))['tests']
results=[]
for name in ('test_cpp_stream_connector_install_consumer','test_cpp_framework_install_consumer'):
 test=next(t for t in tests if t['name']==name)
 command=test['command'];command.insert(command.index('-P'),'-DCMAKE_COMMAND='+str(job/'cmake-slot.sh'))
 if name=='test_cpp_framework_install_consumer':command[-1]=str(fixture)
 print('START',name,'temporary proposed fixture' if name=='test_cpp_framework_install_consumer' else 'official fixture',flush=True)
 start=time.monotonic();done=subprocess.run(command)
 results.append({'name':name,'proposed_fixture':name=='test_cpp_framework_install_consumer','exit_code':done.returncode,'seconds':time.monotonic()-start,'command':command})
 (job/'package-final-validation.json').write_text(json.dumps(results,indent=2))
 print('END',name,done.returncode,flush=True)
 done.check_returncode()
