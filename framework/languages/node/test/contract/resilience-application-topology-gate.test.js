import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');
const resilience = path.join(root, 'e2e/ResilienceLifecycle');

test('resilience topology evidence belongs to the consumer application runtime', () => {
  assert.equal(fs.existsSync(path.join(resilience, 'Server/TopologyProbe/package.json')), false);
  assert.equal(fs.existsSync(path.join(resilience, 'Server/TopologyProbe/main.ts')), false);

  const host = fs.readFileSync(path.join(resilience, 'Server/Consumer/consumer-host-factory.ts'), 'utf8');
  const endpoints = fs.readFileSync(path.join(resilience, 'Server/Consumer/Endpoints/consumer-endpoints.ts'), 'utf8');
  const runner = fs.readFileSync(path.join(resilience, 'run_e2e.sh'), 'utf8');

  assert.match(host, /ZLINK_LOCATION_RUNTIME_QUERY/);
  assert.match(endpoints, /listMeshNodeDescriptors/);
  assert.match(endpoints, /path: '\/location\/peers'/);
  assert.doesNotMatch(endpoints, /state:\s*ZLinkLocationTopologyState\.Ready/);
  assert.match(runner, /--peer-location-url "http:\/\/127\.0\.0\.1:\$CONSUMER_HTTP_PORT"/);
  assert.doesNotMatch(runner, /TopologyProbe|topology-probe-main|TOPOLOGY_PROBE_HTTP_PORT/);
});
