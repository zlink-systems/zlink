import pathlib,re,json,subprocess
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain')
p=job/'test-build/CTestTestfile.cmake';s=p.read_text()
s,n=re.subn(r'"-DCOMPILER=[^"]+"','"-DCOMPILER='+str(job/'cxx-slot.sh')+'"',s)
assert n==12,n
lines=s.splitlines()
for i,line in enumerate(lines):
 if line.startswith('add_test(') and 'test_cpp_framework_tooling_contract' in line and '-DCMAKE_COMMAND=' not in line:
  assert '"-P"' in line
  lines[i]=line.replace('"-P"','"-DCMAKE_COMMAND='+str(job/'cmake-slot.sh')+'" "-P"',1)
tests=json.loads(subprocess.check_output(['ctest','--test-dir',str(job/'test-build'),'--show-only=json-v1'],text=True))['tests']
for test in tests:
 if test['name'] in ('test_godot_stream_connector','test_axmol_stream_connector'):
  env=next((v['value'] for v in test['properties'] if v['name']=='ENVIRONMENT'),[])
  env=[v for v in env if v.split('=',1)[0] not in ('MSBUILDDISABLENODEREUSE','DOTNET_CLI_USE_MSBUILD_SERVER','UseSharedCompilation')]
  env+=['MSBUILDDISABLENODEREUSE=1','DOTNET_CLI_USE_MSBUILD_SERVER=0','UseSharedCompilation=false']
  lines.append('set_tests_properties('+test['name']+' PROPERTIES ENVIRONMENT '+json.dumps(';'.join(env))+')')
p.write_text('\n'.join(lines)+'\n')
