from pathlib import Path
import subprocess
W=Path('/root/worktree/zlink-3867-creply2');P=Path('/root/jobs/fx/cx/v21/s033/creply2');B=W/'framework/bench/grpc/cpp/build'
subprocess.run(['cmake','-S',str(W/'framework/bench/grpc/cpp'),'-B',str(B),'-DCMAKE_BUILD_TYPE=Debug','-DZLINK_FRAMEWORK_CPP_TEST_PROBES=ON','-DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF','-DCMAKE_CXX_FLAGS_DEBUG=-O0 -g1 -fno-omit-frame-pointer --param=ggc-min-expand=20 --param=ggc-min-heapsize=65536'],check=True)
available_kb=int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:')))
jobs='3' if available_kb>=20*1048576 else '2'
subprocess.run(['python3',str(P/'tools/build_suite.py'),'-j'+jobs],check=True)
