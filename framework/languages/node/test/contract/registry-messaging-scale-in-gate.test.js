const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('RM-B2 keeps consumer traffic active while provider B exits', () => {
  const scenario = fs.readFileSync(path.join(
    root,
    'e2e/RegistryMessaging/Client/Scenarios/rm-b2-scale-in-scenario.ts'
  ), 'utf8');

  assert.match(scenario, /cluster\.startConsumer/);
  assert.match(scenario, /const firstDuring = postJson<ProfileRes>\(consumer\.httpUrl/);
  assert.match(scenario, /const draining = cluster\.drain\(providerB\)/);
  assert.match(scenario, /drainResult\.outcome === 0[\s\S]*drainResult\.reason === 0/);
  assert.match(scenario, /during\.every\(\(reply\) => reply\.providerRid === 'api-a' \|\| reply\.providerRid === 'api-b'\)/);
  assert.match(scenario, /rid: 'api-b', present: false/);
  assert.match(scenario, /cluster\.waitForSingleProvider\('api-a', providerA\.channelEndpoint\)/);
  assert.doesNotMatch(scenario, /postJson<ProfileRes>\(providerA\.httpUrl/);
  assert.doesNotMatch(scenario, /setTimeout\(resolve, 1000\)/);
  assert.doesNotMatch(scenario, /readEvidenceIgnoringStopped/);
});
