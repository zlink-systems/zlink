#!/bin/bash
set -euo pipefail
root=/root/worktree/zlink-cstage
job=/root/jobs/fx/cx/v21/s033/cstage
export PATH="$job/tools:$PATH"
export ZLINK_LOCAL_PACKAGE_ROOT="$root/.artifacts/wsl"
export ZLINK_BINDING_BUILD_JOBS=2
export CSTAGE_BUILD_JOBS=2
bash "$job/tools/core-compile.sh"
python3 - <<'PY'
from pathlib import Path
import json,hashlib
p=Path('/root/worktree/zlink-cstage/.artifacts/wsl/install/zlink-core/1.23.0')
f=p/'share/zlink/core-package-provenance.json'
x=json.loads(f.read_text());x.update(measurement_only=True,origin='local Core source prototype',library_sha256=hashlib.sha256((p/'lib/libzlink.so').read_bytes()).hexdigest());f.write_text(json.dumps(x,indent=2))
PY
bash "$job/tools/local-package-build-wsl.sh" cpp
mkdir -p "$job/package-staged/core" "$job/package-staged/cpp"
cp -a "$root/.artifacts/wsl/install/zlink-core/1.23.0/." "$job/package-staged/core/"
cp -a "$root/.artifacts/wsl/install/zlink-cpp/1.23.0/." "$job/package-staged/cpp/"
g++ -std=c++20 -O2 -g -pthread \
  -I"$root/bindings/cpp/src" \
  -I"$job/package-staged/cpp/include" -I"$job/package-staged/core/include" \
  "$root/bindings/cpp/tests/prototype/staged_request.cpp" \
  -L"$job/package-staged/cpp/lib" -L"$job/package-staged/core/lib" \
  -Wl,-rpath,"$job/package-staged/core/lib" -lzlink_cpp -lzlink -o "$job/prototype_test"
