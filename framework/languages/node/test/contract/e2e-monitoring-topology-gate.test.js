const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const scenarioPath = path.resolve(
  __dirname,
  '../../e2e/RuntimeMonitoring/Client/Scenarios/mon-a2-location-runtime-events-scenario.ts'
);
const managedServicePath = path.resolve(
  __dirname,
  '../../e2e/RuntimeMonitoring/Client/Support/managed-service.ts'
);

test('MON-A2 changes provider membership and compares public status projections', () => {
  const source = fs.readFileSync(scenarioPath, 'utf8');
  assert.match(source, /serviceBUrl, '\/shutdown'/);
  assert.match(source, /startServiceB\(options,/);
  const managedService = fs.readFileSync(managedServicePath, 'utf8');
  assert.match(managedService, /options\.filteredServiceMain/);
  assert.match(source, /waitForRouteStatus\(/);
  assert.match(source, /'\/locations\/peers'/);
  assert.match(source, /removed\.readyPeerCount/);
  assert.match(source, /status\.readyPeerCount > removed\.readyPeerCount/);
  assert.match(source, /BigInt\(restored\.sequence\) > BigInt\(baseline\.sequence\)/);
  assert.match(source, /routeStatusesFromEvidence/);
  assert.match(source, /status\.peers\.every/);
  assert.match(source, /status\.peers\.some/);
});
