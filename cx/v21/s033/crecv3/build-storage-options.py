from pathlib import Path
import json,shlex,subprocess
j=Path(__file__).parent;r=Path('/root/worktree/zlink-3867f/framework/languages/cpp')
commands=json.loads((r/'build/linux-ninja-release/compile_commands.json').read_text())
entry=next(x for x in commands if x['file'].endswith('/test_cpp_framework_request_call_allocations.cpp'))
args=shlex.split(entry['command']);clean=[];i=0
while i<len(args):
 a=args[i]
 if a in ['-o','-MF','-MT','-MQ']:i+=2;continue
 if a in ['-c','-MD','-MMD'] or a==entry['file']:i+=1;continue
 clean.append(a);i+=1
clean+=['-I/root/jobs/fx/cx/v21/s033/crecv2/build/bench_proto']
records={}
for name,extra in [('before',['-DCRECV3_BEFORE','-I'+str(j/'before-include')]),('after',[])]:
 cmd=clean[:1]+extra+clean[1:]+[str(j/'storage-options.cpp'),'-o',str(j/f'storage-options-{name}')]
 subprocess.run(cmd,cwd=entry['directory'],check=True)
 records[name]={'command':cmd,'output':subprocess.check_output([str(j/f'storage-options-{name}')],text=True).strip()}
(j/'storage-options.json').write_text(json.dumps(records,indent=2));print(json.dumps(records,indent=2))
