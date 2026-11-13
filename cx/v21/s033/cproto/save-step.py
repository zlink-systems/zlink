from pathlib import Path
import shutil, subprocess, sys
r=Path('/root/worktree/zlink-cproto');j=Path('/root/jobs/fx/cx/v21/s033/cproto');b=r/'framework/languages/cpp/build/linux-ninja-release'
out=j/sys.argv[1];out.mkdir(exist_ok=True)
for name in ['libzlink_framework.a','libzlink_framework_provider_abstractions.a','CMakeCache.txt']:
 if (b/name).exists():shutil.copy2(b/name,out/name)
with (j/(sys.argv[1]+'.diff')).open('w') as f:
 subprocess.run(['git','diff','--','framework/languages/cpp'],cwd=r,stdout=f,check=True)
print('saved',out)
