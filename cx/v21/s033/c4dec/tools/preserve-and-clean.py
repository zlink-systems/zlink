from pathlib import Path
import shutil,json,hashlib,subprocess
r=Path('/root/jobs/fx/cx/v21/s033/c4dec');wt=Path('/root/worktree/zlink-v21-s033/c4dec').resolve();build=wt/'build-c4dec';art=r/'raw/artifacts';art.mkdir(exist_ok=True)
manifest=[]
for p in list(build.glob('c4dec-*'))+list(build.glob('test_cpp_framework_typed_json*'))+[build/'test_cpp_framework_serializer_registry',build/'libc4dec-malloc-counter.so']:
 if p.is_file():
  dst=art/p.name;shutil.copy2(p,dst);manifest.append({'source':str(p),'archive':str(dst),'sha256':hashlib.sha256(dst.read_bytes()).hexdigest()})
config=art/'build-config';config.mkdir(exist_ok=True)
for name in ['CMakeCache.txt','build.ninja','compile_commands.json','.ninja_log','.ninja_deps']:
 p=build/name
 if p.is_file():shutil.copy2(p,config/name)
if (build/'Testing').exists():shutil.copytree(build/'Testing',art/'Testing',dirs_exist_ok=True)
for p in (build/'CMakeFiles').glob('*.log'):shutil.copy2(p,config/p.name)
for p in (build/'CMakeFiles').glob('*.yaml'):shutil.copy2(p,config/p.name)
(r/'raw/artifact-manifest.json').write_text(json.dumps(manifest,indent=2))
removed=[]
for name in ['build-c4dec','build-c4dec-deps']:
 p=wt/name
 assert p.parent.resolve()==wt and not p.is_symlink()
 if p.exists():shutil.rmtree(p);removed.append(str(p))
(r/'raw/final-state.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=wt,text=True).strip(),'branch':subprocess.check_output(['git','branch','--show-current'],cwd=wt,text=True).strip(),'status':subprocess.check_output(['git','status','--short'],cwd=wt,text=True),'removed_build_directories':removed,'raw_preserved':True},indent=2))
print('preserved',len(manifest),'binaries; removed only',removed)
