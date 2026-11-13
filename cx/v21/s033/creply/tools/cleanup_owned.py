from pathlib import Path
import subprocess,shutil,json,difflib
P=Path('/root/jobs/fx/cx/v21/s033/creply');W=Path('/root/worktree/zlink-creply')
# Invoke only after all processes from this task have stopped.
owned={'framework/bench/grpc/cpp/client/bench_cpp_client.cpp':'bench_cpp_client.before.cpp','framework/bench/grpc/cpp/CMakeLists.txt':'CMakeLists.before.txt','framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp':'raw_route_port.before.cpp'}
changes=subprocess.check_output(['git','diff','--',*owned],cwd=W,text=True)
(P/'measurement-tracked.diff').write_text(changes)
bench_diff=subprocess.check_output(['git','diff','--','framework/bench/grpc/cpp/client/bench_cpp_client.cpp','framework/bench/grpc/cpp/CMakeLists.txt'],cwd=W,text=True)
for n in ('creply_mutex.c','creply_mutex_test.c','creply_close_probe.cpp'):
 rel='framework/bench/grpc/cpp/measurement/'+n
 if (W/rel).exists():bench_diff+=''.join(difflib.unified_diff([], (W/rel).read_text().splitlines(True),fromfile='/dev/null',tofile='b/'+rel))
(P/'bench.diff').write_text(bench_diff)
archive=P/'measurement-source';archive.mkdir(exist_ok=True)
for rel,original in owned.items():
 f=W/rel;dest=archive/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,dest)
 if rel.endswith('raw_route_port.cpp'):
  allowed=[(P/'tools'/original).read_text()]
  if (P/'tools/raw_route_port.direct.cpp').exists():allowed.append((P/'tools/raw_route_port.direct.cpp').read_text())
  assert f.read_text() in allowed,'Unrecognized runtime edits; do not restore'
 f.write_bytes((P/'tools'/original).read_bytes())
newdir=W/'framework/bench/grpc/cpp/measurement'
for n in ('creply_mutex.c','creply_mutex_test.c','creply_close_probe.cpp'):
 f=newdir/n
 if f.exists():shutil.copy2(f,archive/n);f.unlink()
if newdir.exists() and not any(newdir.iterdir()):newdir.rmdir()
build=W/'framework/bench/grpc/cpp/build'
if build.exists():shutil.rmtree(build)
status=subprocess.check_output(['git','status','--short'],cwd=W,text=True);(P/'final-status.txt').write_text(status)
subprocess.run(['git','diff','--check'],cwd=W,check=True)
print(status)
