#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

PACK_DIR="$TEMP_DIR/npm"
BROWSER_DIR="$TEMP_DIR/browser-consumer"
SERVER_DIR="$TEMP_DIR/server-consumer"
mkdir -p "$PACK_DIR" "$BROWSER_DIR" "$SERVER_DIR"

packages=(
  framework
  nestjs
  stream-connector
  framework-codec-protobuf
  framework-codec-msgpack
  framework-locations-redis
  stream-wire
)

expected=(
  '@zlink-systems/framework'
  '@zlink-systems/nestjs'
  '@zlink-systems/stream-connector'
  '@zlink-systems/framework-codec-protobuf'
  '@zlink-systems/framework-codec-msgpack'
  '@zlink-systems/framework-locations-redis'
  '@zlink-systems/stream-wire'
)

cd "$ROOT_DIR"
BINDING_SPEC="$(node -p "require('./package.json').dependencies['@zlink-systems/zlink']")"
if [[ "$BINDING_SPEC" != file:* ]]; then
  echo "Node binding dependency must use the central local-package file pin" >&2
  exit 1
fi
BINDING_TGZ="$(realpath "$ROOT_DIR/${BINDING_SPEC#file:}")"
HTTP_CLIENT_SPEC="$(node -p "require('./package.json').dependencies['@zlink-systems/http-client']")"
if [[ "$HTTP_CLIENT_SPEC" != file:* ]]; then
  echo "Node HTTP client dependency must use the central local-package file pin" >&2
  exit 1
fi
HTTP_CLIENT_TGZ="$(realpath "$ROOT_DIR/${HTTP_CLIENT_SPEC#file:}")"
npm run build
for package_dir in "${packages[@]}"; do
  npm pack --silent "./packages/$package_dir" --pack-destination "$PACK_DIR" >/dev/null
done

node - "$PACK_DIR" "${expected[@]}" <<'NODE'
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const [packDir, ...expected] = process.argv.slice(2);
const names = [];
for (const file of fs.readdirSync(packDir)) {
  const archive = path.join(packDir, file);
  const manifest = JSON.parse(execFileSync('tar', ['-xOf', archive, 'package/package.json'], { encoding: 'utf8' }));
  if (manifest.private === true) throw new Error(`${manifest.name} is still private`);
  names.push(manifest.name);
  if (manifest.name === '@zlink-systems/stream-connector') {
    if (manifest.exports['./browser'] !== undefined) throw new Error('connector tarball still exports ./browser');
    if (manifest.exports['.'].require !== undefined) throw new Error('connector tarball still exports CommonJS runtime');
    const listing = execFileSync('tar', ['-tzf', archive], { encoding: 'utf8' });
    if (/Node(Socket|Duplex|WebSocket|Zlink)|WebSocketHandshake|WebSocketFrameCodec/.test(listing)) {
      throw new Error('connector tarball contains removed Node runtime');
    }
  }
}
if (JSON.stringify(names.sort()) !== JSON.stringify([...expected].sort())) {
  throw new Error(`artifact manifest mismatch\nactual=${names.join(',')}\nexpected=${expected.join(',')}`);
}
NODE

cd "$BROWSER_DIR"
npm init -y >/dev/null
npm install --ignore-scripts \
  "$PACK_DIR"/zlink-systems-stream-wire-*.tgz \
  "$PACK_DIR"/zlink-systems-stream-connector-*.tgz \
  "$PACK_DIR"/zlink-systems-framework-codec-protobuf-*.tgz \
  "$PACK_DIR"/zlink-systems-framework-codec-msgpack-*.tgz >/dev/null

cat > index.ts <<'TS'
import {
  zlinkStreamConnectorFactory,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { zlinkStreamMessagePackCodec } from '@zlink-systems/framework-codec-msgpack';
import { createZlinkStreamProtobufCodec } from '@zlink-systems/framework-codec-protobuf';

const connector = zlinkStreamConnectorFactory.create({
  endpoint: 'wss://browser.example.test/stream',
  codec: zlinkStreamMessagePackCodec,
  dispatchMode: ZlinkStreamDispatchMode.Immediate
});
void connector;
void createZlinkStreamProtobufCodec;
TS
cat > tsconfig.json <<'JSON'
{
  "compilerOptions": {
    "target": "ES2022",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "types": [],
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "strict": true,
    "skipLibCheck": true,
    "noEmit": true
  },
  "include": ["index.ts"]
}
JSON
"$ROOT_DIR/node_modules/.bin/tsc" -p tsconfig.json
"$ROOT_DIR/node_modules/.bin/esbuild" index.ts --bundle --platform=browser --format=esm \
  --outfile=browser.mjs --metafile=browser-meta.json >/dev/null
node - <<'NODE'
const fs = require('node:fs');
const graph = JSON.parse(fs.readFileSync('browser-meta.json', 'utf8'));
const inputs = Object.keys(graph.inputs).join('\n');
const bundle = fs.readFileSync('browser.mjs', 'utf8');
if (inputs.includes('node_modules/@zlink-systems/framework/')) {
  throw new Error('browser graph contains @zlink-systems/framework runtime');
}
for (const forbidden of ['node:net', 'node:tls', 'node:crypto', 'node:async_hooks']) {
  if (inputs.includes(forbidden) || bundle.includes(forbidden)) throw new Error(`browser graph contains ${forbidden}`);
}
if (/\b(Buffer|process|__dirname)\b/.test(bundle)) throw new Error('browser bundle contains Node ambient runtime');
import('@zlink-systems/stream-connector/browser').then(
  () => { throw new Error('removed /browser export unexpectedly resolved'); },
  (error) => {
    if (error.code !== 'ERR_PACKAGE_PATH_NOT_EXPORTED') throw error;
  }
);
NODE

cd "$SERVER_DIR"
npm init -y >/dev/null
npm install --ignore-scripts "$BINDING_TGZ" "$HTTP_CLIENT_TGZ" "$PACK_DIR"/*.tgz >/dev/null
npm ls --all >/dev/null
cat > index.cjs <<'JS'
const { zlinkFramework } = require('@zlink-systems/nestjs');
const { zlinkProtobufCodec } = require('@zlink-systems/framework-codec-protobuf/framework');
const { zlinkMessagePackCodec } = require('@zlink-systems/framework-codec-msgpack/framework');
const wire = require('@zlink-systems/stream-wire');

const options = zlinkFramework().codecs()
  .use(zlinkProtobufCodec())
  .use(zlinkMessagePackCodec())
  .build();
if (options === undefined || wire.ZlinkStreamCodec.Protobuf !== 3) {
  throw new Error('CommonJS framework codec registration failed');
}
JS
node index.cjs

node - "$BROWSER_DIR/node_modules/@zlink-systems/stream-wire" <<'NODE'
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const root = process.argv[2];
const commonjs = require(root);
import(pathToFileURL(path.join(root, 'dist/esm/index.mjs')).href).then((esm) => {
  const header = {
    kind: 2,
    codec: commonjs.ZlinkStreamCodec.Json,
    flags: 0,
    requestSeq: 9n,
    name: 'Fixture',
    metadata: new Map([['key', 'value']]),
    correlationId: 'c-1',
    flowId: '018f0f7c-7b4d-7abc-8def-0123456789ab',
    flowOrigin: 3
  };
  const left = commonjs.encodeStreamWireHeader(header);
  const right = esm.encodeStreamWireHeader(header);
  if (Buffer.compare(Buffer.from(left), Buffer.from(right)) !== 0) {
    throw new Error('stream-wire ESM/CommonJS byte fixture mismatch');
  }
});
NODE

echo "NODE_PACKAGED_CONTRACT_PASS packages=${#expected[@]} browser=esm server=commonjs"
