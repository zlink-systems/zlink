from pathlib import Path
import subprocess,shlex,json
root=Path('/root/worktree/zlink-3867r/framework/languages/cpp'); b=root/'build/linux-ninja-release'; j=Path(__file__).parent
commands=(j/'focused-commands.txt').read_text().splitlines()
source='test_cpp_framework_request_transport_projection.cpp'
compile=shlex.split(next(c for c in commands if c.endswith(source)))
compile.insert(1,'-I'+str(j/'before-include'));compile=[x for x in compile if x!='-DZLINK_FRAMEWORK_TEST_CREATION_PROBE'];compile[-1]=str(j/'registration-fault-before.cpp');out=compile.index('-o');compile[out+1]=str(j/'registration-fault-before.o')
subprocess.run(compile,cwd=b,check=True)
link=[x for x in shlex.split(commands[-1])[2:-2] if not x.startswith('-Wl,--wrap=') and not ('CMakeFiles/' in x and x.endswith('.o'))]
link.insert(link.index('-o'),str(j/'registration-fault-before.o'))
for i,x in enumerate(link):
 if x=='libzlink_framework.a':link[i]='/root/jobs/fx/cx/v21/s033/crecv/build/libzlink_framework.a'
link[link.index('-o')+1]=str(j/'registration-fault-before')
(j/'registration-fault-before-commands.json').write_text(json.dumps([compile,link],indent=2))
subprocess.run(link,cwd=b,check=True)
