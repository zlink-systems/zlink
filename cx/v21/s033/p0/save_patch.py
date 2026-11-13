from pathlib import Path
import subprocess
r=Path('/root/worktree/zlink-v21-s033/p0')
p=subprocess.run(['git','diff','--binary'],cwd=r,capture_output=True,check=True).stdout
for name in ['framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp','framework/bench/grpc/cpp/CMakeUserPresets.json']:
 f=r/name
 if f.exists():
  d=subprocess.run(['git','diff','--no-index','--','/dev/null',str(f)],capture_output=True)
  assert d.returncode==1
  s=d.stdout.decode().replace('b'+str(r)+'/', 'b/').replace('a'+str(r)+'/', 'a/')
  p+=s.encode()
Path('/root/jobs/fx/cx/v21/s033/p0/counters.diff').write_bytes(p)
