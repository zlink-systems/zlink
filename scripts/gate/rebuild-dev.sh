#!/usr/bin/env bash
# Rebuild core/build-dev from the checked-out Core, refresh the local Core prefix and the four
# framework language packages, then reinstall node/java against them. Never run while a gate runs.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh" || exit $?
cd "$Z" || exit
echo "[$(ts)] rebuilding core/build-dev at $(git rev-parse --short HEAD)"
JOBS=${JOBS:-8} scripts/build-core.sh dev > "$LOGS/core.log" 2>&1 || { echo "CORE BUILD FAILED"; exit 1; }
bash scripts/gate/materialize-local-core-prefix.sh > "$LOGS/prefix.log" 2>&1 || { echo "PREFIX FAILED"; exit 1; }
scripts/local-package/build-wsl.sh cpp dotnet java node > "$LOGS/pkgs.log" 2>&1 || { echo "PKG BUILD FAILED"; exit 1; }
scripts/local-package/http-client/build-wsl.sh node > "$LOGS/http-client-pkg.log" 2>&1 || { echo "HTTP CLIENT PKG BUILD FAILED"; exit 1; }
CPP_VER="$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/cpp/VERSION)"
CPP_E2E_BUILD="$Z/framework/languages/cpp/build/linux-ninja-c-e2e"
if [[ ! -f "$CPP_E2E_BUILD/CMakeCache.txt" ]] || ! grep -q '^CMAKE_TOOLCHAIN_FILE:[^=]*=.' "$CPP_E2E_BUILD/CMakeCache.txt"; then
  rm -rf "$CPP_E2E_BUILD"
  cmake -S framework/languages/cpp -B "$CPP_E2E_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=ON \
    -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
    -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX="$Z/.artifacts/wsl/install/zlink-core/$CORE_VER" \
    -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX="$Z/.artifacts/wsl/install/zlink-cpp/$CPP_VER" \
    > "$LOGS/cpp-c-e2e-configure.log" 2>&1 || { echo "CPP C-E2E CONFIGURE FAILED"; exit 1; }
fi
( cd framework/languages/node && flock -w7200 /tmp/zlink-node-gate.lock bash -lc 'unset ZLINK_LIBRARY_PATH; TMPDIR=/dev/shm/zlink-tmp-node rm -rf node_modules/@zlink-systems && npm install --no-package-lock --no-save --no-audit --no-fund && TMPDIR=/dev/shm/zlink-tmp-node npm run build' ) > "$LOGS/node.log" 2>&1 || echo "NODE REINSTALL FAILED"
( cd framework/languages/java && flock -w7200 /tmp/zlink-jvm-gate.lock env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java ./gradlew --no-daemon --refresh-dependencies :zlink-framework-core:compileJava ) > "$LOGS/java.log" 2>&1 || echo "JAVA REFRESH FAILED"
NODE_BINDING_VER="$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/node/VERSION)"
echo "[$(ts)] rebuild done"; ls -l --time-style=+%T ".artifacts/wsl/nuget/Zlink.$DOTNET_BINDING_VER.nupkg" ".artifacts/wsl/npm/zlink-systems-zlink-$NODE_BINDING_VER.tgz" "core/build-dev/lib/libzlink.so.$CORE_VER" | awk '{print $6,$7}'
sha256sum "core/build-dev/lib/libzlink.so.$CORE_VER" | tee "$LOGS/core.sha256"
touch "$LOGS/rebuild.done"
