const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const sampleRoot = path.resolve(__dirname, '../../samples/ZoneWorld');

test('ZoneWorld C2 observes graceful termination separately from abrupt B4 and C3', () => {
  const runner = fs.readFileSync(path.join(sampleRoot, 'Runner/sample-runner.mjs'), 'utf8');
  const client = fs.readFileSync(path.join(sampleRoot, 'Client/special.ts'), 'utf8');

  assert.match(runner, /specialClientConfig\(ctx, shared, gateway, ops, 'B4-C3'\)/);
  assert.match(
    runner,
    /await transition\.waitFor\('scenario ZW-B4-C3 armed'\);\s*await ctx\.stop\(targetNode\.nodeId, 'SIGKILL'\)/
  );
  assert.match(
    runner,
    /specialClientConfig\(ctx, shared, gateway, ops, 'C2', sourceNode\.nodeId\)/
  );
  assert.match(
    runner,
    /await c2\.waitFor\('scenario ZW-C2 armed'\);\s*await ctx\.stop\(sourceNode\.nodeId, 'SIGTERM'\)/
  );
  assert.match(client, /scenario === 'C2'/);
  assert.match(client, /scenario ZW-C2 armed/);
  assert.match(client, /scenario ZW-C2 passed/);
});
