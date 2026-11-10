#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -lt 2 || "$(uname -s)" != Linux ]]; then
  echo 'usage (Linux): bash scripts/etcd/build-and-run-4x4.sh CPP_BUILD_DIR OUTPUT_DIR [runner options]' >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
cpp_build="$(realpath "$1")"
mkdir -p "$2"
output="$(realpath "$2")"
shift 2
export ZLINK_ETCD_TEST_LOG_DIR="${ZLINK_ETCD_TEST_LOG_DIR:-$output/logs}"
mkdir -p "$ZLINK_ETCD_TEST_LOG_DIR"
cd "$root"

# CPP_BUILD_DIR is configured by the caller with WITH_ETCD=ON and its dependency prefixes.
cmake --build "$cpp_build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-1}" \
  --target zlink_location_store_provider_adapter >"$ZLINK_ETCD_TEST_LOG_DIR/cpp-build.log" 2>&1
(cd framework/languages/node/packages/framework-locations-etcd && npm run build) \
  >"$ZLINK_ETCD_TEST_LOG_DIR/node-build.log" 2>&1
dotnet build framework/languages/dotnet/tests/Zlink.Framework.LocationStore.ProviderAdapter/Zlink.Framework.LocationStore.ProviderAdapter.csproj \
  -c Release -m:1 --disable-build-servers -p:UseSharedCompilation=false -o "$output/dotnet" \
  >"$ZLINK_ETCD_TEST_LOG_DIR/dotnet-build.log" 2>&1
(cd framework/languages/java && bash ./gradlew --no-daemon --max-workers=1 \
  -Pzlink.includeSamples=false :zlink-framework-locations-etcd:installProviderAdapterDist) \
  >"$ZLINK_ETCD_TEST_LOG_DIR/jvm-build.log" 2>&1

node --input-type=module - "$root" "$cpp_build" "$output" <<'JS'
import fs from "node:fs";
import path from "node:path";
const [root, cpp, output] = process.argv.slice(2);
const adapters = {
  node: { command: process.execPath, args: [path.join(root, "framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs")] },
  dotnet: { command: "dotnet", args: [path.join(output, "dotnet/Zlink.Framework.LocationStore.ProviderAdapter.dll")] },
  jvm: { command: path.join(root, "framework/languages/java/zlink-framework-locations-etcd/build/install/provider-adapter/bin/location-store-provider-adapter"), env: { JAVA_OPTS: "-Xmx256m" } },
  cpp: { command: path.join(cpp, "zlink_location_store_provider_adapter"), env: { ZLINK_ETCD_CPP_BUILD_DIR: cpp } },
};
fs.writeFileSync(path.join(output, "adapters.json"), JSON.stringify(adapters, null, 2) + "\n");
JS
node scripts/etcd/conformance-runner.mjs --adapter-config "$output/adapters.json" "$@" \
  >"$ZLINK_ETCD_TEST_LOG_DIR/conformance.log" 2>&1
tail -n 3 "$ZLINK_ETCD_TEST_LOG_DIR/conformance.log"
