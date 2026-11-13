from pathlib import Path
import shutil
p=Path('/root/jobs/fx/cx/v21/s033/jsend');w=Path('/root/worktree/zlink-jsend/framework/bench/grpc/java')
for module in ('client','zlink-framework-server'):
 src=w/module/'build/install'
 shutil.copytree(src,p/'prototype-build'/module/'build/install',dirs_exist_ok=True)
