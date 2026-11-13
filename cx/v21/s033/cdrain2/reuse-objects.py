from pathlib import Path
import shutil,sys
old=Path('/root/jobs/fx/cx/v21/s033/cdrain')/sys.argv[1]
new=old.parent.with_name('cdrain2')/sys.argv[1]
count=0
for p in (old/'CMakeFiles').rglob('*.o'):
 dst=new/p.relative_to(old)
 if dst.exists(): continue
 dst.parent.mkdir(parents=True,exist_ok=True)
 shutil.copy2(p,dst);count+=1
for name in ('.ninja_deps','.ninja_log'):
 shutil.copy2(old/name,new/name)
print(f'{sys.argv[1]}: reused {count} unchanged candidate objects; Ninja must validate command hashes and dependencies')
