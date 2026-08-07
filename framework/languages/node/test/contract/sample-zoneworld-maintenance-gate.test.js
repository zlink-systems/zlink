const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function source(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, 'samples/ZoneWorld', relativePath), 'utf8');
}

test('ZoneWorld maintenance keeps route work observable without per-tick success logging', () => {
  const actor = source('Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts');
  const ops = source('Server/Ops/ops-handlers.ts');

  assert.doesNotMatch(actor, /actor push submitted/);
  assert.match(actor, /actor push failed/);
  assert.match(ops, /\.timeout\(10_000\)/);
  assert.match(ops, /maintenance apply failed/);
});

test('ZoneWorld starts status observation only after maintenance apply succeeds', () => {
  const client = source('Client/special.ts');
  const response = client.indexOf('const response = await ops.request(new SetMaintenanceReq');
  const accepted = client.indexOf('response.error === null', response);
  const observed = client.indexOf('const observed = ops.waitFor<NodeStatusNotify>', accepted);

  assert.ok(response >= 0);
  assert.ok(accepted > response);
  assert.ok(observed > accepted);
});
