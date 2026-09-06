#!/usr/bin/env bash
# Rebuild core/build-dev from the checked-out Core, refresh the local Core prefix and the four
# framework language packages, then reinstall node/java against them. Never run while a gate runs.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"; cd "$Z"
echo "[$(ts)] rebuilding core/build-dev at $(git rev-parse --short HEAD)"
JOBS=${JOBS:-8} scripts/build-core.sh dev > "$LOGS/core.log" 2>&1 || { echo "CORE BUILD FAILED"; exit 1; }
bash scripts/gate/materialize-local-core-prefix.sh > "$LOGS/prefix.log" 2>&1 || { echo "PREFIX FAILED"; exit 1; }
scripts/local-package/build-wsl.sh cpp dotnet java node > "$LOGS/pkgs.log" 2>&1 || { echo "PKG BUILD FAILED"; exit 1; }
( cd framework/languages/node/packages/http-client && npm pack --pack-destination "$Z/.artifacts/wsl/npm" >/dev/null 2>&1 )
( cd framework/languages/node && flock -w7200 /tmp/zlink-node-gate.lock bash -lc 'unset ZLINK_LIBRARY_PATH; TMPDIR=/dev/shm/zlink-tmp-node rm -rf node_modules/@zlink-systems && npm install --no-package-lock --no-save --no-audit --no-fund && TMPDIR=/dev/shm/zlink-tmp-node npm run build' ) > "$LOGS/node.log" 2>&1 || echo "NODE REINSTALL FAILED"
( cd framework/languages/java && flock -w7200 /tmp/zlink-jvm-gate.lock env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java ./gradlew --no-daemon --refresh-dependencies :zlink-framework-core:compileJava ) > "$LOGS/java.log" 2>&1 || echo "JAVA REFRESH FAILED"
echo "[$(ts)] rebuild done"; ls -l --time-style=+%T .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg .artifacts/wsl/npm/zlink-systems-zlink-0.17.0.tgz core/build-dev/lib/libzlink.so.0.17.0 | awk '{print $6,$7}'
sha256sum core/build-dev/lib/libzlink.so.0.17.0 | tee "$LOGS/core.sha256"
touch "$LOGS/rebuild.done"
