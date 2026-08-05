const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('RM-A4 keeps one consumer across provider endpoint replacement', () => {
  const scenario = fs.readFileSync(path.join(
    root,
    'e2e/RegistryMessaging/Client/Scenarios/rm-a4-same-rid-failover-scenario.ts'
  ), 'utf8');

  assert.match(scenario, /cluster\.startConsumer/);
  assert.match(scenario, /postJson<ProfileRes>\(consumer\.httpUrl/);
  assert.match(scenario, /cluster\.waitForSingleProvider\('api-a', providerV2\.channelEndpoint\)/);
  assert.doesNotMatch(scenario, /postJson<ProfileRes>\(providerV[12]\.httpUrl/);
  assert.doesNotMatch(scenario, /readEvidenceIgnoringStopped/);
});
