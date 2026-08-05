const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

test('ST-F1 and ST-F3 compare packet values and require source cleanup evidence', () => {
  const f1 = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/SpotActorTransfer/Client/Scenarios/st-f1-packet-order-scenario.ts'
  ), 'utf8');
  const f3 = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/SpotActorTransfer/Client/Scenarios/st-f3-request-order-scenario.ts'
  ), 'utf8');
  const runtime = fs.readFileSync(path.join(
    nodeRoot,
    'packages/framework/src/runtime/host/actor-transfer-runtime.ts'
  ), 'utf8');
  assert.match(f1, /assertValuesInOrder\([^;]+\['P1', 'P2', 'P3'\]/s);
  assert.match(f3, /assertValuesInOrder\([^;]+\['S1', 'S2', 'S3', 'S4'\]/s);
  assert.match(f1, /source_cleanup/);
  assert.match(runtime, /onSourceDepartureCompleted/);
});
