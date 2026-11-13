import hashlib,subprocess,sys,datetime
from pathlib import Path
mode,size,kind,out=sys.argv[1:]
root=Path('/root/worktree/zlink-v21-s033/lad-java-l57p');job=Path('/root/jobs/fx/cx/v21/s033/jl57bp')
def git(*args): return subprocess.check_output(['git',*args],cwd=root,text=True).strip()
source=root/'framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java'
rows=['source='+str(source)+' sha256='+hashlib.sha256(source.read_bytes()).hexdigest()]
for module,install in [('client','bench-client'),('zlink-framework-server','bench-zlink-framework-server')]:
 mod=root/'framework/bench/grpc/java'/module
 jars=list((mod/'build/install'/install/'lib').glob('*.jar'))
 own=mod/'build/install'/install/'lib'/(module+'.jar')
 sources=list((mod/'src').rglob('*.java'))+list((root/'framework/bench/grpc/java/shared/src').rglob('*.java'))
 assert own.exists() and all(own.stat().st_mtime>p.stat().st_mtime for p in sources),(own,'stale')
 rows.extend(str(p)+' sha256='+hashlib.sha256(p.read_bytes()).hexdigest() for p in jars)
meta=f"{datetime.datetime.now(datetime.timezone.utc).isoformat()} {mode} request-backpressure-{size} {kind} HEAD={git('rev-parse','HEAD')} dirty={bool(git('status','--porcelain'))} base={git('merge-base','HEAD','origin/main')} raw={out}"
Path(out,'artifacts.txt').write_text(meta+'\n'+'\n'.join(rows)+'\n')
with (job/'progress.md').open('a') as f:
 f.write('\n- '+meta+'; artifacts: '+out+'/artifacts.txt; receive/reply comparison: call-shape.md rows 2/3. L5_7 restores owner queue/worker and activation: extra queue waits, per-request scope and typed JSON allocations, same metrics owner. Binding recv blocking once/message, fresh envelope+protobuf reply; L5_7 production receiver/dispatcher, fresh envelope+typed JSON reply. Main uses same Framework queue/worker with protobuf. No added bench waits/atomics/copies. Baseline L5_7 admission omission is deliberately reproduced as requested.\n')
