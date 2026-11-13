import pathlib, shlex, subprocess, sys
R=pathlib.Path('/root/worktree/zlink-cproto')
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/cproto')
b=R/'framework/bench/grpc/cpp/build'
lines=subprocess.check_output(['ninja','-t','commands','bench_cpp_client'],cwd=b,text=True).splitlines()
command=next(shlex.split(l) for l in lines if l.startswith('/usr/bin/c++') and 'bench_cpp_client.cpp.o -c ' in l)
command[command.index('-o')+1]=str(J/'wire_check.o'); command[command.index('-c')+1]=str(J/'wire_check.cpp')
# Ninja dependency output belongs to the original client. Use an artifact path instead.
if '-MF' in command: command[command.index('-MF')+1]=str(J/'wire_check.d')
command.extend(['-I'+str(R/'framework/languages/cpp/framework/src'),'-I'+str(R/'framework/runtime/protocol/generated/cpp')])
subprocess.run(command,cwd=b,check=True)
command=shlex.split(lines[-1].removeprefix(': && ').removesuffix(' && :'))
command=[str(J/'wire_check.o') if v.endswith('/client/bench_cpp_client.cpp.o') else v for v in command]
command[command.index('-o')+1]=str(J/f'wire-check-{sys.argv[1]}')
subprocess.run(command,cwd=b,check=True)
subprocess.run([str(J/f'wire-check-{sys.argv[1]}'),str(J/f'wire-{sys.argv[1]}')],cwd=b,check=True)
