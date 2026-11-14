import hashlib,json,subprocess,sys,time
from pathlib import Path
root=Path('/root/worktree/zlink-v21-s033/n2net'); raw=Path('/root/jobs/fx/cx/v21/s033/n2hdr/raw'); lang=root/'framework/languages/node'
src=lang/'packages/framework/src/runtime/channels/channel-envelope.ts'; out=lang/'packages/framework/dist/runtime/channels/channel-envelope.js'
assert out.stat().st_mtime_ns>=src.stat().st_mtime_ns
paths=[src,out,lang/'node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node',lang/'node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0']
paths+=list(Path('/root/worktree/zlink-v21-s033/n2net-scratch/framework/bench/grpc/node').glob('*/main.js'))
get=lambda *a:subprocess.check_output(['git','-C',str(root),*a],text=True).strip()
r=dict(time=time.time(),head=get('rev-parse','HEAD'),dirty=get('status','--short'),mergeBase=get('merge-base','HEAD','origin/main'),artifacts={str(p):dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),mtimeNs=p.stat().st_mtime_ns) for p in paths})
(raw/(sys.argv[1]+'-fingerprint.json')).write_text(json.dumps(r,indent=2))
with (raw.parent/'progress.md').open('a') as f:f.write('\n- build/검증 fingerprint: '+sys.argv[1]+' '+r['head']+' dirty='+str(bool(r['dirty']))+' merge-base='+r['mergeBase']+'; raw/'+sys.argv[1]+'-fingerprint.json (source/output fresh, own worktree paths·sha256).\n')
