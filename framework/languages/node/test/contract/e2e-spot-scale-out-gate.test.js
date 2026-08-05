const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('SM-G2 adds the second SpotNode only after the existing owners are established', () => {
  const runner = fs.readFileSync(path.join(root, 'e2e/SpotService/run_e2e.sh'), 'utf8');
  const scenario = fs.readFileSync(
    path.join(root, 'e2e/SpotService/Client/Scenarios/sm-g2-scenario.ts'),
    'utf8'
  );
  const endpoints = fs.readFileSync(
    path.join(root, 'e2e/SpotService/Server/MultiNode/Endpoints/multi-node-endpoints.ts'),
    'utf8'
  );

  assert.match(runner, /SM-G2\)\s+SERVER_ROLES=\(multi-node-a\)/);
  assert.match(runner, /run_client SM-G2-PREPARE[\s\S]*start_named_server multi-node-b[\s\S]*run_client SM-G2-VERIFY/);
  assert.match(endpoints, /\/scale-out\/readiness\/wait/);
  assert.match(scenario, /existingOwner[\s\S]*multiSpotNodeA/);
  assert.match(scenario, /newOwner[\s\S]*multiSpotNodeB/);
});
