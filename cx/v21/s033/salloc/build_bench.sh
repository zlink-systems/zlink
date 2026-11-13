#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-3867a
export VCPKG_ROOT=/root/.cache/zlink/vcpkg
export VCPKG_MAX_CONCURRENCY=3
export ZLINK_LOCAL_PACKAGE_ROOT=/root/worktree/zlink-ratio040/.artifacts/wsl
cmake -S framework/bench/grpc/cpp -B framework/bench/grpc/cpp/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_INSTALLED_DIR=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT="$ZLINK_LOCAL_PACKAGE_ROOT"
# build.sh owns configuration; enforce this task's compile parallelism and keep-going check.
cmake() {
  if [[ "$1" == --build ]]; then
    command cmake --build "$2" --parallel 3 -- -k 0
  else
    command cmake "$@"
  fi
}
export -f cmake
bash framework/bench/grpc/cpp/build.sh
