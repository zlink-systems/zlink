const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('MON-A1 keeps Host and RouteMesh status sources separate', () => {
  const scenario = fs.readFileSync(path.join(
    root,
    'e2e/RuntimeMonitoring/Client/Scenarios/mon-a1-socket-events-scenario.ts'
  ), 'utf8');

  assert.match(scenario, /readHostStatus\(options\.serviceUrl\)/);
  assert.match(scenario, /readRouteStatus\(options\.serviceUrl\)/);
  assert.match(scenario, /waitForRouteStatus\(/);
  assert.match(scenario, /BigInt\(routeAfter\.sequence\) > BigInt\(routeBefore\.sequence\)/);
  assert.match(scenario, /BigInt\(hostAfter\.sequence\) >= BigInt\(hostBefore\.sequence\)/);
  assert.match(scenario, /JSON\.stringify\(routeBefore\) !== JSON\.stringify\(routeAfter\)/);
  assert.match(scenario, /routeStatusesFromEvidence/);
  assert.match(scenario, /hostStatusesFromEvidence/);
});
