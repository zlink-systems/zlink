from pathlib import Path
import difflib,hashlib,json,shutil
r=Path('/root/worktree/zlink-cstage');j=Path('/root/jobs/fx/cx/v21/s033/cstage');orig=j/'original';snap=j/'prototype-source'
new={'core/src/api/socket/staged_request_prototype.hpp','bindings/cpp/tests/prototype/staged_request.cpp'}
files=sorted({str(p.relative_to(orig)) for p in orig.rglob('*') if p.is_file()}|new)
diff=[];manifest={}
for f in files:
 p=r/f;old=(orig/f).read_bytes() if (orig/f).exists() else None;now=p.read_bytes()
 if old==now:continue
 q=snap/f;q.parent.mkdir(parents=True,exist_ok=True);shutil.copy(p,q)
 manifest[f]={'original_sha256':hashlib.sha256(old).hexdigest() if old is not None else None,'prototype_sha256':hashlib.sha256(now).hexdigest()}
 diff.append(f'diff --git a/{f} b/{f}\n')
 if old is None:diff.append('new file mode 100644\n')
 diff.extend(difflib.unified_diff((old or b'').decode().splitlines(True),now.decode().splitlines(True),fromfile=f'a/{f}' if old is not None else '/dev/null',tofile=f'b/{f}'))
(j/'prototype.diff').write_text(''.join(diff));(j/'source-manifest.json').write_text(json.dumps(manifest,indent=2))
print('saved',len(manifest),'files; diff lines',sum(1 for _ in (j/'prototype.diff').open()))
