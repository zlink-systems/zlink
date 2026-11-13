from pathlib import Path
import subprocess
r=Path('/root/jobs/fx/cx/v21/s033/cbclose2')
core='/root/.cache/zlink/core/1.23.0/linux-x64'
for label,header in [('firstpass',r/'firstpass/bindings/cpp/src/Runtime/Native/socket_handle.hpp'),('after',Path('/root/worktree/zlink-4060-cpp-binding/bindings/cpp/src/Runtime/Native/socket_handle.hpp'))]:
 cmd=['g++','-std=c++20','-O3','-pthread','-I/root/worktree/zlink-4060-cpp-binding/bindings/cpp/include','-I'+core+'/include','-DSOCKET_HEADER="'+str(header)+'"',str(r/'wait-proof.cpp'),'-L'+core+'/lib','-lzlink','-Wl,-rpath,'+core+'/lib','-Wl,--wrap=zlink_close','-o',str(r/('wait-proof-'+label))]
 subprocess.run(cmd,check=True)
