from pathlib import Path
import hashlib,json,subprocess
root=Path('/root/worktree/zlink-v21-s033/n2hdr');job=Path('/root/jobs/fx/cx/v21/s033/n2hdr');raw=job/'raw'
get=lambda *args:subprocess.check_output(['git','-C',str(root),*args],text=True).strip()
changed=get('diff','--name-only').splitlines()+get('ls-files','--others','--exclude-standard').splitlines()
(raw/'changed-files.txt').write_text('\n'.join(changed)+'\n')
hashes=[]
for name in changed:
 if '/packages/' in name and '/src/' in name and name.endswith('.ts'):
  src=root/name;dist=Path(str(root/name.replace('/src/','/dist/'))[:-3]+'.js')
  assert dist.stat().st_mtime_ns>=src.stat().st_mtime_ns
  for f in [src,dist]: hashes.append(dict(path=str(f),sha256=hashlib.sha256(f.read_bytes()).hexdigest(),mtimeNs=f.stat().st_mtime_ns))
for name in ['zlink.node','libzlink.so.0']:
 f=root/'framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64'/name
 hashes.append(dict(path=str(f),sha256=hashlib.sha256(f.read_bytes()).hexdigest(),mtimeNs=f.stat().st_mtime_ns))
(raw/'final-code-hashes.json').write_text(json.dumps(hashes,indent=2))
provenance=json.loads((raw/'baseline-module-provenance.json').read_text())
provenance['candidateHeaderDistSha256']='df4b655ea6cc284739e3d8807bf8c23dbaf96739fc60466108140da8e2774963'
provenance['finalHeaderDistSha256']=hashlib.sha256((root/'framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js').read_bytes()).hexdigest()
(raw/'baseline-module-provenance.json').write_text(json.dumps(provenance,ensure_ascii=False,indent=2))
labels=['own-before-L1','own-before-L2','own-after-L1-final','own-after-L2-final','audit-direct-before-L2','audit-final-L2','audit-maps-L1','audit-maps-L2']
audit={}
for label in labels:
 p=raw/'node'/label;phases={}
 for phase in ['warmup','active']:
  s=json.loads((p/(phase+'-source-stats.json')).read_text());t=json.loads((p/(phase+'-target-stats.json')).read_text())
  assert s['errors']==t['errors']==0
  assert s['submitted']==s['completed']==t['received']==t['completed']
  assert s['inFlight']==t['inFlight']==0
  phases[phase]=dict(submitted=s['submitted'],completed=s['completed'],received=t['received'],errors=0)
 life=json.loads((p/'lifecycle.json').read_text());assert life['elapsed']<20 and all(v['returncode']==0 for v in life['processes'])
 for point in ['start','active']:
  snap=json.loads((p/(point+'.json')).read_text())
  assert all(t['affinity']=='6-7,14-15' for rows in snap['processes'].values() for t in rows)
 for f in p.glob('*.cpuprofile'):
  cpu=json.loads(f.read_text())
  foreign=[v['callFrame']['url'] for v in cpu['nodes'] if '/root/worktree/' in v['callFrame']['url'] and not any(prefix in v['callFrame']['url'] for prefix in [str(root)+'/',str(root)+'-scratch/'])]
  assert not foreign,(label,foreign[:3])
 audit[label]=dict(phases=phases,lifecycle=life)
(raw/'profile-audit.json').write_text(json.dumps(audit,indent=2))
print('validated',len(labels),'profile/audit runs, production dist pairs',(len(hashes)-2)//2)
