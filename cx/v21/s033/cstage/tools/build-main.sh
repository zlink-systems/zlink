#!/bin/bash
set -euo pipefail
root=/root/worktree/zlink-cstage
job=/root/jobs/fx/cx/v21/s033/cstage
export PATH="$job/tools:$PATH"
export ZLINK_LOCAL_PACKAGE_ROOT="$root/.artifacts/wsl"
export ZLINK_BINDING_BUILD_JOBS=2
python3 - <<'INPUTS'
from pathlib import Path
import hashlib,json
r=Path('/root/worktree/zlink-cstage');j=Path('/root/jobs/fx/cx/v21/s033/cstage');hashes={}
for p in (j/'original').rglob('*'):
 if p.is_file():
  f=p.relative_to(j/'original');actual=r/f
  if str(f).startswith(('core/','bindings/')):assert actual.read_bytes()==p.read_bytes(),str(f)
  hashes[str(f)]=hashlib.sha256(actual.read_bytes()).hexdigest()
(j/'main-source-sha.json').write_text(json.dumps(hashes,indent=2))
INPUTS
bash "$job/tools/core-compile.sh"
mkdir -p "$root/.artifacts/wsl/install/zlink-core/1.23.0/share/zlink"
cp /root/.cache/zlink/core/1.23.0/linux-x64/share/zlink/core-package-provenance.json "$root/.artifacts/wsl/install/zlink-core/1.23.0/share/zlink/"
python3 - <<'META'
from pathlib import Path
import json,hashlib
p=Path('/root/worktree/zlink-cstage/.artifacts/wsl/install/zlink-core/1.23.0')
f=p/'share/zlink/core-package-provenance.json';x=json.loads(f.read_text());x.update(measurement_only=True,origin='local unmodified main Core source',library_sha256=hashlib.sha256((p/'lib/libzlink.so').read_bytes()).hexdigest());f.write_text(json.dumps(x,indent=2))
META
bash "$job/tools/local-package-build-wsl.sh" cpp
export PATH="$job/tools:$PATH"
cd "$root/framework/bench/grpc"
bash cpp/build.sh
mkdir -p "$job/bin-main" "$job/package-main"
cp cpp/build/bench_cpp_client cpp/build/bench_cpp_zlink_server "$job/bin-main/"
cp -a "$root/.artifacts/wsl/install/zlink-core/1.23.0" "$job/package-main/core"
cp -a "$root/.artifacts/wsl/install/zlink-cpp/1.23.0" "$job/package-main/cpp"
