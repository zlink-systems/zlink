#!/usr/bin/env bash
set -euo pipefail
cd /root/worktree/zlink-nsend/framework/languages/node
node scripts/prepare_browser_build.mjs
node scripts/esbuild-bundle.mjs packages/framework-codec-msgpack/src/framework.ts --bundle --format=cjs --platform=node --target=node18 --external:@zlink-systems/framework --external:@zlink-systems/stream-connector --external:@zlink-systems/stream-wire --outfile=packages/framework-codec-msgpack/dist/server/framework.cjs
node scripts/esbuild-bundle.mjs packages/framework-codec-protobuf/src/framework.ts --bundle --format=cjs --platform=node --target=node18 --external:@zlink-systems/framework --external:@zlink-systems/stream-connector --external:@zlink-systems/stream-wire --outfile=packages/framework-codec-protobuf/dist/server/framework.cjs
python3 -c "from pathlib import Path; import hashlib; p=Path('packages/framework-codec-protobuf/dist/server/framework.cjs'); expected=Path('/root/jobs/fx/cx/v21/s033/nsend/baseline-protobuf-bundle.sha256').read_text().strip(); assert hashlib.sha256(p.read_bytes()).hexdigest()==expected, 'protobuf build changed from baseline'"
node --test --test-name-pattern='framework protobuf envelope deserialization isolates returned byte subarrays' test/contract/stream-connector-codecs.test.js
python3 /root/jobs/fx/cx/v21/s033/nsend/negative-storage-check.py
cd /root/worktree/zlink-nsend/framework/bench/grpc/node
NODE_PATH=/root/worktree/zlink-nsend/framework/languages/node/node_modules npm test
cd /root/worktree/zlink-nsend
node framework/languages/node/node_modules/prettier/bin/prettier.cjs --check framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts
git diff --check
