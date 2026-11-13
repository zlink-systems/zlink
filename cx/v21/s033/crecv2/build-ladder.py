from pathlib import Path
import json,subprocess
J=Path(__file__).parent; old=J.parent/'cladder'; current='/root/worktree/zlink-3867r'
c=json.loads((old/'build-config.json').read_text())
source=(old/'src/full-window-client.cpp').read_text().replace('/root/worktree/zlink-3867s',current)
(J/'ladder/src').mkdir(parents=True,exist_ok=True)
(J/'ladder/src/full-window-client.cpp').write_text(source)
b=J/'ladder/build/rung9';b.mkdir(parents=True,exist_ok=True)
def convert(x):return x.replace('/root/worktree/zlink-3867s',current).replace(str(old/'lib'),str(J/'build')).replace(str(old/'src'),str(J/'ladder/src'))
# Baseline used the same generated protobuf ABI; use this build's generated headers/objects.
includes=[convert(x) for x in c['includes']]
includes=[x.replace(str(old/'build'),str(J/'build/bench_proto')) for x in includes]
includes+=['-I'+str(J/'build/bench_proto')]
compile=['c++','-O3','-DNDEBUG','-std=c++20','-g1','-pthread']+[x.replace(chr(92),'') for x in c['defines']]+includes+['-I'+current+'/framework/runtime/protocol/generated/cpp','-c',str(J/'ladder/src/full-window-client.cpp'),'-o',str(b/'full-window-client.o')]
link=['c++','-pthread',str(b/'full-window-client.o'),str(J/'build/CMakeFiles/bench_cpp_client.dir/bench_proto/bench.pb.cc.o'),str(J/'build/CMakeFiles/bench_cpp_client.dir/bench_proto/bench.grpc.pb.cc.o')]+[convert(x) for x in c['libs']]+['-o',str(b/'bench_cpp_client')]
(J/'ladder/build-commands.json').write_text(json.dumps([compile,link],indent=2))
subprocess.run(compile,check=True);subprocess.run(link,check=True)
for name in ['bench_cpp_grpc_server','bench_cpp_zlink_server','bench_cpp_framework_server']:
 p=b/name
 if not p.exists():p.symlink_to(J/'build'/name)
