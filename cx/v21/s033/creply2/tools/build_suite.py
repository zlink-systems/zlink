import json,pathlib,re,subprocess,sys
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply2')
B=pathlib.Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
listing=subprocess.check_output(['ctest','--test-dir',str(B),'--show-only=json-v1','-L','^(unit|contract)$'],text=True)
(P/'ctest-list.json').write_text(listing)
tests=json.loads(listing)['tests'];names={t['name'] for t in tests}
commands={};all_arguments=[]
for f in B.rglob('CTestTestfile.cmake'):
 text=f.read_text()
 for name,command in re.findall(r'add_test\(\[=\[(.*?)\]=\] "([^"]+)"',text):
  if name in names:commands[name]=command
 for name,body in re.findall(r'add_test\(\[=\[(.*?)\]=\] ([^\n]*)\)',text):
  if name in names:all_arguments.extend(re.findall(r'"([^"]*)"',body))
available={line.rsplit(': ',1)[0] for line in subprocess.check_output(['ninja','-C',str(B),'-t','targets','all'],text=True).splitlines() if ': ' in line}
targets={'test_cpp_framework_reply_close','test_cpp_framework_close_failure','creply_prototype_close_control'}
for command in all_arguments:
 p=pathlib.Path(command)
 if p.is_relative_to(B):
  relative=str(p.relative_to(B))
  if relative in available:targets.add(relative)
# Script-based contract checks consume these libraries even without a test executable.
for t in ('zlink_framework','zlink_framework_http_client','zlink_framework_stream_connector'):
 if t in available:targets.add(t)
(P/'suite-targets.json').write_text(json.dumps({'test_count':len(tests),'targets':sorted(targets),'script_tests':{n:c for n,c in commands.items() if not c.startswith(str(B))}},indent=2))
jobs=next((arg[2:] for arg in sys.argv if arg.startswith('-j')), '3')
print('Selected tests:',len(tests),'build targets:',len(targets),'parallel jobs:',jobs,flush=True)
if '--list-only' not in sys.argv and '--parallel-only' in sys.argv:
 subprocess.run(['cmake','--build',str(B),'--target',*sorted(targets),'-j'+jobs],check=True)
elif '--list-only' not in sys.argv:
 # The largest translation units need several GiB each. Compile
 # only the very largest objects alone, medium objects at -j2, then the
 # remaining full suite at the requested parallelism.
 names={target.rsplit('/',1)[-1] for target in targets};large=[];very_large=[]
 for line in (B/'build.ninja').read_text().splitlines():
  if not line.startswith('build ') or '.cpp.o:' not in line:continue
  obj=line.split(':',1)[0][6:];source=pathlib.Path(line.split(':',1)[1].split()[1])
  match=re.search(r'CMakeFiles/(.*?)\.dir/',obj)
  if match and match[1] in names and source.is_file() and source.stat().st_size>200*1024:
   (very_large if source.stat().st_size>300*1024 else large).append(obj)
 print('Very large object targets:',len(very_large),'at -j1; large:',len(large),f'at -j2; full suite at -j{jobs}',flush=True)
 if very_large:subprocess.run(['cmake','--build',str(B),'--target',*very_large,'-j1'],check=True)
 if large:subprocess.run(['cmake','--build',str(B),'--target',*large,'-j2'],check=True)
 subprocess.run(['cmake','--build',str(B),'--target',*sorted(targets),'-j'+jobs],check=True)

