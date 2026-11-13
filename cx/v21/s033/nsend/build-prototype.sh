#!/usr/bin/env bash
set -euo pipefail
python3 -c "from pathlib import Path; m=int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:'))); print('BUILD MemAvailableKiB',m); assert m>=12582912"
cd /root/worktree/zlink-nsend/framework/languages/node
python3 -c "from pathlib import Path; s=Path('packages/framework/src/runtime/backend/node/node-raw-binding-port.ts').read_text(); assert 'const part = parts[index]!.copy();' in s, 'apply nsend/prototype.diff before building this variant'"
node /root/jobs/fx/cx/v21/s033/nsend/check-binding-source.cjs
node node_modules/typescript/bin/tsc -b tsconfig.build.json
node scripts/prepare_browser_build.mjs
node scripts/esbuild-bundle.mjs packages/framework-codec-msgpack/src/framework.ts --bundle --format=cjs --platform=node --target=node18 --external:@zlink-systems/framework --external:@zlink-systems/stream-connector --external:@zlink-systems/stream-wire --outfile=packages/framework-codec-msgpack/dist/server/framework.cjs
node scripts/esbuild-bundle.mjs packages/framework-codec-protobuf/src/framework.ts --bundle --format=cjs --platform=node --target=node18 --external:@zlink-systems/framework --external:@zlink-systems/stream-connector --external:@zlink-systems/stream-wire --outfile=packages/framework-codec-protobuf/dist/server/framework.cjs
node --test --test-concurrency=1 test/contract/node-raw-binding-send-admission.test.js test/contract/node-raw-receive-storage-cost.test.js test/contract/node-multipart-string-buffer.test.js test/contract/raw-mesh-send-results.test.js test/contract/send-public-surface.test.js test/contract/encoded-payload-storage-cost.test.js
node --test --test-name-pattern='framework protobuf envelope deserialization isolates returned byte subarrays' test/contract/stream-connector-codecs.test.js
python3 /root/jobs/fx/cx/v21/s033/nsend/negative-storage-check.py
cd /root/worktree/zlink-nsend/framework/bench/grpc/node
NODE_PATH=/root/worktree/zlink-nsend/framework/languages/node/node_modules npm test
cd /root/worktree/zlink-nsend
node framework/languages/node/node_modules/prettier/bin/prettier.cjs --check framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts
git diff --check
