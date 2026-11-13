from pathlib import Path
import hashlib,json,shutil
r=Path('/root/worktree/zlink-cstage');j=Path('/root/jobs/fx/cx/v21/s033/cstage');m=json.loads((j/'source-manifest.json').read_text())
for f,x in m.items():
 p=r/f;assert hashlib.sha256(p.read_bytes()).hexdigest()==x['prototype_sha256'],('source changed after snapshot',f)
for f,x in m.items():
 p=r/f
 if x['original_sha256'] is None:p.unlink()
 else:shutil.copyfile(j/'original'/f,p)
p=r/'bindings/cpp/tests/prototype'
if p.exists() and not any(p.iterdir()):p.rmdir()
print('restored own modifications only:',len(m))
