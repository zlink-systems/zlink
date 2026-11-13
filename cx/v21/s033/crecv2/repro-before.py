from pathlib import Path
import subprocess,shlex,json
root=Path('/root/worktree/zlink-3867r/framework/languages/cpp'); b=root/'build/linux-ninja-release'; j=Path(__file__).parent
commands=(j/'focused-commands.txt').read_text().splitlines()
source='test_cpp_framework_request_transport_projection.cpp'
compile=shlex.split(next(c for c in commands if c.endswith(source)))
compile.insert(1,'-I'+str(j/'before-include'));compile[-1]=str(j/'repro-mesh-cases.cpp');out=compile.index('-o');compile[out+1]=str(j/'projection-before.o')
subprocess.run(compile,cwd=b,check=True)
link=[x for x in shlex.split(commands[-1])[2:-2] if not x.startswith('-Wl,--wrap=') and not x.endswith(('test_cpp_framework_request_call_allocations.cpp.o','test_cpp_framework_request_transport.cpp.o','native_call_counter.cpp.o'))]
for i,x in enumerate(link):
 if x.endswith(source+'.o'):link[i]=str(j/'projection-before.o')
 elif x=='libzlink_framework.a':link[i]='/root/jobs/fx/cx/v21/s033/crecv/build/libzlink_framework.a'
link[link.index('-o')+1]=str(j/'receiver-before')
(j/'repro-commands.json').write_text(json.dumps([compile,link],indent=2))
subprocess.run(link,cwd=b,check=True)
