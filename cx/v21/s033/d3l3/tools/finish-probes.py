import os,pathlib,time,json,subprocess,shutil
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/d3l3');s=pathlib.Path('/root/worktree/zlink-v21-s033/d3l3-scratch');inode=os.stat('/root/jobs/fx/cx/measure.lock').st_ino
os.sched_setaffinity(0,{6,7,14,15})
def held():return any('FLOCK' in l and 'WRITE' in l and '->' not in l and any(v.endswith(':'+str(inode)) for v in l.split()) for l in pathlib.Path('/proc/locks').read_text().splitlines())
def ready():return all('Build succeeded.' in (j/'raw'/f).read_text() for f in ['build-assertions.log','build-probe-cpu.log'])
deadline=time.monotonic()+3600
while held() or not ready():
 if time.monotonic()>deadline:raise RuntimeError('build/guard wait limit')
 time.sleep(2)
rows=[]
for project,label,args in [('Assertions','scratch-assertions',[]),('Probe','cpu-probe',['--cpu'])]:
 while held():time.sleep(2)
 dll=s/project/'bin/Release/net8.0/Zlink.Framework.UnitTests.dll';sources=list((s/project).glob('*.cs'));assert dll.stat().st_mtime>=max(p.stat().st_mtime for p in sources),'stale binary'
 if project=='Assertions':shutil.copytree(s/'bin/L3/Client/runtimes',dll.parent/'runtimes',dirs_exist_ok=True)
 env=os.environ.copy();env.update(DOTNET_TieredCompilation='0',DOTNET_gcServer='0')
 start=time.monotonic()
 with open(j/'raw'/(label+'.json'),'w') as out,open(j/'raw'/(label+'.log'),'w') as err:
  r=subprocess.run(['taskset','-c','6-7,14-15','timeout','-k','1','19s','dotnet',str(dll)]+args,env=env,stdout=out,stderr=err)
 rows.append(dict(project=project,label=label,rc=r.returncode,elapsed=time.monotonic()-start,dllMtime=dll.stat().st_mtime,newestSourceMtime=max(p.stat().st_mtime for p in sources)))
 (j/'raw/final-probe-runs.json').write_text(json.dumps(rows,indent=2));print(rows[-1],flush=True)
 if r.returncode:raise SystemExit(r.returncode)
