from pathlib import Path
import shutil
j=Path('/root/jobs/fx/cx/v21/s033/cstage');r=Path('/root/worktree/zlink-cstage')
for p in (j/'prototype-source').rglob('*'):
 if p.is_file():
  d=r/p.relative_to(j/'prototype-source');d.parent.mkdir(parents=True,exist_ok=True);shutil.copy(p,d)
d=r/'bindings/cpp/tests/prototype/staged_request.cpp';d.parent.mkdir(parents=True,exist_ok=True);shutil.copy(j/'tools/prototype_test.cpp',d)
print('Main binaries saved; prototype activated',flush=True)
